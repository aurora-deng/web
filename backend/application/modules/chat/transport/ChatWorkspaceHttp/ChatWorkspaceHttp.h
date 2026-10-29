#pragma once
#include "backend/application/modules/chat/application/ChatWorkspaceService/ChatWorkspaceService.h"
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
namespace webserver::chat {
    class ChatWorkspaceHttp final {
        public:
        ChatWorkspaceHttp(phase11::transport::Phase11AuthHttp&auth,phase11::DatabaseExecutor&executor,ChatWorkspaceService&service,std::function<void()>committed={
        }
        )
        :auth_(auth),executor_(executor),service_(service),committed_(std::move(committed)){
        }
        void registerRoutes(Router&router);
        private:phase11::transport::Phase11AuthHttp&auth_;
        phase11::DatabaseExecutor&executor_;
        ChatWorkspaceService&service_;
        std::function<void()>committed_;
    };
}
