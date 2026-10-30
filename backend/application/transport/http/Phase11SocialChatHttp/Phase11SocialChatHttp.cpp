#include "backend/application/transport/http/Phase11SocialChatHttp/Phase11SocialChatHttp.h"
namespace webserver::phase11::transport {
void Phase11SocialChatHttp::registerRoutes(Router &router) {
    contacts_.registerRoutes(router);
    conversations_.registerRoutes(router);
    messageHistory_.registerRoutes(router);
    groupGovernance_.registerRoutes(router);
}
}
