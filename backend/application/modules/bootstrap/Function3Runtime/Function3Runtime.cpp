#include "Function3Runtime.h"
#include "backend/application/modules/bootstrap/CommunityContentRuntime/CommunityContentRuntime.h"
#include "backend/application/modules/messaging/bootstrap/CommunityDeliveryRuntime/CommunityDeliveryRuntime.h"
#include "backend/application/modules/admin/infrastructure/AdminPostgresRepository/AdminPostgresRepository.h"
#include "backend/application/modules/admin/infrastructure/Phase11ModelAdministration/Phase11ModelAdministration.h"
#include "backend/application/modules/admin/transport/AdminHttp/AdminHttp.h"
#include "backend/application/modules/admin/transport/ModelAdminHttp/ModelAdminHttp.h"
#include "backend/application/modules/announcement/infrastructure/AnnouncementPostgresRepository/AnnouncementPostgresRepository.h"
#include "backend/application/modules/announcement/transport/AnnouncementHttp/AnnouncementHttp.h"
#include "backend/application/modules/assistant/infrastructure/AssistantModelsPostgres/AssistantModelsPostgres.h"
#include "backend/application/modules/assistant/infrastructure/LegacyArchivePostgres/LegacyArchivePostgres.h"
#include "backend/application/modules/assistant/transport/AssistantModelsHttp/AssistantModelsHttp.h"
#include "backend/application/modules/assistant/transport/PrivateArchiveHttp/PrivateArchiveHttp.h"
#include "backend/application/modules/cache/infrastructure/RedisReadCache/RedisReadCache.h"
#include "backend/application/modules/ranking/infrastructure/RankingPostgresRepository/RankingPostgresRepository.h"
#include "backend/application/modules/shared/infrastructure/Phase11Bridge/Phase11Bridge.h"
#include "backend/application/modules/tree/infrastructure/TreeRankingSource/TreeRankingSource.h"
#include "backend/ai/speech/infrastructure/SpeechRuntime/SpeechRuntime.h"
#include "backend/ai/speech/transport/SpeechAdminHttp/SpeechAdminHttp.h"
#include "backend/ai/speech/transport/SpeechHttp/SpeechHttp.h"
#include "backend/server/sse/SseSessionManager.h"
#include "backend/server/websocket/WebSocketSessionManager/WebSocketSessionManager.h"

#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace webserver::function3 {
namespace {
bool requireSchema(SqlContext& sql) {
    if (sql.query("SELECT 1 WHERE (SELECT count(*) FROM phase11_schema_migrations WHERE version BETWEEN 1 AND 24)=24").empty())
        throw std::runtime_error("Function3 requires database migrations through 024; no automatic migrations performed");
    return true;
}

std::unique_ptr<IQueryCache> buildCache() {
    const char* port = std::getenv("WEB_FUNCTION3_REDIS_PORT");
    if (!port) return {};
    RedisCacheConfig config;
    config.port = std::stoul(port);
    if (const char* prefix = std::getenv("WEB_FUNCTION3_REDIS_PREFIX")) config.prefix = prefix;
    if (const char* password = std::getenv("WEB_FUNCTION3_REDIS_PASSWORD")) config.password = password;
    return std::make_unique<RedisReadCache>(std::move(config));
}
}

struct Function3Runtime::Impl {
    SqlContext sql;
    // Must run before constructing services that launch workers.
    const bool schemaReady;
    Phase11Bridge bridge;
    std::unique_ptr<IQueryCache> cache;
    CommunityDeliveryRuntime delivery;
    CommunityContentRuntime content;

    TreeRankingSource rankSource;
    RankingPostgresRepository rankRepository;
    AnnouncementPostgresRepository announcementRepository;
    AnnouncementService announcements;
    RankingService ranking;
    AnnouncementHttp announcementHttp;

    AdminPostgresRepository adminRepository;
    AdminService admin;
    AdminHttp adminHttp;
    Phase11ModelAdministration modelBridge;
    ModelAdminService modelAdmin;
    phase11::BoundedExecutor modelExecutor{1, 4};
    ModelAdminHttp modelHttp;

    SpeechRuntime speech;
    SpeechHttp speechHttp;
    phase11::BoundedExecutor previewExecutor{1, 4};
    SpeechAdminHttp speechAdminHttp;

    LegacyArchivePostgres privateArchiveRepository;
    PrivateArchiveService privateArchiveService;
    PrivateArchiveHttp privateArchiveHttp;
    AssistantModelsPostgres assistantModelsRepository;
    PrivateModelDirectory privateModelDirectory;
    AssistantModelsHttp assistantModelsHttp;

    SseSessionManager& sse;
    phase11::transport::Phase11AuthHttp& auth;
    phase11::BoundedExecutor& httpExecutor;
    std::mutex rankMutex;
    std::condition_variable rankWake;
    bool stopping{};
    std::thread rankWorker;

    Impl(std::string connection, phase11::transport::Phase11AuthHttp& authorization,
         phase11::BoundedExecutor& executor, SseSessionManager& sessions,
         phase11::IFileBusiness& files, phase11::ISocialBusiness& social,
         WebSocketSessionManager& websockets, phase11::IModelRepository& models,
         phase11::ModelRegistryService& registry, phase11::ModelEvaluationService& evaluation,
         std::function<void(Id)> invalidatePrivateKnowledge)
        : sql({std::move(connection), 4, std::chrono::milliseconds(2000)}),
          schemaReady(requireSchema(sql)), bridge(sql), cache(buildCache()),
          delivery(sql, bridge, authorization, executor),
          content(sql, bridge, delivery.writer(), authorization, executor, files, social),
          rankSource(sql), rankRepository(sql), announcementRepository(sql),
          announcements(sql, bridge, announcementRepository, delivery.writer()),
          ranking(sql, rankRepository, rankSource, announcements),
          announcementHttp(authorization, executor, announcements, ranking),
          adminRepository(sql), admin(sql, bridge, adminRepository),
          adminHttp(authorization, executor, admin, content.moderation(), content.tree(),
                    [&sessions, &websockets](Id user){websockets.closeUser(user);sessions.closeClient(user);}),
          modelBridge(models, registry, evaluation), modelAdmin(bridge, modelBridge, sql, delivery.writer()),
          modelHttp(authorization, modelExecutor, modelAdmin),
          speech(sql, delivery.writer()), speechHttp(authorization, executor, announcements, speech.service()),
          speechAdminHttp(authorization, previewExecutor, bridge, speech.service()),
          privateArchiveRepository(sql, delivery.commitNotifier()),
          privateArchiveService(bridge, privateArchiveRepository, std::move(invalidatePrivateKnowledge)),
          privateArchiveHttp(authorization, executor, privateArchiveService),
          assistantModelsRepository(sql), privateModelDirectory(assistantModelsRepository),
          assistantModelsHttp(authorization, executor, privateModelDirectory),
          sse(sessions), auth(authorization), httpExecutor(executor) {
        content.setCache(cache.get());
        announcementHttp.setCache(cache.get());
        rankWorker = std::thread([this] {
            std::unique_lock lock(rankMutex);
            while (!stopping) {
                lock.unlock();
                try { if (ranking.settle() && cache) cache->invalidate("ranking"); }
                catch (const std::exception&) { /* The next slot can retry safely. */ }
                lock.lock();
                rankWake.wait_for(lock, std::chrono::seconds(5), [this]{return stopping;});
            }
        });
    }

    void stop() noexcept {
        { std::lock_guard lock(rankMutex); if (stopping) return; stopping = true; }
        rankWake.notify_one();
        if (rankWorker.joinable()) rankWorker.join();
        content.stop();
        modelExecutor.shutdown();
        previewExecutor.shutdown();
        // Join broker callbacks before stopping their speech destination.
        delivery.stop();
        speech.stop();
    }
    ~Impl() { stop(); }
};

Function3Runtime::Function3Runtime(
        std::string connection, phase11::transport::Phase11AuthHttp& auth,
        phase11::BoundedExecutor& executor, SseSessionManager& sse, phase11::IFileBusiness& files,
        phase11::ISocialBusiness& social, WebSocketSessionManager& websockets,
        phase11::IModelRepository& models, phase11::ModelRegistryService& registry,
        phase11::ModelEvaluationService& evaluation, std::function<void(Id)> invalidatePrivateKnowledge)
    : impl_(std::make_unique<Impl>(std::move(connection), auth, executor, sse, files, social,
                                   websockets, models, registry, evaluation,
                                   std::move(invalidatePrivateKnowledge))) {}
Function3Runtime::~Function3Runtime() = default;
void Function3Runtime::stop() noexcept { impl_->stop(); }
void Function3Runtime::stopMessaging() noexcept { impl_->delivery.stop(); }
std::function<void()> Function3Runtime::commitNotifier() const { return impl_->delivery.commitNotifier(); }

void Function3Runtime::registerRoutes(Router& router) {
    impl_->privateArchiveHttp.registerRoutes(router);
    impl_->assistantModelsHttp.registerRoutes(router);
    {
        auto scope = http::executor(router, impl_->httpExecutor);
        router.GET("/api/me/speech/status", [this](RequestContext& ctx) {
            return http::guarded(ctx, [&] {
                impl_->auth.authorizeRequest(ctx);
                const char* worker = std::getenv("WEB_FUNCTION3_TTS_WORKER");
                const char* mq = std::getenv("WEB_FUNCTION3_MQ_ENDPOINT");
                http::reply(ctx, "{\"enabled\":" + std::string(worker && *worker ? "true" : "false") +
                                 ",\"delivery\":" + http::quote(mq && *mq ? "rocketmq" : "local") + "}");
            });
        });
        router.GET("/api/admin/cache", [this](RequestContext& ctx) {
            return http::guarded(ctx, [&] {
                auto user = impl_->auth.authorizeRequest(ctx).user.id;
                if (activeActor(impl_->bridge, user).role != Role::Administrator)
                    throw BusinessError(Failure::Forbidden, "administrator required");
                auto metrics = impl_->cache ? impl_->cache->metrics() : CacheMetrics{};
                http::reply(ctx, "{\"enabled\":" + std::string(impl_->cache ? "true" : "false") +
                                 ",\"hits\":" + std::to_string(metrics.hits) +
                                 ",\"misses\":" + std::to_string(metrics.misses) +
                                 ",\"errors\":" + std::to_string(metrics.errors) +
                                 ",\"bypassed\":" + std::to_string(metrics.bypassed) + "}");
            });
        });
    }
    impl_->content.registerRoutes(router);
    impl_->adminHttp.registerRoutes(router);
    impl_->modelHttp.registerRoutes(router);
    impl_->announcementHttp.registerRoutes(router);
    impl_->speechHttp.registerRoutes(router);
    impl_->speechAdminHttp.registerRoutes(router);
    impl_->delivery.registerRoutes(router);
}

phase11::IOutboxEventSink& Function3Runtime::startMessaging(
        phase11::IOutboxEventSink& sink, phase11::IMessageRepository& messages) {
    return impl_->delivery.start(sink, messages, impl_->sse,
        [speech = &impl_->speech](Id announcement, std::string text, Milliseconds expires) {
            speech->enqueue(announcement, std::move(text), expires);
        });
}
}
