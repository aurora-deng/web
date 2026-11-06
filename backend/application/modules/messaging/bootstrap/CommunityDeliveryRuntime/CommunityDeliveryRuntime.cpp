#include "CommunityDeliveryRuntime.h"
#include "backend/application/modules/messaging/infrastructure/LocalEventBroker/LocalEventBroker.h"
#include "backend/application/modules/messaging/infrastructure/PostgresEventPipeline/PostgresEventPipeline.h"
#include "backend/application/modules/messaging/infrastructure/PostgresEventWriter/PostgresEventWriter.h"
#include "backend/application/modules/messaging/transport/MessagingHttp/MessagingHttp.h"
#include "backend/server/sse/SseSessionManager.h"
#ifdef WEBSERVER_HAS_ROCKETMQ
#include "backend/application/modules/messaging/infrastructure/RocketMqBroker/RocketMqBroker.h"
#endif

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace webserver::function3 {
struct CommunityDeliveryRuntime::Impl {
    struct Signal {
        std::mutex mutex;
        std::condition_variable wake;
        std::uint64_t generation{};
        void notify() {
            { std::lock_guard lock(mutex); ++generation; }
            wake.notify_one();
        }
    };

    SqlContext& sql;
    std::shared_ptr<Signal> signal = std::make_shared<Signal>();
    PostgresEventWriter events;
    PostgresEventPipeline pipeline;
    MessagingService service;
    MessagingHttp http;
    std::unique_ptr<IEventBroker> broker;
    std::unique_ptr<ChatMqSink> chatSink;
    std::atomic<bool> stopping{};
    std::thread publisher;

    Impl(SqlContext& db, IActorDirectory& actors, phase11::transport::Phase11AuthHttp& auth,
         phase11::BoundedExecutor& executor)
        : sql(db), events(sql, notifier()), pipeline(sql),
          service(sql, actors, pipeline, events), http(auth, executor, service) {}

    std::function<void()> notifier() const {
        // A transaction can finish after shutdown; this callback owns only the signal.
        return [signal = signal] { signal->notify(); };
    }

    void publish() {
        std::unique_lock lock(signal->mutex);
        while (!stopping) {
            const auto observed = signal->generation;
            lock.unlock();
            bool busy = false;
            try {
                for (int i = 0; i < 4 && !stopping; ++i) {
                    if (!pipeline.publishBatch(*broker)) break;
                    busy = true;
                }
            } catch (...) {
                // Durable rows remain eligible for the next bounded poll.
            }
            lock.lock();
            signal->wake.wait_for(lock, std::chrono::milliseconds(busy ? 10 : 250),
                [&] { return stopping || signal->generation != observed; });
        }
    }

    void stop() noexcept {
        { std::lock_guard lock(signal->mutex); if (stopping.exchange(true)) return; }
        signal->wake.notify_all();
        if (publisher.joinable()) publisher.join();
        broker.reset();
    }
};

CommunityDeliveryRuntime::CommunityDeliveryRuntime(
        SqlContext& sql, IActorDirectory& actors, phase11::transport::Phase11AuthHttp& auth,
        phase11::BoundedExecutor& executor)
    : impl_(std::make_unique<Impl>(sql, actors, auth, executor)) {}
CommunityDeliveryRuntime::~CommunityDeliveryRuntime() { stop(); }
IEventWriter& CommunityDeliveryRuntime::writer() noexcept { return impl_->events; }
std::function<void()> CommunityDeliveryRuntime::commitNotifier() const { return impl_->notifier(); }
void CommunityDeliveryRuntime::registerRoutes(Router& router) { impl_->http.registerRoutes(router); }
void CommunityDeliveryRuntime::stop() noexcept { impl_->stop(); }

phase11::IOutboxEventSink& CommunityDeliveryRuntime::start(
        phase11::IOutboxEventSink& realtime, phase11::IMessageRepository& messages,
        SseSessionManager& sse, SpeechJob enqueueSpeech) {
    auto& impl = *impl_;
    if (impl.broker || impl.stopping) throw std::logic_error("delivery is already started or stopped");
    const char* endpoint = std::getenv("WEB_FUNCTION3_MQ_ENDPOINT");
    BrokerConsumer consumer = [pipeline = &impl.pipeline, &realtime, &sse,
                               enqueueSpeech = std::move(enqueueSpeech)](const BrokerMessage& message) {
        return pipeline->consume(message, [&](std::string type, std::string envelope) {
            if (type == "chat.legacy") {
                const auto outer = phase11::transport::parseFlatStringObject(envelope);
                const auto fields = phase11::transport::parseFlatStringObject(outer.at("payload"));
                phase11::OutboxEvent event;
                event.id = std::stoull(fields.at("id"));
                event.type = fields.at("type");
                event.aggregateType = fields.at("aggregate");
                event.aggregateId = std::stoull(fields.at("aggregateId"));
                event.payload = fields.at("payload");
                (void)realtime.publish(event);
            } else if (type.starts_with("tree.") || type.starts_with("announcement.")) {
                SseEvent event;
                event.eventName = type;
                event.id = "function3:" + phase11::transport::parseFlatStringObject(envelope).at("id");
                event.data = std::move(envelope);
                (void)sse.broadcast(event);
            }
        }, [&](const auto& fields) {
            if (fields.at("type") == "speech.requested") {
                const auto payload = phase11::transport::parseFlatStringObject(fields.at("payload"));
                enqueueSpeech(std::stoull(payload.at("announcementId")), payload.at("text"),
                              std::stoll(payload.at("expiresAt")));
            }
        });
    };
    if (endpoint && *endpoint) {
#ifdef WEBSERVER_HAS_ROCKETMQ
        impl.broker = std::make_unique<RocketMqBroker>(RocketMqConfig{endpoint}, std::move(consumer));
        impl.chatSink = std::make_unique<ChatMqSink>(impl.sql, messages, impl.notifier());
#else
        (void)messages;
        throw std::runtime_error("WEB_FUNCTION3_MQ_ENDPOINT requires WEBSERVER_ENABLE_ROCKETMQ");
#endif
    } else {
        impl.broker = std::make_unique<LocalEventBroker>(std::move(consumer));
    }
    impl.publisher = std::thread([state = &impl] { state->publish(); });
    return impl.chatSink ? static_cast<phase11::IOutboxEventSink&>(*impl.chatSink) : realtime;
}
}
