#include "backend/application/bootstrap/ChatRuntime/ChatRuntime.h"
#include "backend/application/bootstrap/StorageRuntime/StorageRuntime.h"
#include "backend/application/bootstrap/IdentityRuntime/IdentityRuntime.h"

#include "backend/application/storage/postgres/PostgresStore/PostgresStore.h"
#include "backend/application/modules/chat/application/ChatService/ChatService.h"
#include "backend/application/modules/chat/transport/ConversationsHttp/ConversationsHttp.h"
#include "backend/application/modules/chat/transport/MessageHistoryHttp/MessageHistoryHttp.h"
#include "backend/application/modules/chat/transport/GroupGovernanceHttp/GroupGovernanceHttp.h"
#include "backend/application/modules/chat/bootstrap/ChatWorkspaceModule/ChatWorkspaceModule.h"
#include "backend/server/Runtime/ServerRuntime.h"
namespace webserver::bootstrap {
struct ChatRuntime::Impl {
    struct Transport {
        phase11::transport::ConversationsHttp conversations;
        phase11::transport::MessageHistoryHttp messages;
        phase11::transport::GroupGovernanceHttp groups;
        webserver::chat::ChatWorkspaceModule workspace;
        Transport(StorageRuntime&s,IdentityRuntime&i,ServerRuntime&,phase11::ChatService&chat,std::function<void()> committed)
        :conversations(i.http(),chat,s.executor(),committed),messages(i.http(),chat,s.executor(),committed),groups(i.http(),chat,s.executor(),committed),
         workspace(i.http(),s.executor(),s.store(),s.store(),s.store(),s.store(),s.store(),s.store(),std::move(committed)){}
    };
    StorageRuntime& storage;
    phase11::ChatService chat;
    std::unique_ptr<Transport> transport;
    explicit Impl(StorageRuntime&s):storage(s),
      chat(s.store(),s.store(),s.store(),s.store(),s.store(),s.store(),s.store()){}
};
ChatRuntime::ChatRuntime(StorageRuntime&s):impl_(std::make_unique<Impl>(s)){}
ChatRuntime::~ChatRuntime()=default;
phase11::ChatService& ChatRuntime::chat(){return impl_->chat;}
void ChatRuntime::bind(IdentityRuntime&i,ServerRuntime&n,std::function<void()>c){if(impl_->transport)throw std::logic_error("chat transport already bound");impl_->transport=std::make_unique<Impl::Transport>(impl_->storage,i,n,impl_->chat,std::move(c));}
void ChatRuntime::registerRoutes(Router&r){if(!impl_->transport)throw std::logic_error("chat transport not bound");impl_->transport->conversations.registerRoutes(r);impl_->transport->messages.registerRoutes(r);impl_->transport->groups.registerRoutes(r);impl_->transport->workspace.registerRoutes(r);}
}
