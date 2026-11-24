#include "SqliteAgentSession.h"

#include <algorithm>
#include <functional>
#include <sstream>

namespace nebula::agent {
namespace {

using fleet::sqlite::Statement;
using fleet::sqlite::Transaction;
constexpr int recentWindow = 6;

void appendIds(std::vector<std::string>& result, std::string_view line, std::size_t limit) {
    std::istringstream input{std::string(line)};
    std::string id;
    while (result.size() < limit && input >> id) {
        if (!fleet::validId(id)) throw std::runtime_error("invalid session memory");
        if (std::ranges::find(result, id) == result.end()) result.push_back(id);
    }
}

std::string idsOf(const auto& values, auto project) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) out += ' ';
        out += std::invoke(project, value);
    }
    return out;
}

}  // namespace

SqliteAgentSession::SqliteAgentSession(const std::filesystem::path& file, std::string scope, std::string session)
    : db_(file, "agent session", ""), scope_(std::move(scope)), session_(std::move(session)) {
    if (!fleet::validId(session_) || scope_.empty() || scope_.size() > 100) {
        throw std::invalid_argument("invalid Agent session");
    }
    db_.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS agent_turns (
          turn_index INTEGER PRIMARY KEY, scope TEXT NOT NULL, session_id TEXT NOT NULL,
          turn_id TEXT NOT NULL, task_ids TEXT NOT NULL, UNIQUE(scope,session_id,turn_id)
        );
        CREATE INDEX IF NOT EXISTS agent_turn_window ON agent_turns(scope,session_id,turn_index DESC);
        CREATE TABLE IF NOT EXISTS agent_summary (
          scope TEXT NOT NULL, session_id TEXT NOT NULL, task_ids TEXT NOT NULL, PRIMARY KEY(scope,session_id)
        );
    )SQL");
}

std::vector<std::string> SqliteAgentSession::context(std::size_t limit) {
    if (limit == 0 || limit > maxReviewTasks) throw std::invalid_argument("invalid Agent memory window");
    std::vector<std::string> result;
    Statement recent(db_, "SELECT task_ids FROM agent_turns WHERE scope=? AND session_id=? "
                          "ORDER BY turn_index DESC LIMIT ?");
    recent.bind(1, scope_).bind(2, session_).bind(3, recentWindow);
    while (recent.next() && result.size() < limit) appendIds(result, recent.string(0), limit);
    Statement summary(db_, "SELECT task_ids FROM agent_summary WHERE scope=? AND session_id=?");
    summary.bind(1, scope_).bind(2, session_);
    if (summary.next()) appendIds(result, summary.string(0), limit);
    return result;
}

std::size_t SqliteAgentSession::remember(std::string_view turn, std::span<const TaskEvidence> evidence) {
    if (!fleet::validId(turn) || evidence.empty() || evidence.size() > maxReviewTasks ||
        std::ranges::any_of(evidence, [](const auto& row) { return !validEvidence(row, row.taskId); })) {
        throw std::invalid_argument("invalid Agent memory turn");
    }
    Transaction write(db_);
    auto count = Statement(db_, "SELECT count(*) FROM (SELECT scope,session_id FROM agent_summary UNION "
                               "SELECT scope,session_id FROM agent_turns) WHERE scope<>? OR session_id<>?");
    count.bind(1, scope_).bind(2, session_);
    if (!count.next() || count.integer(0) >= 128) throw std::runtime_error("Agent session archive full");
    Statement(db_, "INSERT INTO agent_turns(scope,session_id,turn_id,task_ids) VALUES(?,?,?,?) ON CONFLICT DO NOTHING")
        .bind(1, scope_).bind(2, session_).bind(3, turn).bind(4, idsOf(evidence, &TaskEvidence::taskId)).run();
    std::vector<std::string> summary;
    Statement old(db_, "SELECT task_ids FROM agent_turns WHERE scope=? AND session_id=? "
                       "ORDER BY turn_index DESC LIMIT -1 OFFSET ?");
    old.bind(1, scope_).bind(2, session_).bind(3, recentWindow);
    if (old.next()) {
        do { appendIds(summary, old.string(0), maxReviewTasks); } while (old.next());
        {
            Statement previous(db_, "SELECT task_ids FROM agent_summary WHERE scope=? AND session_id=?");
            previous.bind(1, scope_).bind(2, session_);
            if (previous.next()) appendIds(summary, previous.string(0), maxReviewTasks);
        }
        Statement(db_, "INSERT INTO agent_summary(scope,session_id,task_ids) VALUES(?,?,?) "
                       "ON CONFLICT(scope,session_id) DO UPDATE SET task_ids=excluded.task_ids")
            .bind(1, scope_).bind(2, session_).bind(3, idsOf(summary, std::identity{})).run();
        Statement(db_, "DELETE FROM agent_turns WHERE turn_index IN (SELECT turn_index FROM agent_turns "
                       "WHERE scope=? AND session_id=? ORDER BY turn_index DESC LIMIT -1 OFFSET ?)")
            .bind(1, scope_).bind(2, session_).bind(3, recentWindow).run();
    }
    write.commit();
    return summary.size();
}

}  // namespace nebula::agent
