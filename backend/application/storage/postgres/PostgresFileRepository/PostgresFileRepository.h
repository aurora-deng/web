#pragma once
#include "backend/application/domain/FileTypes/FileTypes.h"
#include "backend/application/modules/profiles/ports/IProfileOperations/IProfileOperations.h"
#include "backend/application/storage/postgres/PostgresOutboxWriter/PostgresOutboxWriter.h"
namespace webserver::phase11 {
/** File SQL adapter. Uses the caller's connection; never borrows another lease. */
class PostgresFileRepository final {
public:
    static FileObject createFile(PGconn *connection, FileObject file);
    static std::optional<FileObject> findFile(PGconn *connection, FileId id);
    static bool updateFileProgress(PGconn *connection, FileId id, UserId owner, std::uint64_t expectedOffset, std::uint64_t uploadedSize);
    static bool completeFile(PGconn *connection, FileId id, UserId owner, std::string sha256, TimePoint now);
    static bool failFile(PGconn *connection, FileId id, UserId owner);
    static bool attachFile(PGconn *connection, MessageId message, FileId file);
    static std::vector<MessageAttachment> attachmentsForMessage(PGconn *connection, MessageId message);
    static bool canAccessFile(PGconn *connection, UserId user, FileId file);
};
}
