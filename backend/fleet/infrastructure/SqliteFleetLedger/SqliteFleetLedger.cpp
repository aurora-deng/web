#include "SqliteFleetLedger.h"

namespace nebula::fleet {
namespace {
constexpr auto schema = R"SQL(
    CREATE TABLE IF NOT EXISTS tasks (
        epoch INTEGER PRIMARY KEY AUTOINCREMENT,
        task_id TEXT NOT NULL UNIQUE,
        station INTEGER NOT NULL CHECK(station IN (1,2)),
        trace_id TEXT NOT NULL,
        robot_id TEXT NOT NULL DEFAULT '',
        phase INTEGER NOT NULL DEFAULT 1 CHECK(phase BETWEEN 1 AND 7),
        outcome TEXT NOT NULL DEFAULT ''
    );
    CREATE INDEX IF NOT EXISTS tasks_queued ON tasks(epoch) WHERE phase=1;
    DROP INDEX IF EXISTS tasks_queue;
    CREATE UNIQUE INDEX IF NOT EXISTS tasks_owner ON tasks(robot_id) WHERE phase IN (2,3,4,7);
)SQL";

Task taskOf(const sqlite::Statement& row) {
    return {{row.string(0), row.string(1), static_cast<Station>(row.integer(2)), row.integer(3)},
            row.string(4), static_cast<Phase>(row.integer(5)), row.string(6)};
}

bool transitionAllowed(Phase before, Phase after) {
    if (before == Phase::leased) return after == Phase::accepted;
    if (before == Phase::accepted || before == Phase::executing) {
        return (before == Phase::accepted && after == Phase::executing) ||
               after == Phase::succeeded || after == Phase::failed || after == Phase::uncertain;
    }
    return false;
}
}

SqliteFleetLedger::SqliteFleetLedger(const std::filesystem::path& path, std::size_t queueLimit)
    : db_(path, "fleet", ""), queueLimit_(queueLimit) {
    if (!queueLimit_ || queueLimit_ > 4096) throw Error(ErrorCode::invalid, "invalid fleet queue limit");
    sqlite::Transaction create(db_);
    db_.exec(schema);
    createReviewTable();
    create.commit();
}

std::optional<Task> SqliteFleetLedger::lookup(std::string_view taskId) {
    if (!validId(taskId)) throw Error(ErrorCode::invalid, "invalid task ID");
    sqlite::Statement row(db_, "SELECT task_id,trace_id,station,epoch,robot_id,phase,outcome FROM tasks WHERE task_id=?");
    row.bind(1, taskId);
    if (!row.next()) return std::nullopt;
    return taskOf(row);
}

std::vector<Task> SqliteFleetLedger::listBefore(std::int64_t beforeEpoch, unsigned limit) {
    if (beforeEpoch < 0 || limit < 1 || limit > 17) throw Error(ErrorCode::invalid, "invalid task page");
    sqlite::Statement query(db_, beforeEpoch ?
        "SELECT task_id,trace_id,station,epoch,robot_id,phase,outcome FROM tasks "
        "WHERE epoch < ?2 ORDER BY epoch DESC LIMIT ?1" :
        "SELECT task_id,trace_id,station,epoch,robot_id,phase,outcome FROM tasks ORDER BY epoch DESC LIMIT ?1");
    query.bind(1, limit);
    if (beforeEpoch) query.bind(2, beforeEpoch);
    std::vector<Task> rows;
    while (query.next()) rows.push_back(taskOf(query));
    return rows;
}

Task SqliteFleetLedger::submit(std::string_view taskId, Station station, std::string_view traceId) {
    if (!validId(taskId) || !validStation(station) || !validTrace(traceId)) {
        throw Error(ErrorCode::invalid, "invalid task parameters");
    }
    sqlite::Transaction write(db_);
    if (auto prior = lookup(taskId)) {
        if (prior->command.station != station) throw Error(ErrorCode::conflict, "task ID reused with another station");
        write.commit();
        return *prior;
    }
    if (taskId.starts_with(replacementPrefix)) throw Error(ErrorCode::invalid, "reserved Agent work order ID");
    auto created = insertTask(taskId, station, traceId);
    write.commit();
    return created;
}

Task SqliteFleetLedger::insertTask(std::string_view taskId, Station station, std::string_view traceId) {
    // Completed task identities remain replayable. Only the live queue consumes admission slots.
    sqlite::Statement capacity(db_, "SELECT 1 FROM tasks WHERE phase=1 LIMIT 1 OFFSET ?");
    capacity.bind(1, static_cast<std::int64_t>(queueLimit_ - 1));
    if (capacity.next()) {
        throw Error(ErrorCode::exhausted, "fleet queue is full");
    }
    sqlite::Statement(db_, "INSERT INTO tasks(task_id,station,trace_id) VALUES (?,?,?)")
        .bind(1, taskId).bind(2, static_cast<int>(station)).bind(3, traceId).run();
    auto created = lookup(taskId);
    if (!created) throw Error(ErrorCode::storage, "task was not stored");
    return *created;
}

std::optional<Command> SqliteFleetLedger::dispatch(std::string_view robotId, Capabilities capabilities) {
    if (!validId(robotId) || !validCapabilities(capabilities)) {
        throw Error(ErrorCode::invalid, "invalid robot registration");
    }
    sqlite::Transaction write(db_);
    // The assigned task is itself the outbox. Keep the row and epoch through
    // retries, process restarts and connection loss.
    sqlite::Statement previous(db_, R"SQL(
        SELECT task_id,trace_id,station,epoch,robot_id,phase,outcome
        FROM tasks WHERE robot_id=? AND phase IN (2,3,4,7)
    )SQL");
    previous.bind(1, robotId);
    if (previous.next()) {
        auto task = taskOf(previous);
        write.commit();
        if (task.phase == Phase::leased && hasStation(capabilities, task.command.station)) return task.command;
        return std::nullopt;
    }
    sqlite::Statement queued(db_, R"SQL(
        SELECT task_id,trace_id,station,epoch,robot_id,phase,outcome
        FROM tasks WHERE phase=1 AND (station & ?) != 0 ORDER BY epoch LIMIT 1
    )SQL");
    queued.bind(1, capabilities);
    if (!queued.next()) {
        write.commit();
        return std::nullopt;
    }
    auto task = taskOf(queued);
    sqlite::Statement(db_, "UPDATE tasks SET robot_id=?,phase=2 WHERE epoch=?")
        .bind(1, robotId).bind(2, task.command.epoch).run();
    write.commit();
    return task.command;
}

void SqliteFleetLedger::apply(std::string_view robotId, const Receipt& receipt) {
    checkReceipt(receipt);
    if (receipt.outcome == supersededOutcome) throw Error(ErrorCode::invalid, "reserved Agent outcome");
    sqlite::Transaction write(db_);
    const auto task = lookup(receipt.taskId);
    if (!task || task->robotId != robotId || task->command.epoch != receipt.epoch) {
        throw Error(ErrorCode::conflict, "receipt does not own this task");
    }
    if (task->phase == receipt.phase && task->outcome == receipt.outcome) {
        write.commit();
        return;
    }
    if (!transitionAllowed(task->phase, receipt.phase)) {
        throw Error(ErrorCode::conflict, "receipt cannot advance task phase");
    }
    sqlite::Statement(db_, "UPDATE tasks SET phase=?,outcome=? WHERE epoch=?")
        .bind(1, static_cast<int>(receipt.phase)).bind(2, receipt.outcome).bind(3, receipt.epoch).run();
    write.commit();
}

FleetLoad SqliteFleetLedger::load() {
    sqlite::Statement row(db_, R"SQL(
        SELECT (SELECT COUNT(*) FROM tasks WHERE phase=1),
               (SELECT COUNT(*) FROM tasks WHERE phase IN (2,3,4,7))
    )SQL");
    if (!row.next()) throw Error(ErrorCode::storage, "missing fleet queue counters");
    return {static_cast<unsigned>(row.integer(0)), static_cast<unsigned>(row.integer(1)), 0,
            static_cast<unsigned>(queueLimit_)};
}
}
