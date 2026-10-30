#pragma once
#include "backend/application/transport/http/ContactsHttp/ContactsHttp.h"
#include "backend/application/transport/http/ConversationsHttp/ConversationsHttp.h"
#include "backend/application/transport/http/MessageHistoryHttp/MessageHistoryHttp.h"
#include "backend/application/transport/http/GroupGovernanceHttp/GroupGovernanceHttp.h"
namespace webserver::phase11::transport {
/** Compatibility composition root. Domain adapters own handlers and budgets. */
class Phase11SocialChatHttp final {
public:
    Phase11SocialChatHttp(Phase11AuthHttp &authentication,ISocialBusiness &social,IChatBusiness &chat,
        DatabaseExecutor &databaseExecutor,WebSocketSessionManager &webSockets,std::function<void()>outboxCommitted={})
        :contacts_(authentication,social,databaseExecutor,webSockets,outboxCommitted),
         conversations_(authentication,chat,databaseExecutor,outboxCommitted),
         messageHistory_(authentication,chat,databaseExecutor,outboxCommitted),
         groupGovernance_(authentication,chat,databaseExecutor,outboxCommitted){}
    void registerRoutes(Router &router);
private:
    ContactsHttp contacts_;
    ConversationsHttp conversations_;
    MessageHistoryHttp messageHistory_;
    GroupGovernanceHttp groupGovernance_;
};
}
