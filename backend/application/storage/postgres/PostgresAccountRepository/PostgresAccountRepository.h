#pragma once
#include "backend/application/domain/AccountTypes/AccountTypes.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
#include "backend/application/storage/postgres/PostgresOutboxWriter/PostgresOutboxWriter.h"
namespace webserver::phase11 {
/** Account SQL adapter. Uses the caller's connection; never borrows another lease. */
class PostgresAccountRepository final {
public:
    static User create(PGconn *connection, User user);
    static std::optional<User> findUser(PGconn *connection, UserId id);
    static std::optional<User> findUserByName(PGconn *connection, std::string_view username);
    static bool updateProfile(PGconn *connection, UserId id, std::string displayName, std::string biography);
    static ProfileWriteResult writeProfileOnce(PGconn *connection, UserId user, std::string operation, std::string displayName, std::string biography);
    static bool updateAvatar(PGconn *connection, UserId id, std::optional<FileId> avatar);
    static Session createSession(PGconn *connection, Session session);
    static std::optional<Session> findSessionByTokenHash(PGconn *connection, std::string_view tokenHash);
    static bool revokeSession(PGconn *connection, std::string_view sessionId, TimePoint now);
};
}
