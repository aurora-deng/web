#pragma once
#include "backend/application/modules/contacts/application/UserDirectoryService/UserDirectoryService.h"
#include "backend/application/modules/chat/ports/IWorkspaceRepository/IWorkspaceRepository.h"
#include "backend/application/ports/Ports/Ports.h"
#include "backend/application/modules/chat/application/WorkspaceRequests/WorkspaceRequests.h"
#include <utility>
namespace webserver::chat {
    /** 聊天工作区用例：搜索、偏好和上下文；消息提交/ACK仍复用原聊天业务。 */
    class ChatWorkspaceService final {
        public:
        ChatWorkspaceService(IDatabase&db,IUserRepository&users,IConversationRepository&conversations,IMessageRepository&messages,IOutboxRepository&outbox,IWorkspaceRepository&workspace)
        :directory_(users,workspace),db_(db),users_(users),conversations_(conversations),messages_(messages),outbox_(outbox),workspace_(workspace){
        }
        UserPage users(UserId actor,UserDirectoryQuery query)const;
        MessagePage search(UserId actor,MessageSearchQuery query)const;
        MessageContext context(UserId actor,MessageContextQuery query)const;
        ConversationPreference setPreferences(UserId actor,SetConversationPreference command,TimePoint now);
        // Compatibility overloads keep existing clients/contracts source-compatible.
        UserPage users(UserId actor,std::string query,UserId after,std::size_t limit)const{return users(actor,{std::move(query),after,limit});}
        MessagePage search(UserId actor,ConversationId conversation,std::string query,MessageId before,std::size_t limit)const{return search(actor,{conversation,std::move(query),before,limit});}
        ConversationPreference preferences(UserId actor,ConversationId conversation)const;
        ConversationPreference setPreferences(UserId actor,ConversationId conversation,bool muted,TimePoint now){return setPreferences(actor,{conversation,muted},now);}
        MessageContext context(UserId actor,ConversationId conversation,MessageId message,std::size_t radius)const{return context(actor,{conversation,message,radius});}
        private:
        void active(UserId actor)const;
        void member(UserId actor,ConversationId conversation)const;
        contacts::UserDirectoryService directory_;
        IDatabase&db_;
        IUserRepository&users_;
        IConversationRepository&conversations_;
        IMessageRepository&messages_;
        IOutboxRepository&outbox_;
        IWorkspaceRepository&workspace_;
    };
}
