#include "backend/application/modules/ai/application/AiChatService/AiChatService.h"
#include "backend/application/storage/InMemoryStore/InMemoryStore.h"
#include "backend/application/modules/shared/domain/ApplicationError/ApplicationError.h"
#include <stdexcept>
using namespace webserver::phase11;
struct Sink final:IAiGenerationEventSink {
    void token(const AiTokenEvent&)override{}
    void completed(const AiCompletedEvent&)override{}
};
int main(){
    InMemoryStore store;ChatService chat(store,store,store,store,store,store,store);
    ModelRouter router(store);DatabaseExecutor executor(1,8);Sink sink;
    AiChatService service(chat,store,store,router,executor,store,sink);
    bool rejected=false;
    try{service.generate({});}catch(const ApplicationError&){rejected=true;}
    if(!rejected||service.snapshot().active)throw std::runtime_error("invalid admission entered executor");
    service.shutdown();service.shutdown();executor.shutdown();
}
