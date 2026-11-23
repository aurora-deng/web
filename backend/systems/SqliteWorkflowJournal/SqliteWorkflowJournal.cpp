#include "SqliteWorkflowJournal.h"

#include <stdexcept>

namespace nebula::systems {
using namespace workflow;
using fleet::sqlite::Statement;

SqliteWorkflowJournal::SqliteWorkflowJournal(const std::filesystem::path& file) : db_(file, "workflow", "") {
    db_.exec("PRAGMA foreign_keys=ON");
    fleet::sqlite::Transaction setup(db_);
    db_.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS workflow_runs (
            run_id TEXT PRIMARY KEY,
            definition TEXT NOT NULL,
            trace_id TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS workflow_nodes (
            run_id TEXT NOT NULL REFERENCES workflow_runs(run_id),
            node_id TEXT NOT NULL,
            attempts INTEGER NOT NULL CHECK(attempts BETWEEN 1 AND 3),
            effect INTEGER NOT NULL CHECK(effect BETWEEN 0 AND 2),
            result_kind INTEGER NOT NULL DEFAULT 0 CHECK(result_kind BETWEEN 0 AND 2),
            value TEXT NOT NULL DEFAULT '',
            error_code TEXT NOT NULL DEFAULT '',
            PRIMARY KEY(run_id, node_id)
        );
    )SQL");
    setup.commit();
}

std::string SqliteWorkflowJournal::open(std::string_view run, std::string_view definition, std::string_view trace) {
    std::lock_guard lock(access_);
    if (!validIdentifier(run, 64) || definition.empty() || definition.size() > 128 * 1024 ||
        (!trace.empty() && !validTraceId(trace))) throw std::invalid_argument("invalid workflow journal definition");
    {
        Statement lookup(db_, "SELECT definition, trace_id FROM workflow_runs WHERE run_id=?");
        lookup.bind(1, run);
        if (lookup.next()) {
            const auto savedTrace = lookup.string(1);
            if (lookup.string(0) != definition || (!trace.empty() && trace != savedTrace)) {
                throw std::invalid_argument("workflow identity conflicts with checkpoint");
            }
            return savedTrace;
        }
    }
    {
        Statement count(db_, "SELECT COUNT(*) FROM workflow_runs");
        if (!count.next() || count.integer(0) >= 128) throw std::runtime_error("workflow archive is full");
    }
    auto identity = trace.empty() ? newTraceId() : std::string(trace);
    Statement(db_, "INSERT INTO workflow_runs(run_id,definition,trace_id) VALUES (?,?,?)")
        .bind(1, run).bind(2, definition).bind(3, identity).run();
    return identity;
}

Checkpoint SqliteWorkflowJournal::readNode(std::string_view run, std::string_view nodeId) {
    Statement statement(db_, "SELECT attempts,effect,result_kind,value,error_code FROM workflow_nodes WHERE run_id=? AND node_id=?");
    statement.bind(1, run).bind(2, nodeId);
    if (!statement.next()) return {};
    Checkpoint checkpoint{static_cast<unsigned>(statement.integer(0)), static_cast<Effect>(statement.integer(1)), std::nullopt};
    if (const auto kind = statement.integer(2)) {
        checkpoint.result = WorkResult{kind == 1 ? WorkStatus::success : WorkStatus::failure, statement.string(3), statement.string(4)};
    }
    return checkpoint;
}

Checkpoint SqliteWorkflowJournal::node(std::string_view run, std::string_view nodeId) {
    std::lock_guard lock(access_);
    return readNode(run, nodeId);
}

void SqliteWorkflowJournal::start(std::string_view run, std::string_view nodeId, Effect effect, unsigned attempt) {
    std::lock_guard lock(access_);
    if (!validIdentifier(nodeId)) throw std::invalid_argument("invalid workflow node ID");
    const auto old = readNode(run, nodeId);
    if (old.result || old.attempts + 1 != attempt || (old.attempts && (old.effect != effect || effect == Effect::external))) {
        throw std::invalid_argument("invalid workflow attempt transition");
    }
    Statement(db_, R"SQL(
        INSERT INTO workflow_nodes(run_id,node_id,attempts,effect) VALUES (?,?,?,?)
        ON CONFLICT(run_id,node_id) DO UPDATE SET attempts=excluded.attempts
    )SQL").bind(1, run).bind(2, nodeId).bind(3, attempt).bind(4, static_cast<int>(effect)).run();
}

void SqliteWorkflowJournal::settle(std::string_view run, std::string_view nodeId, const WorkResult& result) {
    std::lock_guard lock(access_);
    const auto old = readNode(run, nodeId);
    if (!old.attempts || old.result || (result.status != WorkStatus::success && result.status != WorkStatus::failure) ||
        result.value.size() > 4096 || (!result.errorCode.empty() && !validIdentifier(result.errorCode))) {
        throw std::invalid_argument("invalid workflow settlement");
    }
    Statement(db_, "UPDATE workflow_nodes SET result_kind=?, value=?, error_code=? WHERE run_id=? AND node_id=?")
        .bind(1, result.status == WorkStatus::success ? 1 : 2).bind(2, result.value).bind(3, result.errorCode)
        .bind(4, run).bind(5, nodeId).run();
}

}
