#include "PostgresWorkflowQueue.h"

#include <libpq-fe.h>

#include <algorithm>
#include <stdexcept>

namespace nebula::systems {
namespace {

using namespace workflow;
using PgResult = std::unique_ptr<PGresult, decltype(&PQclear)>;

constexpr std::string_view effectName(Effect effect) {
    switch (effect) {
    case Effect::read: return "read";
    case Effect::idempotent: return "idempotent";
    case Effect::external: return "external";
    }
    throw std::invalid_argument("unsupported workflow effect");
}

constexpr std::string_view resultName(WorkStatus status) {
    switch (status) {
    case WorkStatus::success: return "success";
    case WorkStatus::failure: return "failed";
    case WorkStatus::retryable: return "retry";
    case WorkStatus::uncertain: return "uncertain";
    }
    throw std::invalid_argument("unsupported workflow result");
}

void validateRun(std::string_view run) {
    if (!validIdentifier(run, 64)) throw std::invalid_argument("invalid workflow run ID");
}

std::string definitionOf(const WorkflowGraph& graph) {
    std::string value = graph.name();
    for (const auto& step : graph.steps()) {
        const auto& node = step.node;
        value += "\n" + node.id + ":" + std::string(effectName(node.effect)) + ":" +
                 std::to_string(node.attempts) + ":" + std::to_string(node.timeout.count()) + ":" +
                 std::to_string(node.backoff.count());
        for (const auto& dep : node.needs) value += ":" + dep;
    }
    return value;
}

}  // namespace

class PostgresWorkflowQueue::Impl {
public:
    explicit Impl(std::string_view dsn) : db_(PQconnectdb(std::string(dsn).c_str()), &PQfinish) {
        if (!db_ || PQstatus(db_.get()) != CONNECTION_OK || PQsetClientEncoding(db_.get(), "UTF8") != 0) {
            throw std::runtime_error("workflow PostgreSQL connection failed");
        }
        query("SET statement_timeout = 3000");
        query("SET lock_timeout = 2000");
    }

    PgResult query(const char* sql, const std::vector<std::string>& args = {}) {
        std::vector<const char*> values;
        for (const auto& arg : args) values.push_back(arg.c_str());
        PgResult result(PQexecParams(db_.get(), sql, static_cast<int>(values.size()), nullptr,
                                     values.data(), nullptr, nullptr, 0), &PQclear);
        if (!result || (PQresultStatus(result.get()) != PGRES_COMMAND_OK && PQresultStatus(result.get()) != PGRES_TUPLES_OK)) {
            const auto* code = result ? PQresultErrorField(result.get(), PG_DIAG_SQLSTATE) : nullptr;
            throw std::runtime_error("workflow PostgreSQL statement failed (" + std::string(code ? code : "no_result") + ")");
        }
        return result;
    }

private:
    std::unique_ptr<PGconn, decltype(&PQfinish)> db_;
};

PostgresWorkflowQueue::PostgresWorkflowQueue(std::string_view dsn) : impl_(std::make_unique<Impl>(dsn)) {}
PostgresWorkflowQueue::~PostgresWorkflowQueue() = default;

std::string PostgresWorkflowQueue::submit(std::string_view run, const WorkflowGraph& graph, Milliseconds budget) {
    validateRun(run);
    if (budget < Milliseconds{1} || budget > Milliseconds{600000} ||
        std::ranges::any_of(graph.steps(), [](const auto& step) { return step.node.acceptIncomplete; })) {
        throw std::invalid_argument("invalid clustered workflow deadline or dependency policy");
    }
    const std::string id(run);
    const auto trace = newTraceId();
    const auto definition = definitionOf(graph);
    impl_->query("BEGIN");
    try {
        const auto inserted = impl_->query("INSERT INTO workflow_cluster_run(run_id,trace_id,workflow,definition,deadline) "
                    "VALUES($1,$2,$3,$4,clock_timestamp()+$5::int*interval '1 ms') ON CONFLICT(run_id) DO NOTHING RETURNING run_id",
                    {id, trace, graph.name(), definition, std::to_string(budget.count())});
        if (!PQntuples(inserted.get())) {
            const auto saved = impl_->query("SELECT trace_id,definition FROM workflow_cluster_run WHERE run_id=$1", {id});
            if (PQntuples(saved.get()) != 1 || PQgetvalue(saved.get(), 0, 1) != definition) {
                throw std::invalid_argument("clustered workflow run identity conflicts");
            }
            impl_->query("COMMIT");
            return PQgetvalue(saved.get(), 0, 0);
        }
        for (const auto& step : graph.steps()) {
            const auto& node = step.node;
            impl_->query("INSERT INTO workflow_cluster_node(run_id,node_id,effect,max_attempts,timeout_ms,backoff_ms) "
                        "VALUES($1,$2,$3,$4,$5,$6)", {id, node.id, std::string(effectName(node.effect)),
                        std::to_string(node.attempts), std::to_string(node.timeout.count()), std::to_string(node.backoff.count())});
            for (const auto& predecessor : node.needs) {
                impl_->query("INSERT INTO workflow_cluster_edge VALUES($1,$2,$3)", {id, predecessor, node.id});
            }
        }
        impl_->query("COMMIT");
    } catch (...) {
        impl_->query("ROLLBACK");
        throw;
    }
    return trace;
}

std::optional<NodeLease> PostgresWorkflowQueue::claim(std::string_view run, const WorkflowGraph& graph,
                                                       std::string_view owner, Milliseconds lease) {
    validateRun(run);
    if (!validIdentifier(owner, 64) || lease < Milliseconds{50} || lease > Milliseconds{30000}) {
        throw std::invalid_argument("invalid workflow lease");
    }
    const std::string id(run);
    const auto definition = impl_->query("SELECT definition FROM workflow_cluster_run WHERE run_id=$1", {id});
    if (PQntuples(definition.get()) != 1 || PQgetvalue(definition.get(), 0, 0) != definitionOf(graph)) {
        throw std::invalid_argument("worker has a different registered workflow");
    }
    const auto result = impl_->query("SELECT * FROM workflow_cluster_claim_v2($1,$2,$3)",
                                     {id, std::string(owner), std::to_string(lease.count())});
    if (!PQntuples(result.get())) return std::nullopt;
    NodeLease claim{id, PQgetvalue(result.get(), 0, 0), std::string(owner), PQgetvalue(result.get(), 0, 2),
                    static_cast<unsigned>(std::stoul(PQgetvalue(result.get(), 0, 1))),
                    Milliseconds{std::stoll(PQgetvalue(result.get(), 0, 3))}, {}};
    const auto step = std::ranges::find_if(graph.steps(), [&](const auto& row) { return row.node.id == claim.node; });
    if (step == graph.steps().end()) throw std::runtime_error("worker has no handler for a leased workflow node");
    const auto inputs = impl_->query(
        "SELECT n.node_id,n.value FROM workflow_cluster_edge e JOIN workflow_cluster_node n "
        "ON n.run_id=e.run_id AND n.node_id=e.predecessor WHERE e.run_id=$1 AND e.successor=$2 AND n.state='success'",
        {id, claim.node});
    if (PQntuples(inputs.get()) != static_cast<int>(step->node.needs.size())) {
        throw std::runtime_error("persisted workflow dependencies differ");
    }
    for (const auto& predecessor : step->node.needs) {
        for (int row = 0; row < PQntuples(inputs.get()); ++row) {
            if (predecessor == PQgetvalue(inputs.get(), row, 0)) {
                claim.inputs.push_back({predecessor, PQgetvalue(inputs.get(), row, 1), State::success});
                break;
            }
        }
    }
    if (claim.inputs.size() != step->node.needs.size()) throw std::runtime_error("persisted workflow input missing");
    return claim;
}

std::vector<ReadyWorkflow> PostgresWorkflowQueue::readyRuns(std::span<const WorkflowGraph> registered) {
    if (registered.empty() || registered.size() > 64) throw std::invalid_argument("invalid workflow registry");
    std::string names;
    for (const auto& graph : registered) {
        if (!names.empty()) names += ',';
        names += graph.name();
    }
    const auto result = impl_->query("SELECT * FROM workflow_cluster_ready_runs(string_to_array($1,','),16)", {names});
    std::vector<ReadyWorkflow> ready;
    for (int i = 0; i < PQntuples(result.get()); ++i) {
        ready.push_back({PQgetvalue(result.get(), i, 0), PQgetvalue(result.get(), i, 1)});
    }
    return ready;
}

bool PostgresWorkflowQueue::renew(const NodeLease& lease, Milliseconds extension) {
    if (extension < Milliseconds{50} || extension > Milliseconds{30000}) {
        throw std::invalid_argument("invalid workflow lease extension");
    }
    const auto renewed = impl_->query("SELECT workflow_cluster_renew($1,$2,$3,$4,$5)",
        {lease.run, lease.node, lease.owner, std::to_string(lease.attempt), std::to_string(extension.count())});
    return std::string_view(PQgetvalue(renewed.get(), 0, 0)) == "t";
}

bool PostgresWorkflowQueue::finish(const NodeLease& lease, const WorkResult& result) {
    if (result.value.size() > 4096 || result.value.find('\0') != result.value.npos ||
        (!result.errorCode.empty() && !validIdentifier(result.errorCode))) {
        throw std::invalid_argument("invalid workflow completion");
    }
    const auto written = impl_->query("SELECT workflow_cluster_finish($1,$2,$3,$4,$5,$6,$7)",
        {lease.run, lease.node, lease.owner, std::to_string(lease.attempt),
         std::string(resultName(result.status)), result.value, result.errorCode});
    return std::string_view(PQgetvalue(written.get(), 0, 0)) == "t";
}

bool PostgresWorkflowQueue::reconcile(std::string_view run, std::string_view node, unsigned attempt,
                                      bool succeeded, std::string_view evidenceRef, std::string_view value) {
    validateRun(run);
    if (!validIdentifier(node) || attempt < 1 || attempt > 3 || !validIdentifier(evidenceRef) ||
        value.size() > 4096 || value.find('\0') != value.npos || (!succeeded && !value.empty())) {
        throw std::invalid_argument("invalid workflow verification");
    }
    const auto saved = impl_->query("SELECT workflow_cluster_reconcile($1,$2,$3,$4,$5,$6)",
        {std::string(run), std::string(node), std::to_string(attempt), succeeded ? "success" : "failed",
         std::string(evidenceRef), std::string(value)});
    return std::string_view(PQgetvalue(saved.get(), 0, 0)) == "t";
}

std::string PostgresWorkflowQueue::state(std::string_view run) {
    validateRun(run);
    const auto result = impl_->query("SELECT workflow_cluster_run_state($1)", {std::string(run)});
    if (PQgetisnull(result.get(), 0, 0)) throw std::runtime_error("unknown workflow run");
    return PQgetvalue(result.get(), 0, 0);
}

}  // namespace nebula::systems
