#include "backend/application/modules/activities/infrastructure/ActivityPostgresViews/ActivityPostgresViews.h"
#include "backend/application/modules/activities/transport/ActivityHttp/ActivityHttp.h"
#include "backend/application/modules/cache/infrastructure/RedisReadCache/RedisReadCache.h"
#include "backend/application/modules/messaging/infrastructure/LocalEventBroker/LocalEventBroker.h"
#include "backend/application/modules/messaging/transport/MessagingHttp/MessagingHttp.h"
#include "Function3Runtime.h"
#include "backend/application/modules/assistant/infrastructure/LegacyArchivePostgres/LegacyArchivePostgres.h"
#include "backend/application/modules/assistant/transport/PrivateArchiveHttp/PrivateArchiveHttp.h"
#include "backend/application/modules/assistant/infrastructure/AssistantModelsPostgres/AssistantModelsPostgres.h"
#include "backend/application/modules/assistant/transport/AssistantModelsHttp/AssistantModelsHttp.h"
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
#include "backend/application/modules/shared/infrastructure/Phase11Bridge/Phase11Bridge.h"
#include "backend/application/modules/tree/infrastructure/TreePostgresRepository/TreePostgresRepository.h"
#include "backend/application/modules/tree/infrastructure/TreeRankingSource/TreeRankingSource.h"
#include "backend/application/modules/ranking/infrastructure/RankingPostgresRepository/RankingPostgresRepository.h"
#include "backend/application/modules/announcement/infrastructure/AnnouncementPostgresRepository/AnnouncementPostgresRepository.h"
#include "backend/application/modules/admin/infrastructure/AdminPostgresRepository/AdminPostgresRepository.h"
#include "backend/application/modules/moderation/infrastructure/ReportPostgresRepository/ReportPostgresRepository.h"
#include "backend/application/modules/messaging/infrastructure/PostgresEventWriter/PostgresEventWriter.h"
#include "backend/application/modules/messaging/infrastructure/PostgresEventPipeline/PostgresEventPipeline.h"
#include "backend/server/sse/SseSessionManager.h"
#include "backend/server/websocket/WebSocketSessionManager/WebSocketSessionManager.h"
#ifdef WEBSERVER_HAS_ROCKETMQ
#include "backend/application/modules/messaging/infrastructure/RocketMqBroker/RocketMqBroker.h"
#endif
#include "backend/application/modules/tree/transport/TreeHttp/TreeHttp.h"
#include "backend/application/modules/tree/transport/TreeMediaHttp/TreeMediaHttp.h"
#include "backend/application/modules/tree/infrastructure/FfmpegMediaProcessor/FfmpegMediaProcessor.h"
#include "backend/application/modules/tree/infrastructure/TreeMediaPostgresRepository/TreeMediaPostgresRepository.h"
#include "backend/application/modules/admin/transport/AdminHttp/AdminHttp.h"
#include "backend/application/modules/admin/transport/ModelAdminHttp/ModelAdminHttp.h"
#include "backend/application/modules/admin/infrastructure/Phase11ModelAdministration/Phase11ModelAdministration.h"
#include "backend/application/modules/announcement/transport/AnnouncementHttp/AnnouncementHttp.h"
#include "backend/ai/speech/infrastructure/SpeechRuntime/SpeechRuntime.h"
#include "backend/ai/speech/transport/SpeechHttp/SpeechHttp.h"
#include "backend/ai/speech/transport/SpeechAdminHttp/SpeechAdminHttp.h"
#include <condition_variable>
#include <thread>
#include <cstdlib>
namespace webserver::function3 {
struct Function3Runtime::Impl {
    struct Signal {std::mutex mutex;std::condition_variable wake;std::uint64_t generation{};};
    std::shared_ptr<Signal>signal=std::make_shared<Signal>();
    std::unique_ptr<IQueryCache> cache;
    SqlContext sql;
    const bool schemaReady;
    Phase11Bridge bridge;
    PostgresEventWriter events;
    TreePostgresRepository treeRepository;
    TreeRankingSource rankSource;
    RankingPostgresRepository rankRepository;
    AnnouncementPostgresRepository announcementRepository;
    AdminPostgresRepository adminRepository;
    ReportPostgresRepository reportRepository;
    TreeService tree;
    AnnouncementService announcements;
    RankingService ranking;
    AdminService admin;
    Phase11ModelAdministration modelBridge;
    ModelAdminService modelAdmin;
    phase11::BoundedExecutor modelExecutor{1,4};
    ModelAdminHttp modelHttp;
    ModerationService moderation;
    SpeechRuntime speech;
    TreeHttp treeHttp;
    ActivityPostgresViews activityViews;ActivityService activities;ActivityHttp activityHttp;
    AdminHttp adminHttp;
    AnnouncementHttp announcementHttp;
    SpeechHttp speechHttp;
    phase11::BoundedExecutor previewExecutor{1,4};
    SpeechAdminHttp speechAdminHttp;
    phase11::BoundedExecutor mediaExecutor{1,16};
    FfmpegMediaProcessor mediaProcessor;
    TreeMediaPostgresRepository mediaRepository;
    TreeMediaService mediaService;
    TreeMediaHttp mediaHttp;
    PostgresEventPipeline pipeline;
    MessagingService messaging;
    MessagingHttp messagingHttp;
    LegacyArchivePostgres privateArchiveRepository;
    PrivateArchiveService privateArchiveService;
    PrivateArchiveHttp privateArchiveHttp;
    AssistantModelsPostgres assistantModelsRepository;
    PrivateModelDirectory privateModelDirectory;
    AssistantModelsHttp assistantModelsHttp;
    SseSessionManager &sse;
    phase11::transport::Phase11AuthHttp &auth;phase11::BoundedExecutor &httpExecutor;
    std::unique_ptr<IEventBroker> broker;
    std::unique_ptr<ChatMqSink> chatMq;
    std::mutex&mutex=signal->mutex;
    std::condition_variable&wake=signal->wake;
    std::atomic<bool> stopping{};
    std::uint64_t&publishGeneration=signal->generation;
    void signalPublisher(){ {std::lock_guard lock(mutex);++publishGeneration;}wake.notify_all(); }
    std::thread worker;
    std::thread publisher;
    Impl(std::string connection,phase11::transport::Phase11AuthHttp&auth,phase11::BoundedExecutor&executor,SseSessionManager&sessions,phase11::IFileBusiness&files,phase11::ISocialBusiness&social,WebSocketSessionManager&websockets,
         phase11::IModelRepository&models,phase11::ModelRegistryService&registry,phase11::ModelEvaluationService&evaluation,std::function<void(Id)>invalidatePrivateKnowledge)
        :sql({std::move(connection),4,std::chrono::milliseconds(2000)}),schemaReady(requireSchema(sql)),bridge(sql),events(sql,[this]{signalPublisher();}),treeRepository(sql),rankSource(sql),rankRepository(sql),announcementRepository(sql),adminRepository(sql),reportRepository(sql),
         tree(sql,bridge,treeRepository,bridge,events,&treeRepository),announcements(sql,bridge,announcementRepository,events),ranking(sql,rankRepository,rankSource,announcements),admin(sql,bridge,adminRepository),modelBridge(models,registry,evaluation),modelAdmin(bridge,modelBridge,sql,events),modelHttp(auth,modelExecutor,modelAdmin),moderation(sql,bridge,treeRepository,reportRepository,events),
         speech(sql,events),treeHttp(auth,executor,tree,moderation),activityViews(sql),activities(sql,bridge,tree,activityViews,activityViews),activityHttp(auth,executor,activities,social),adminHttp(auth,executor,admin,moderation,tree,[&sessions,&websockets](Id user){websockets.closeUser(user);sessions.closeClient(user);}),announcementHttp(auth,executor,announcements,ranking),speechHttp(auth,executor,announcements,speech.service()),speechAdminHttp(auth,previewExecutor,bridge,speech.service()),
         mediaProcessor(std::getenv("WEB_FUNCTION3_FFMPEG_ROOT")?std::getenv("WEB_FUNCTION3_FFMPEG_ROOT"):"/home/pikachu/phase11-deps/ffmpeg/8.1.3/install",
             std::getenv("WEB_FUNCTION3_MEDIA_ROOT")?std::getenv("WEB_FUNCTION3_MEDIA_ROOT"):"/home/pikachu/phase11-data/function3/media"),mediaRepository(sql),mediaService(tree,files,sql,mediaProcessor,mediaRepository),mediaHttp(auth,mediaExecutor,mediaService),pipeline(sql),messaging(sql,bridge,pipeline,events),messagingHttp(auth,executor,messaging),privateArchiveRepository(sql,[this]{signalPublisher();}),privateArchiveService(bridge,privateArchiveRepository,std::move(invalidatePrivateKnowledge)),privateArchiveHttp(auth,executor,privateArchiveService),assistantModelsRepository(sql),privateModelDirectory(assistantModelsRepository),assistantModelsHttp(auth,executor,privateModelDirectory),sse(sessions),auth(auth),httpExecutor(executor) {
        if(const char*port=std::getenv("WEB_FUNCTION3_REDIS_PORT")){RedisCacheConfig c;c.port=std::stoul(port);if(const char*prefix=std::getenv("WEB_FUNCTION3_REDIS_PREFIX"))c.prefix=prefix;if(const char*password=std::getenv("WEB_FUNCTION3_REDIS_PASSWORD"))c.password=password;cache=std::make_unique<RedisReadCache>(std::move(c));treeHttp.setCache(cache.get());announcementHttp.setCache(cache.get());}
        worker=std::thread([this]{std::unique_lock lock(mutex);while(!stopping){lock.unlock();try{if(ranking.settle()&&cache)cache->invalidate("ranking");}catch(const std::exception&){/* 本轮失败不丢事件，下轮由唯一 slot 约束安全重试。 */}lock.lock();wake.wait_for(lock,std::chrono::seconds(5),[&]{return stopping.load();});}});
    }
    static bool requireSchema(SqlContext&sql){
        if(sql.query("SELECT 1 FROM phase11_schema_migrations WHERE version=23").empty())throw std::runtime_error("Function3 requires database migrations through 023; no automatic migrations performed");
        return true; // 在任何后台 Worker 创建之前检查，失败启动不会领取任务或结算榜单。
    }
    ~Impl(){{std::lock_guard lock(mutex);stopping=true;}wake.notify_all();speech.stop();if(worker.joinable())worker.join();if(publisher.joinable())publisher.join();broker.reset();mediaExecutor.shutdown();modelExecutor.shutdown();previewExecutor.shutdown();}
    phase11::IOutboxEventSink &startMessaging(phase11::IOutboxEventSink &realtime,phase11::IMessageRepository &messages){
        const char *endpoint=std::getenv("WEB_FUNCTION3_MQ_ENDPOINT");const bool external=endpoint&&*endpoint;
        if(broker)throw std::logic_error("messaging already started");
        BrokerConsumer consumer=[this,&realtime](const BrokerMessage&m){
            return pipeline.consume(m,[&](std::string type,std::string envelope){
                if(type=="chat.legacy"){
                    auto outer=phase11::transport::parseFlatStringObject(envelope);
                    auto f=phase11::transport::parseFlatStringObject(outer.at("payload"));
                    phase11::OutboxEvent e;e.id=std::stoull(f.at("id"));e.type=f.at("type");e.aggregateType=f.at("aggregate");e.aggregateId=std::stoull(f.at("aggregateId"));e.payload=f.at("payload");
                    (void)realtime.publish(e);
                }else if(type.starts_with("tree.")||type.starts_with("announcement.")){
                    SseEvent event;event.eventName=type;event.id="function3:"+phase11::transport::parseFlatStringObject(envelope).at("id");event.data=envelope;(void)sse.broadcast(event);
                }
            },[this](const auto&f){
                if(f.at("type")=="speech.requested"){
                    const auto p=phase11::transport::parseFlatStringObject(f.at("payload"));speech.enqueue(std::stoull(p.at("announcementId")),p.at("text"),std::stoll(p.at("expiresAt")));
                }
            });
        };
        if(external){
#ifdef WEBSERVER_HAS_ROCKETMQ
            broker=std::make_unique<RocketMqBroker>(RocketMqConfig{endpoint},std::move(consumer));
            chatMq=std::make_unique<ChatMqSink>(sql,messages,[this]{signalPublisher();});
#else
            throw std::runtime_error("WEB_FUNCTION3_MQ_ENDPOINT requires WEBSERVER_ENABLE_ROCKETMQ");
#endif
        }else broker=std::make_unique<LocalEventBroker>(std::move(consumer));
        publisher=std::thread([this]{std::unique_lock lock(mutex);while(!stopping){const auto observed=publishGeneration;lock.unlock();bool busy=false;try{for(int i=0;i<4&&!stopping;++i){if(!pipeline.publishBatch(*broker))break;busy=true;}}catch(...){}lock.lock();wake.wait_for(lock,std::chrono::milliseconds(busy?10:250),[&]{return stopping.load()||publishGeneration!=observed;});}});
        return chatMq?static_cast<phase11::IOutboxEventSink&>(*chatMq):realtime;
    }
};
Function3Runtime::Function3Runtime(std::string connection,phase11::transport::Phase11AuthHttp&auth,phase11::BoundedExecutor&executor,SseSessionManager&sse,phase11::IFileBusiness&files,phase11::ISocialBusiness&social,WebSocketSessionManager&websockets,
    phase11::IModelRepository&models,phase11::ModelRegistryService&registry,phase11::ModelEvaluationService&evaluation,std::function<void(Id)>invalidatePrivateKnowledge)
    :impl_(std::make_unique<Impl>(std::move(connection),auth,executor,sse,files,social,websockets,models,registry,evaluation,std::move(invalidatePrivateKnowledge))){}
Function3Runtime::~Function3Runtime()=default;
std::function<void()>Function3Runtime::commitNotifier()const{auto signal=impl_->signal;return[signal]{{std::lock_guard lock(signal->mutex);++signal->generation;}signal->wake.notify_all();};}
void Function3Runtime::registerRoutes(Router &router){
    impl_->privateArchiveHttp.registerRoutes(router);
    impl_->assistantModelsHttp.registerRoutes(router);
    {auto scope=http::executor(router,impl_->httpExecutor);
    router.GET("/api/me/speech/status",[this](RequestContext&ctx){return http::guarded(ctx,[&]{impl_->auth.authorizeRequest(ctx);const char*w=std::getenv("WEB_FUNCTION3_TTS_WORKER");const char*mq=std::getenv("WEB_FUNCTION3_MQ_ENDPOINT");http::reply(ctx,"{\"enabled\":"+std::string(w&&*w?"true":"false")+",\"delivery\":"+http::quote(mq&&*mq?"rocketmq":"local")+"}");});});
    router.GET("/api/admin/cache",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto user=impl_->auth.authorizeRequest(ctx).user.id;if(activeActor(impl_->bridge,user).role!=Role::Administrator)throw BusinessError(Failure::Forbidden,"administrator required");auto m=impl_->cache?impl_->cache->metrics():CacheMetrics{};http::reply(ctx,"{\"enabled\":"+std::string(impl_->cache?"true":"false")+",\"hits\":"+std::to_string(m.hits)+",\"misses\":"+std::to_string(m.misses)+",\"errors\":"+std::to_string(m.errors)+",\"bypassed\":"+std::to_string(m.bypassed)+"}");});});}
impl_->activityHttp.registerRoutes(router);impl_->treeHttp.registerRoutes(router);impl_->adminHttp.registerRoutes(router);impl_->modelHttp.registerRoutes(router);impl_->announcementHttp.registerRoutes(router);impl_->mediaHttp.registerRoutes(router);impl_->speechHttp.registerRoutes(router);impl_->speechAdminHttp.registerRoutes(router);impl_->messagingHttp.registerRoutes(router);}
phase11::IOutboxEventSink&Function3Runtime::startMessaging(phase11::IOutboxEventSink&sink,phase11::IMessageRepository&messages){return impl_->startMessaging(sink,messages);}
}
