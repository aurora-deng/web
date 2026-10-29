#pragma once
#include "backend/application/modules/chat/domain/WorkspaceTypes/WorkspaceTypes.h"
namespace webserver::chat {
    /** 值对象只含公开资料/消息，业务端口不暴露 SQL 或网络连接。 */
    class IWorkspaceRepository {
        public:
        virtual ~IWorkspaceRepository()=default;
        virtual std::vector<UserCard>searchUsers(UserId viewer,std::string_view prefix,UserId after,std::size_t limit)const=0;
        virtual std::vector<Message>searchVisibleMessages(UserId viewer,ConversationId conversation,std::string_view query,MessageId before,std::size_t limit)const=0;
        virtual ConversationPreference preference(UserId user,ConversationId conversation)const=0;
        virtual PreferenceWrite writePreference(UserId user,ConversationId conversation,bool muted)=0;
    };
}
