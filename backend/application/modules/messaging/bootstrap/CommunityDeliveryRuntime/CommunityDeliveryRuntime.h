#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class Router;
class SseSessionManager;
namespace webserver::phase11 {
class BoundedExecutor;
class IMessageRepository;
class IOutboxEventSink;
namespace transport { class Phase11AuthHttp; }
}
namespace webserver::function3 {
class IActorDirectory;
class IEventWriter;
class SqlContext;

/** Owns the durable event writer, broker consumer, publisher and replay routes. */
class CommunityDeliveryRuntime final {
public:
    using SpeechJob = std::function<void(std::uint64_t, std::string, std::int64_t)>;

    CommunityDeliveryRuntime(SqlContext&, IActorDirectory&,
                             phase11::transport::Phase11AuthHttp&, phase11::BoundedExecutor&);
    ~CommunityDeliveryRuntime();
    CommunityDeliveryRuntime(const CommunityDeliveryRuntime&) = delete;
    CommunityDeliveryRuntime& operator=(const CommunityDeliveryRuntime&) = delete;

    IEventWriter& writer() noexcept;
    std::function<void()> commitNotifier() const;
    void registerRoutes(Router&);

    // The composition root stops this runtime before releasing either callback consumer.
    phase11::IOutboxEventSink& start(phase11::IOutboxEventSink&, phase11::IMessageRepository&,
                                    SseSessionManager&, SpeechJob);
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
