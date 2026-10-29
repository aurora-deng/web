#pragma once
#include "backend/application/storage/postgres/PostgresSql/PostgresSql.h"
#include "backend/application/domain/Domain/Domain.h"
namespace webserver::phase11 {
    /** Read repository delegated to by PostgresStore; preserves one leased transaction. */
    class PostgresMessageRepository final {
        public:
        static std::string columns(std::string prefix={
        }
        );
        static std::vector<Message>hydrate(PGconn*connection,const pg::Result&rows);
        static std::vector<Message>after(PGconn*,ConversationId,std::uint64_t,std::size_t);
        static std::vector<Message>before(PGconn*,ConversationId,std::uint64_t,std::size_t);
        static std::vector<Message>search(PGconn*,ConversationId,std::string_view,std::size_t);
        static std::optional<Message>own(PGconn*,ConversationId,UserId,std::string_view);
        static std::optional<Message>find(PGconn*,MessageId);
    };
}
