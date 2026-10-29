#pragma once
#include "backend/application/modules/chat/ports/IWorkspaceRepository/IWorkspaceRepository.h"
#include "backend/application/storage/postgres/PostgresSql/PostgresSql.h"
namespace webserver::phase11 {
    /** Driver helper: every call receives the facade's already leased connection. */
    class PostgresWorkspaceRepository final {
        public:
        static std::vector<chat::UserCard>users(PGconn*,UserId,std::string_view,UserId,std::size_t);
        static std::vector<Message>search(PGconn*,UserId,ConversationId,std::string_view,MessageId,std::size_t);
        static chat::ConversationPreference preference(PGconn*,UserId,ConversationId);
        static chat::PreferenceWrite writePreference(PGconn*,UserId,ConversationId,bool);
    };
}
