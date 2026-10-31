#include "backend/application/bootstrap/ContactsRuntime/ContactsRuntime.h"
#include "backend/application/bootstrap/BusinessRuntime/BusinessRuntime.h"
#include "backend/application/bootstrap/BootstrapSettings/BootstrapSettings.h"

#include "backend/server/http/RequestContext/RequestContext.h"
#include "backend/server/Runtime/ServerRuntime.h"
#include <iostream>
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
#include "backend/application/bootstrap/StorageRuntime/StorageRuntime.h"
#include "backend/application/bootstrap/IdentityRuntime/IdentityRuntime.h"
#include "backend/application/bootstrap/ChatRuntime/ChatRuntime.h"
#include "backend/ai/bootstrap/KnowledgeRuntime/KnowledgeRuntime.h"

#include "backend/application/bootstrap/FileRuntime/FileRuntime.h"
#include "backend/application/bootstrap/RealtimeRuntime/RealtimeRuntime.h"
#include "backend/ai/bootstrap/AiRuntime/AiRuntime.h"
#include "backend/ai/models/DeterministicKnowledgeIndex/DeterministicKnowledgeIndex.h"
#include "backend/application/storage/postgres/PostgresStore/PostgresStore.h"
#include "backend/application/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"
#include "backend/application/modules/files/application/FileService/FileService.h"
#include "backend/application/modules/contacts/application/SocialService/SocialService.h"
#include "backend/application/modules/bootstrap/Function3Runtime/Function3Runtime.h"
#include "backend/application/runtime/BusinessShutdown/BusinessShutdown.h"
#include "backend/application/observability/BusinessMetricsFormatter/BusinessMetricsFormatter.h"
#endif
namespace webserver::bootstrap {
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
struct BusinessRuntime::Impl {
    // Dependency order also protects partial construction: borrowed resources
    // precede their users; shutdown drains producers before their repositories.
    StorageRuntime storage;
    IdentityRuntime identity;
    ContactsRuntime contacts;
    ChatRuntime chat;
    FileRuntime files;
    KnowledgeRuntime knowledge;
    AiRuntime ai;
    std::unique_ptr<function3::Function3Runtime> community;
    std::unique_ptr<RealtimeRuntime> realtime;
    application::BusinessShutdown shutdown;
    Impl(ServerRuntime&net,bool prod):storage(config::RuntimeConfig::environment("WEB_PHASE11_DATABASE"),flag("WEB_FUNCTION3_ENABLE")),
      identity(storage,prod),contacts(storage),chat(storage),files(storage),knowledge(storage,identity),ai(storage,identity,chat,knowledge,net,prod){
        if(flag("WEB_FUNCTION3_ENABLE"))community=std::make_unique<function3::Function3Runtime>(storage.connection(),identity.http(),storage.executor(),net.sseManager(),files.service(),contacts.service(),net.wsManager(),storage.store(),ai.registry(),ai.evaluations(),[index=&knowledge.index()](std::uint64_t id){index->removeDocument(id);});
        realtime=std::make_unique<RealtimeRuntime>(storage,contacts,chat,ai,net,community.get());
        contacts.bind(identity,net,realtime->notifier());
        chat.bind(identity,net,realtime->notifier());files.bind(identity,chat,realtime->notifier());
        shutdown.add("presence",[this]{realtime->detachPresence();});
        shutdown.add("private generations",[this]{ai.cancelPrivate();});
        shutdown.add("AI orchestration",[this]{ai.stop();});
        shutdown.add("file tasks",[this]{files.drain();});
        shutdown.add("knowledge tasks",[this]{knowledge.drain();});
        shutdown.add("accepted database tasks",[this]{storage.drain();});
        shutdown.add("outbox",[this]{realtime->stop();});
        shutdown.add("community workers",[this]{if(community)community->stop();});
    }
    ~Impl(){shutdown.stop();}
};
#else
struct BusinessRuntime::Impl {};
#endif
BusinessRuntime::BusinessRuntime(ServerRuntime&net,bool production){
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
    if(!config::RuntimeConfig::environment("WEB_PHASE11_DATABASE").empty()){
        impl_=std::make_unique<Impl>(net,production);
        std::cout<<"Phase 11 authentication enabled\n";
        return;
    }
#else
    (void)net;(void)production;
#endif
    if(flag("WEB_PHASE11_REQUIRED"))throw std::invalid_argument("WEB_PHASE11_REQUIRED needs compiled drivers and database/token-key configuration");
}
BusinessRuntime::~BusinessRuntime(){stop();}
bool BusinessRuntime::enabled()const noexcept{return bool(impl_);}
phase11::transport::Phase11AuthHttp* BusinessRuntime::authentication()const noexcept{
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
    return impl_?&impl_->identity.http():nullptr;
#else
    return nullptr;
#endif
}
bool BusinessRuntime::ready()const{
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
    return !impl_||impl_->storage.store().healthy();
#else
    return true;
#endif
}
std::string BusinessRuntime::metrics()const{
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
    if(impl_)return phase11::formatBusinessMetrics(&impl_->storage.store(),impl_->storage.deliveryMetrics(),&impl_->storage.executor(),&impl_->files.executor(),&impl_->knowledge.executor(),&impl_->realtime->pump(),&impl_->ai.chat(),impl_->ai.resilient());
#endif
    return {};
}
void BusinessRuntime::registerRoutes(Router&r){
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
    if(impl_){impl_->identity.http().registerRoutes(r);impl_->ai.registerRoutes(r);impl_->contacts.registerRoutes(r);impl_->chat.registerRoutes(r);impl_->files.registerRoutes(r);impl_->knowledge.registerRoutes(r);if(impl_->community)impl_->community->registerRoutes(r);return;}
#endif
    const auto unavailable=[](RequestContext&ctx){ctx.response->status=503;ctx.response->statusText="Service Unavailable";ctx.response->json("{\"error\":\"phase11 authentication is not configured\"}");ctx.response->setHeader("Cache-Control","no-store");return true;};
    r.POST("/api/auth/register",unavailable);r.POST("/api/auth/login",unavailable);r.POST("/api/auth/logout",unavailable);r.GET("/api/me",unavailable);r.PATCH("/api/me",unavailable);
}
void BusinessRuntime::stop()noexcept{
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
    if(impl_)impl_->shutdown.stop();
#endif
}
bool BusinessRuntime::stoppedCleanly()const noexcept{
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
    return !impl_ || impl_->shutdown.failures()==0;
#else
    return true;
#endif
}
}
