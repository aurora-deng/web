#include "backend/ai/bootstrap/AiRuntime/AiRuntime.h"
#include "backend/application/bootstrap/BootstrapSettings/BootstrapSettings.h"
#include "backend/application/bootstrap/StorageRuntime/StorageRuntime.h"
#include "backend/application/bootstrap/IdentityRuntime/IdentityRuntime.h"
#include "backend/application/bootstrap/ChatRuntime/ChatRuntime.h"
#include "backend/ai/bootstrap/KnowledgeRuntime/KnowledgeRuntime.h"

#include "backend/application/storage/postgres/PostgresStore/PostgresStore.h"
#include "backend/application/business/AiChatService/AiChatService.h"
#include "backend/application/modules/chat/application/ChatService/ChatService.h"
#include "backend/application/transport/realtime/Phase11Realtime/Phase11Realtime.h"
#include "backend/ai/bootstrap/ModelProviderAssembly/ModelProviderAssembly.h"
#include "backend/ai/models/ModelEvaluationService/ModelEvaluationService.h"
#include "backend/ai/models/RagService/RagService.h"
#include "backend/ai/private/infrastructure/PrivateModelRouter/PrivateModelRouter.h"
#include "backend/ai/private/transport/PrivateGenerationHttp/PrivateGenerationHttp.h"
#include "backend/server/Runtime/ServerRuntime.h"
namespace webserver::bootstrap {
namespace {
auto chatOptions(std::size_t maxChunks){
    webserver::phase11::AiChatServiceConfig aiChatConfig;
    aiChatConfig.maxActiveGenerations = size(
        "WEB_PHASE11_AI_MAX_ACTIVE", 64, 1, 65'536);
    aiChatConfig.maxActivePerUser = size(
        "WEB_PHASE11_AI_MAX_PER_USER", 2, 1, 1024);
    aiChatConfig.maxActivePerAiAccount = size(
        "WEB_PHASE11_AI_MAX_PER_ACCOUNT", 16, 1, 65'536);
    aiChatConfig.generationTimeout = std::chrono::milliseconds(
        size("WEB_PHASE11_AI_DEADLINE_MS",
               330'000, 100, 3'600'000));
    aiChatConfig.ragMaxChunks = size(
        "WEB_PHASE11_AI_RAG_MAX_CHUNKS", 6, 1,
        maxChunks);
    return aiChatConfig;
}
struct PrivateEndpoint {
    phase11::PrivateGenerationService tasks;
    phase11::transport::PrivateGenerationHttp http;
    PrivateEndpoint(phase11::PrivateModelRouter&m,IdentityRuntime&i,StorageRuntime&s,bool web)
    :tasks(m),http(i.http(),s.executor(),tasks,web){}
};
}
struct AiRuntime::Impl {
    phase11::ModelEvaluationService evaluations;
    phase11::ModelRegistryService registry;
    phase11::ModelRouter router;
    std::vector<std::shared_ptr<phase11::IModelProvider>> providers;
    std::vector<std::shared_ptr<phase11::ResilientModelProvider>> resilient;
    phase11::transport::Phase11AiEventSink events;
    std::unique_ptr<phase11::AiChatService> chat;
    phase11::PrivateModelRouter privateModel;
    std::unique_ptr<PrivateEndpoint> privateEndpoint,webEndpoint;
    Impl(StorageRuntime&s,IdentityRuntime&i,ChatRuntime&c,KnowledgeRuntime&k,ServerRuntime&net,bool prod)
    :evaluations(s.store(),s.store()),registry(s.store(),&evaluations),router(s.store()),events(net.sseManager()),privateModel(router,s.store()){
        phase11::configureModelProviders(router,registry,providers,resilient,prod);
        chat=std::make_unique<phase11::AiChatService>(c.chat(),s.store(),s.store(),router,s.executor(),s.store(),events,chatOptions(k.maxChunks()),&k.rag());
        privateEndpoint=std::make_unique<PrivateEndpoint>(privateModel,i,s,false);
        webEndpoint=std::make_unique<PrivateEndpoint>(privateModel,i,s,true);
    }
};
AiRuntime::AiRuntime(StorageRuntime&s,IdentityRuntime&i,ChatRuntime&c,KnowledgeRuntime&k,ServerRuntime&n,bool p):impl_(std::make_unique<Impl>(s,i,c,k,n,p)){}
AiRuntime::~AiRuntime(){cancelPrivate();stop();}
phase11::ModelRegistryService& AiRuntime::registry(){return impl_->registry;}
phase11::ModelEvaluationService& AiRuntime::evaluations(){return impl_->evaluations;}
phase11::AiChatService& AiRuntime::chat(){return *impl_->chat;}
const std::vector<std::shared_ptr<phase11::ResilientModelProvider>>& AiRuntime::resilient()const{return impl_->resilient;}
void AiRuntime::registerRoutes(Router&r){impl_->privateEndpoint->http.registerRoutes(r);impl_->webEndpoint->http.registerRoutes(r);}
void AiRuntime::cancelPrivate()noexcept{impl_->privateEndpoint.reset();impl_->webEndpoint.reset();}
void AiRuntime::stop()noexcept{if(impl_->chat)impl_->chat->shutdown();}
}
