#pragma once
#include "backend/application/domain/MessageTypes/MessageTypes.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
#include "backend/application/storage/postgres/PostgresOutboxWriter/PostgresOutboxWriter.h"
namespace webserver::phase11 {
/** MessageWrite SQL adapter. Uses the caller's connection; never borrows another lease. */
class PostgresMessageWriteRepository final {
public:
    static AppendMessageResult appendMessage(PGconn *connection, bool ownsTransaction, Message message);
    static RecallMessageResult recallMessage(PGconn *connection, MessageId id, UserId actor, TimePoint now);
};
}
