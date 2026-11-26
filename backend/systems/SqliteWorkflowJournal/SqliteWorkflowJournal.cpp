#include "SqliteWorkflowJournal.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace nebula::systems {
using namespace workflow;
using fleet::sqlite::Statement;
namespace {
constexpr std::size_t projectedRuns = 32;
constexpr std::size_t nodesPerRun = 128;
// Room for a 128 KiB definition and all 128 bounded (4 KiB) checkpoints,
// including SQLite page/index overhead. Existing runs remain replayable.
constexpr std::int64_t runReserveBytes = 2 * 1024 * 1024;

JournalLimits checkedLimits(JournalLimits limits) {
    if (!limits.maxRuns || limits.maxRuns > 50000 || limits.maxBytes < 4 * 1024 * 1024 ||
        limits.maxBytes > 64 * 1024 * 1024) {
        throw std::invalid_argument("invalid workflow journal budget");
    }
    return limits;
}

std::int64_t pragma(fleet::sqlite::Database& db, const char* query) {
    Statement value(db, query);
    if (!value.next()) throw std::runtime_error("workflow journal budget unavailable");
    return value.integer(0);
}
}

SqliteWorkflowJournal::SqliteWorkflowJournal(const std::filesystem::path& file, std::filesystem::path snapshot,
                                             JournalLimits limits)
    : snapshot_(std::move(snapshot)), limits_(checkedLimits(limits)), db_(file, "workflow", "") {
    pageBytes_ = pragma(db_, "PRAGMA page_size");
    if (pageBytes_ < 512 || pageBytes_ > 65536) throw std::runtime_error("invalid SQLite page size");
    maxPages_ = static_cast<std::int64_t>(limits_.maxBytes / pageBytes_);
    // Never shrink or remove an existing archive to satisfy a new budget.
    if (pragma(db_, "PRAGMA page_count") > maxPages_) {
        throw std::runtime_error("existing workflow archive exceeds storage budget");
    }
    const auto cap = "PRAGMA max_page_count=" + std::to_string(maxPages_);
    db_.exec(cap.c_str());
    if (pragma(db_, "PRAGMA max_page_count") != maxPages_) {
        throw std::runtime_error("workflow journal storage cap unavailable");
    }
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
    publish();
}

std::string SqliteWorkflowJournal::open(std::string_view run, std::string_view definition, std::string_view trace) {
    std::lock_guard lock(access_);
    if (!validIdentifier(run, 64) || definition.empty() || definition.size() > 128 * 1024 ||
        (!trace.empty() && !validTraceId(trace))) throw std::invalid_argument("invalid workflow journal definition");
    fleet::sqlite::Transaction admission(db_);
    {
        Statement lookup(db_, "SELECT definition, trace_id FROM workflow_runs WHERE run_id=?");
        lookup.bind(1, run);
        if (lookup.next()) {
            const auto savedTrace = lookup.string(1);
            if (lookup.string(0) != definition || (!trace.empty() && trace != savedTrace)) {
                throw std::invalid_argument("workflow identity conflicts with checkpoint");
            }
            admission.commit();
            return savedTrace;
        }
    }
    {
        Statement count(db_, "SELECT COUNT(*) FROM workflow_runs");
        if (!count.next() || count.integer(0) >= static_cast<std::int64_t>(limits_.maxRuns)) {
            throw JournalCapacityExceeded("workflow archive run budget exhausted");
        }
    }
    const auto pages = pragma(db_, "PRAGMA page_count");
    const auto free = pragma(db_, "PRAGMA freelist_count");
    const auto reserve = (runReserveBytes + pageBytes_ - 1) / pageBytes_;
    if (free < 0 || free > pages || pages - free + reserve > maxPages_) {
        throw JournalCapacityExceeded("workflow archive storage budget exhausted");
    }
    auto identity = trace.empty() ? newTraceId() : std::string(trace);
    Statement(db_, "INSERT INTO workflow_runs(run_id,definition,trace_id) VALUES (?,?,?)")
        .bind(1, run).bind(2, definition).bind(3, identity).run();
    admission.commit();
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
    if (!old.attempts) {
        Statement count(db_, "SELECT COUNT(*) FROM workflow_nodes WHERE run_id=?");
        count.bind(1, run);
        if (!count.next() || count.integer(0) >= static_cast<std::int64_t>(nodesPerRun)) {
            throw std::invalid_argument("workflow checkpoint node budget exhausted");
        }
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
    publish(run);
}

void SqliteWorkflowJournal::publish(std::string_view currentRun) {
    if (snapshot_.empty()) return;
    // The authoritative archive is never pruned. The hot projection holds whole
    // recent runs only: 32 * 128 nodes, with explicit coverage and no input values.
    std::vector<std::string> runs;
    {
        Statement row(db_, "SELECT run_id FROM workflow_runs ORDER BY rowid DESC LIMIT 32");
        while (row.next()) runs.push_back(row.string(0));
    }
    if (!currentRun.empty() && std::ranges::find(runs, currentRun) == runs.end()) {
        runs.insert(runs.begin(), std::string(currentRun));
        if (runs.size() > projectedRuns) runs.pop_back();
    }
    const auto total = pragma(db_, "SELECT COUNT(*) FROM workflow_runs");
    std::ostringstream output;
    output << R"({"format":1,"truncated":)" << (total > static_cast<std::int64_t>(runs.size()) ? "true" : "false")
           << R"(,"runs":[)";
    bool first = true;
    for (const auto& run : runs) {
        if (!std::exchange(first, false)) output << ',';
        output << std::quoted(run);
    }
    output << R"(],"nodes":[)";
    first = true;
    for (const auto& run : runs) {
        Statement row(db_, "SELECT run_id,node_id,attempts,effect,result_kind,error_code FROM workflow_nodes WHERE run_id=? ORDER BY node_id LIMIT 129");
        row.bind(1, run);
        std::size_t count = 0;
        while (row.next()) {
            if (++count > nodesPerRun) throw std::runtime_error("checkpoint snapshot node limit");
            if (!std::exchange(first, false)) output << ',';
            output << R"({"run_id":)" << std::quoted(row.string(0)) << R"(,"node_id":)" << std::quoted(row.string(1))
                   << R"(,"attempts":)" << row.integer(2) << R"(,"effect":)" << row.integer(3)
                   << R"(,"result_kind":)" << row.integer(4) << R"(,"error_code":)" << std::quoted(row.string(5))
                   << R"(,"value":""})";
        }
    }
    output << "]}";
    const auto value = output.str();
    if (value.size() > 2 * 1024 * 1024) throw std::runtime_error("checkpoint snapshot byte limit");
    auto temporary = snapshot_; temporary += ".pending";
    if (std::filesystem::is_symlink(snapshot_) || std::filesystem::is_symlink(temporary)) {
        throw std::runtime_error("invalid checkpoint snapshot path");
    }
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << value; file.flush();
        if (!file) throw std::runtime_error("checkpoint snapshot unavailable");
    }
    std::filesystem::rename(temporary, snapshot_);
}

}
