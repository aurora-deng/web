#include "backend/ai/bootstrap/KnowledgeRuntime/KnowledgeRuntime.h"
#include "backend/application/bootstrap/BootstrapSettings/BootstrapSettings.h"
#include "backend/application/bootstrap/StorageRuntime/StorageRuntime.h"
#include "backend/application/bootstrap/IdentityRuntime/IdentityRuntime.h"

#include "backend/application/storage/postgres/PostgresStore/PostgresStore.h"
#include "backend/ai/models/RagService/RagService.h"
#include "backend/ai/models/DeterministicKnowledgeIndex/DeterministicKnowledgeIndex.h"
#include "backend/ai/training/TrainingDataService/TrainingDataService.h"
#include "backend/application/transport/http/Phase11KnowledgeHttp/Phase11KnowledgeHttp.h"
#include "backend/application/transport/http/Phase11TrainingHttp/Phase11TrainingHttp.h"
namespace webserver::bootstrap {
namespace {
auto ragOptions(){
    webserver::phase11::RagServiceConfig ragConfig;
    ragConfig.maxDocumentBytes = size(
        "WEB_PHASE11_RAG_DOCUMENT_BYTES", 2 * 1024 * 1024,
        1024, 16 * 1024 * 1024);
    ragConfig.maxChunkBytes = size(
        "WEB_PHASE11_RAG_CHUNK_BYTES", 1200, 256, 8192);
    ragConfig.overlapBytes = size(
        "WEB_PHASE11_RAG_OVERLAP_BYTES", 160, 0, 4095);
    ragConfig.maxRetrievedChunks = size(
        "WEB_PHASE11_RAG_MAX_CHUNKS", 8, 1, 32);
    return ragConfig;
}
auto httpOptions(std::size_t bytes){phase11::transport::Phase11KnowledgeHttpConfig c;c.maximumRequestBytes=bytes*2+64*1024;return c;}
}
struct KnowledgeRuntime::Impl {
    phase11::RagServiceConfig config=ragOptions();
    phase11::DeterministicKnowledgeIndex index;
    phase11::RagService rag;
    phase11::TrainingDataService training;
    phase11::BoundedExecutor executor;
    phase11::transport::Phase11KnowledgeHttp http;
    phase11::transport::Phase11TrainingHttp trainingHttp;
    Impl(StorageRuntime&s,IdentityRuntime&i):rag(s.store(),s.store(),s.store(),index,config),
      training(s.store(),s.store(),s.store(),s.store(),s.store()),
      executor(size("WEB_PHASE11_RAG_WORKERS",2,1,16),size("WEB_PHASE11_RAG_QUEUE",32,1,4096)),
      http(i.http(),rag,executor,httpOptions(config.maxDocumentBytes)),trainingHttp(i.http(),training,s.executor()){}
};
KnowledgeRuntime::KnowledgeRuntime(StorageRuntime&s,IdentityRuntime&i):impl_(std::make_unique<Impl>(s,i)){}
KnowledgeRuntime::~KnowledgeRuntime(){drain();}
phase11::RagService& KnowledgeRuntime::rag(){return impl_->rag;}
phase11::DeterministicKnowledgeIndex& KnowledgeRuntime::index(){return impl_->index;}
phase11::BoundedExecutor& KnowledgeRuntime::executor(){return impl_->executor;}
std::size_t KnowledgeRuntime::maxChunks()const{return impl_->config.maxRetrievedChunks;}
void KnowledgeRuntime::registerRoutes(Router&r){impl_->http.registerRoutes(r);impl_->trainingHttp.registerRoutes(r);}
void KnowledgeRuntime::drain()noexcept{impl_->executor.shutdown();}
}
