#pragma once
#include "backend/application/ports/IConversationRepository/IConversationRepository.h"
#include "backend/application/ports/IDatabase/IDatabase.h"
#include "backend/application/ports/IMessageRepository/IMessageRepository.h"
#include "backend/application/ports/IOutboxRepository/IOutboxRepository.h"
#include "backend/application/ports/IUserRepository/IUserRepository.h"
#include "backend/application/modules/chat/transport/ChatWorkspaceHttp/ChatWorkspaceHttp.h"
namespace webserver::chat {
    /** Domain composition root: drivers are injected, neither routes nor main duplicate use-case rules. */
    class ChatWorkspaceModule final {
        public:
        ChatWorkspaceModule(phase11::transport::Phase11AuthHttp& auth,
        phase11::DatabaseExecutor& executor, IDatabase& db, IUserRepository& users,
        IConversationRepository& conversations, IMessageRepository& messages,
        IOutboxRepository& outbox, IWorkspaceRepository& workspace, std::function<void()> committed)
        : service_(db,users,conversations,messages,outbox,workspace),
        http_(auth,executor,service_,std::move(committed)) {
        }
        void registerRoutes(Router& router) {
            http_.registerRoutes(router);
        }
        private:
        ChatWorkspaceService service_;
        ChatWorkspaceHttp http_;
    };
}
