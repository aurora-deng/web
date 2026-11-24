#include "SqliteEdgeLedger.h"

namespace nebula::fleet {
namespace {
constexpr auto schema = R"SQL(
    CREATE TABLE IF NOT EXISTS inbox (
        task_id TEXT PRIMARY KEY NOT NULL,
        trace_id TEXT NOT NULL,
        station INTEGER NOT NULL CHECK(station IN (1,2)),
        epoch INTEGER UNIQUE NOT NULL,
        phase INTEGER NOT NULL DEFAULT 3 CHECK(phase BETWEEN 3 AND 7),
        outcome TEXT NOT NULL DEFAULT '',
        reported INTEGER NOT NULL DEFAULT 0 CHECK(reported IN (0,1)),
        starts INTEGER NOT NULL DEFAULT 0 CHECK(starts BETWEEN 0 AND 1)
    );
    CREATE INDEX IF NOT EXISTS inbox_pending ON inbox(epoch) WHERE reported=0;
    CREATE INDEX IF NOT EXISTS inbox_active ON inbox(phase) WHERE phase=3 OR phase=4 OR phase=7;
    DROP INDEX IF EXISTS inbox_report;
)SQL";

Command commandOf(const sqlite::Statement& row) {
    return {row.string(0), row.string(1), static_cast<Station>(row.integer(2)), row.integer(3)};
}
}

SqliteEdgeLedger::SqliteEdgeLedger(const std::filesystem::path& path, std::string_view robotId)
    : db_(path, "edge", robotId) {
    if (!validId(robotId)) throw Error(ErrorCode::invalid, "invalid edge identity");
    sqlite::Transaction recovery(db_);
    db_.exec(schema);
    // A crash after begin and before finish leaves the side effect unknown.
    db_.exec("UPDATE inbox SET phase=7,outcome='edge_restart',reported=0 WHERE phase=4");
    recovery.commit();
}

void SqliteEdgeLedger::stage(const Command& command) {
    checkCommand(command);
    std::lock_guard lock(mutex_);
    sqlite::Transaction write(db_);
    sqlite::Statement previous(db_, "SELECT task_id,trace_id,station,epoch FROM inbox WHERE task_id=?");
    previous.bind(1, command.taskId);
    if (previous.next()) {
        if (commandOf(previous) != command) throw Error(ErrorCode::conflict, "command differs from durable inbox");
        write.commit();
        return;
    }
    sqlite::Statement busy(db_, "SELECT 1 FROM inbox WHERE phase=3 OR phase=4 OR phase=7 LIMIT 1");
    if (busy.next()) throw Error(ErrorCode::conflict, "edge already has an active or uncertain command");
    // Retain exact command receipts for deduplication, including late replays after a restart.
    sqlite::Statement capacity(db_, "SELECT 1 FROM inbox WHERE reported=0 LIMIT 1 OFFSET 255");
    if (capacity.next()) throw Error(ErrorCode::exhausted, "edge pending receipt limit reached");
    sqlite::Statement(db_, "INSERT INTO inbox(task_id,trace_id,station,epoch) VALUES (?,?,?,?)")
        .bind(1, command.taskId).bind(2, command.traceId).bind(3, static_cast<int>(command.station))
        .bind(4, command.epoch).run();
    write.commit();
}

std::optional<Receipt> SqliteEdgeLedger::pendingReceipt() {
    std::lock_guard lock(mutex_);
    sqlite::Statement row(db_, "SELECT task_id,epoch,phase,outcome FROM inbox WHERE reported=0 ORDER BY epoch LIMIT 1");
    if (!row.next()) return std::nullopt;
    return Receipt{row.string(0), row.integer(1), static_cast<Phase>(row.integer(2)), row.string(3)};
}

void SqliteEdgeLedger::confirm(const Receipt& receipt) {
    checkReceipt(receipt);
    std::lock_guard lock(mutex_);
    // An executing receipt can cross a terminal update on the worker thread.
    // Confirm exactly the phase echoed by the coordinator.
    sqlite::Statement(db_, "UPDATE inbox SET reported=1 WHERE task_id=? AND epoch=? AND phase=? AND outcome=?")
        .bind(1, receipt.taskId).bind(2, receipt.epoch).bind(3, static_cast<int>(receipt.phase))
        .bind(4, receipt.outcome).run();
}

std::optional<Command> SqliteEdgeLedger::beginReady() {
    std::lock_guard lock(mutex_);
    sqlite::Transaction write(db_);
    sqlite::Statement row(db_, "SELECT task_id,trace_id,station,epoch FROM inbox WHERE phase=3 AND reported=1 LIMIT 1");
    if (!row.next()) {
        write.commit();
        return std::nullopt;
    }
    const auto command = commandOf(row);
    sqlite::Statement(db_, "UPDATE inbox SET phase=4,reported=0,starts=starts+1 WHERE epoch=?")
        .bind(1, command.epoch).run();
    write.commit();
    return command;
}

void SqliteEdgeLedger::finish(const Command& command, Phase phase, std::string_view outcome) {
    if ((phase != Phase::succeeded && phase != Phase::failed && phase != Phase::uncertain) || !validId(outcome)) {
        throw Error(ErrorCode::invalid, "invalid execution result");
    }
    std::lock_guard lock(mutex_);
    sqlite::Transaction write(db_);
    sqlite::Statement row(db_, "SELECT task_id,trace_id,station,epoch,phase FROM inbox WHERE task_id=?");
    row.bind(1, command.taskId);
    if (!row.next() || commandOf(row) != command || static_cast<Phase>(row.integer(4)) != Phase::executing) {
        throw Error(ErrorCode::conflict, "cannot finish a command this edge did not start");
    }
    sqlite::Statement(db_, "UPDATE inbox SET phase=?,outcome=?,reported=0 WHERE epoch=?")
        .bind(1, static_cast<int>(phase)).bind(2, outcome).bind(3, command.epoch).run();
    write.commit();
}
}
