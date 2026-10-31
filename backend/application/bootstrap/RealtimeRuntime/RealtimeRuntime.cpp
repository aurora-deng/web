#include "backend/application/bootstrap/ContactsRuntime/ContactsRuntime.h"
#include "backend/application/bootstrap/RealtimeRuntime/RealtimeRuntime.h"
#include "backend/application/bootstrap/BootstrapSettings/BootstrapSettings.h"
#include "backend/application/bootstrap/StorageRuntime/StorageRuntime.h"
#include "backend/application/bootstrap/ChatRuntime/ChatRuntime.h"

#include "backend/ai/bootstrap/AiRuntime/AiRuntime.h"
#include "backend/application/storage/postgres/PostgresStore/PostgresStore.h"
#include "backend/application/modules/contacts/application/SocialService/SocialService.h"
#include "backend/application/modules/chat/application/ChatService/ChatService.h"
#include "backend/application/modules/bootstrap/Function3Runtime/Function3Runtime.h"
#include "backend/application/modules/messaging/infrastructure/ChatOutboxWriter/ChatOutboxWriter.h"
#include "backend/application/transport/realtime/Phase11Realtime/Phase11Realtime.h"
#include "backend/server/Runtime/ServerRuntime.h"
namespace webserver::bootstrap {
struct RealtimeRuntime::Impl {
    phase11::transport::Phase11RealtimeOutboxSink sink;
    std::unique_ptr<phase11::transport::Phase11OutboxPump> pump;
    std::unique_ptr<phase11::transport::Phase11Realtime> commands;
    std::unique_ptr<phase11::transport::Phase11PresenceBridge> presence;
    bool notifyEnabled=!flag("WEB_PHASE11_OUTBOX_POLL_ONLY");
    std::function<void()> notifier(){return [this]{if(notifyEnabled)pump->notifyCommitted();};}
    Impl(StorageRuntime&s,ContactsRuntime&contacts,ChatRuntime&c,AiRuntime&a,ServerRuntime&net,function3::Function3Runtime*community)
    :sink(s.delivery(),s.delivery(),s.delivery(),net.wsManager(),net.sseManager()){
        const char* endpoint=std::getenv("WEB_FUNCTION3_MQ_ENDPOINT");
        if(community&&endpoint&&*endpoint)s.store().setOutboxWriter(std::make_shared<function3::ChatOutboxWriter>(community->commitNotifier()));
        auto& destination=community?community->startMessaging(sink,s.delivery()):static_cast<phase11::IOutboxEventSink&>(sink);
        pump=std::make_unique<phase11::transport::Phase11OutboxPump>(s.store(),destination);
        commands=std::make_unique<phase11::transport::Phase11Realtime>(c.chat(),s.executor(),&a.chat(),notifier());
        commands->registerHandlers(net.wsDispatcher());
        presence=std::make_unique<phase11::transport::Phase11PresenceBridge>(contacts.service(),s.executor(),net.wsManager(),notifier());
    }
};
RealtimeRuntime::RealtimeRuntime(StorageRuntime&s,ContactsRuntime&contacts,ChatRuntime&c,AiRuntime&a,ServerRuntime&n,function3::Function3Runtime*f):impl_(std::make_unique<Impl>(s,contacts,c,a,n,f)){}
RealtimeRuntime::~RealtimeRuntime(){detachPresence();stop();}
std::function<void()> RealtimeRuntime::notifier(){return impl_->notifier();}
phase11::transport::Phase11OutboxPump& RealtimeRuntime::pump(){return *impl_->pump;}
void RealtimeRuntime::detachPresence()noexcept{impl_->presence.reset();}
void RealtimeRuntime::stop()noexcept{if(impl_->pump)impl_->pump->stop();}
}
