#include "backend/agent/application/FleetAgent/FleetAgent.h"
#include "backend/workflow/ObserverEvents/ObserverEvents.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
using namespace nebula::agent;
using namespace nebula::workflow;
using nebula::fleet::Phase;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

EvidenceResult fixture(std::string_view taskId) {
    constexpr auto trace = "1234567890abcdef1234567890abcdef";
    TaskEvidence row{std::string(taskId), trace, Phase::queued, 12, false, false, "",
                     "test.fixture", std::chrono::system_clock::now(), true};
    if (taskId == "failed") {
        row.phase = Phase::failed;
        row.outcome = "precondition_failed";
    } else if (taskId == "uncertain") {
        row.phase = Phase::uncertain;
        row.needsReconciliation = true;
        row.outcome = "edge_restart";
    } else if (taskId == "finished") {
        row.phase = Phase::succeeded;
        row.outcome = "arrived";
    } else if (taskId == "disconnected") {
        row.phase = Phase::accepted;
        row.needsReconciliation = true;
    } else if (taskId == "leased") {
        row.phase = Phase::leased;
    } else if (taskId == "executing") {
        row.phase = Phase::executing;
    } else if (taskId != "queued") {
        return {ReadStatus::missing, {}};
    }
    row.assigned = row.phase != Phase::queued;
    return {ReadStatus::found, std::move(row)};
}

class FixtureReader final : public TaskEvidenceReader {
public:
    EvidenceResult read(std::string_view id, std::stop_token, Clock::time_point) override {
        ++reads;
        return load(id);
    }

    unsigned reads{};
    std::function<EvidenceResult(std::string_view)> load = fixture;
};

class FixtureProvider final : public ProposalProvider {
public:
    Candidate plan(Intent, std::span<const TaskEvidence>, std::stop_token stop, Clock::time_point deadline) override {
        ++calls;
        while (waitUntilDeadline && !stop.stop_requested() && Clock::now() < deadline) {
            std::this_thread::sleep_until(deadline + Milliseconds{1});
        }
        return output;
    }

    unsigned calls{};
    bool waitUntilDeadline{};
    Candidate output{CandidateStatus::proposed, fleetProposal("fleet.review_failures", 1, {"failed"})};
};

void expectFailure(const Answer& answer, std::string_view code) {
    require(answer.run.state == State::failed && !answer.proposal &&
            std::ranges::any_of(answer.run.nodes, [&](const auto& node) { return node.errorCode == code; }), code);
}

}

int main() {
    try {
        FixtureReader reader;
        FixtureProvider provider;
        FleetAgent rules(reader);
        FleetAgent assisted(reader, &provider);
        require(skillCatalog().size() == 19 && findSkill(getTaskStatus.id, 1) &&
                findSkill(getTaskStatus.id, 1)->effect == SkillEffect::read &&
                !findSkill(proposeSimReschedule.id, 2), "versioned skill catalog");

        require(!findSkill("fleet.nav.dispatch", 1) &&
                findSkill("fleet.task.reassign", 1)->effect == SkillEffect::proposal &&
                findSkill("study.log_progress", 1)->effect == SkillEffect::external &&
                findSkill("study.log_progress", 1)->idempotencyKey == "idempotencyKey",
                "effect-separated bounded skill catalog");
        for (const auto& skill : skillCatalog()) {
            require(!skill.parameterSchema.empty() && !skill.preconditions.empty() && skill.timeoutMs > 0,
                    "skills carry constraints, not prompts");
        }

        // Multi-version lookup and duplicate admission are separate invariants.
        auto v2 = getTaskStatus; v2.version=2;
        const std::array versions{getTaskStatus,v2};
        validateCatalog(versions);
        require(findSkill(versions,getTaskStatus.id,2)==&versions[1] && !findSkill(getTaskStatus.id,2), "lookup exact version");
        bool duplicate=false;
        try { const std::array bad{getTaskStatus,getTaskStatus}; validateCatalog(bad); }
        catch (const std::invalid_argument&) { duplicate=true; }
        require(duplicate,"reject duplicate skill identities");
        require(validateArguments(getTaskStatus,{{"taskId","failed"}}) &&
                !validateArguments(getTaskStatus,{{"taskId","failed"},{"extra",true}}) &&
                !validateArguments(getTaskStatus,{}), "execute typed parameter constraints");
        require(!validateArguments(*findSkill("chat.recall",1),{{"query","x"},{"scope","conversation"},{"limit",10}}), "scope requires conversation");
        require(fleetProposal("fleet.review_failures",1,{"failed","uncertain"}) ==
                fleetProposal("fleet.review_failures",1,{"uncertain","failed"}), "evidence is set semantics");
        unsigned cases = 0;
        auto run = [&](FleetAgent& agent, const Request& request, std::stop_token stop = {}) {
            return agent.run(request, {.runId = "agent-test-" + std::to_string(++cases), .stop = stop});
        };
        const Request review{Intent::review, {"failed"}};
        const Request propose{Intent::simulateReschedule, {"queued"}};
        const auto reviewed = fleetProposal("fleet.review_failures", 1, {"failed"});
        const auto proposed = fleetProposal("fleet.propose_sim_reschedule", 1, {"queued"});

        const auto summary = run(rules, {Intent::review, {"failed", "finished", "uncertain"}});
        require(summary.proposal == fleetProposal("fleet.review_failures", 1, {"failed", "uncertain"}) &&
                summary.evidence.size() == 3 && summary.planner == PlannerSource::rules, "review only relevant evidence");
        require(run(rules, {Intent::review, {"disconnected"}}).proposal ==
                fleetProposal("fleet.review_failures", 1, {"disconnected"}), "review interrupted acceptance");
        expectFailure(run(rules, {Intent::review, {"finished"}}), "no_failure_evidence");
        require(run(rules, propose).proposal == proposed, "queued task yields a simulation proposal");
        for (const char* id : {"leased", "executing", "uncertain"}) {
            expectFailure(run(rules, {Intent::simulateReschedule, {id}}), "task_not_eligible");
        }

        const auto beforeInvalid = reader.reads;
        for (const Request& request : std::vector<Request>{
                 {static_cast<Intent>(99), {"failed"}}, {Intent::review, {}}, {Intent::review, {"failed", "failed"}},
                 {Intent::review, {"failed", "finished", "leased", "uncertain"}},
                 {Intent::simulateReschedule, {"queued", "failed"}}, {Intent::review, {"not\"valid"}},
             }) {
            expectFailure(run(rules, request), "invalid_request");
        }
        require(reader.reads == beforeInvalid, "invalid requests never reach the evidence tool");

        const auto missing = run(rules, {Intent::review, {"failed", "missing"}});
        expectFailure(missing, "unknown_task");
        require(missing.evidence.empty(), "do not expose a partial batch");
        reader.load = [](auto) { return EvidenceResult{ReadStatus::denied, {}}; };
        auto beforeRead = reader.reads;
        expectFailure(run(assisted, review), "evidence_denied");
        require(provider.calls == 0 && reader.reads == beforeRead + 1, "denied evidence is never retried or given to provider");

        beforeRead = reader.reads;
        reader.load = [](auto) { return EvidenceResult{ReadStatus::unavailable, {}}; };
        expectFailure(run(rules, review), "evidence_unavailable");
        require(reader.reads == beforeRead + 2, "transient read retry is bounded");
        beforeRead = reader.reads;
        reader.load = [&](auto id) {
            return reader.reads == beforeRead + 1 ? EvidenceResult{ReadStatus::unavailable, {}} : fixture(id);
        };
        require(run(rules, review).proposal == reviewed && reader.reads == beforeRead + 2, "retry reads a fresh snapshot");

        reader.load = [](auto) { return fixture("queued"); };
        expectFailure(run(rules, review), "invalid_evidence");
        reader.load = [](auto id) {
            auto result = fixture(id);
            result.evidence.traceId = "not-a-fleet-trace";
            return result;
        };
        expectFailure(run(rules, review), "invalid_evidence");
        reader.load = [](auto id) {
            auto result = fixture(id);
            result.evidence.outcome = "unsafe\nmetadata";
            return result;
        };
        expectFailure(run(rules, review), "invalid_evidence");
        reader.load = fixture;

        const auto callsBeforeStale = provider.calls;
        reader.load = [](auto id) {
            auto result = fixture(id);
            result.evidence.fresh = false;
            return result;
        };
        const auto stale = run(assisted, review);
        require(stale.proposal == reviewed && stale.planner == PlannerSource::fallback &&
                provider.calls == callsBeforeStale, "stale evidence bypasses provider");
        reader.load = [](auto id) {
            auto result = fixture(id);
            result.evidence.readAt -= std::chrono::seconds{31};
            return result;
        };
        const auto aged = run(assisted, review);
        require(aged.proposal == reviewed && aged.planner == PlannerSource::fallback &&
                !aged.evidence.front().fresh && provider.calls == callsBeforeStale,
                "old reader timestamp overrides a claimed fresh flag");
        reader.load = [](auto id) {
            auto result = fixture(id);
            result.evidence.source.clear();
            return result;
        };
        expectFailure(run(assisted, review), "invalid_evidence");
        reader.load = fixture;

        const auto good = run(assisted, review);
        require(good.proposal == reviewed && good.planner == PlannerSource::provider, "valid provider proposal");
        for (const Proposal& proposal : std::vector<Proposal>{
                 {"unknown.skill", 1, {"failed"}}, {"fleet.review_failures", 2, {"failed"}},
                 {"fleet.get_task_status", 1, {"failed"}}, {"study.log_progress",1,{"failed"}}, proposed,
                 {"fleet.review_failures", 1, {"invented-reference"}},
                 {"fleet.review_failures", 1, {"failed", "failed"}},
             }) {
            provider.output = {CandidateStatus::proposed, proposal};
            expectFailure(run(assisted, review), "proposal_rejected");
        }
        provider.output = {CandidateStatus::proposed, fleetProposal("fleet.review_failures",1,{"uncertain","failed"})};
        const auto reordered = run(assisted,{Intent::review,{"failed","uncertain"}});
        require(reordered.run.state == State::success && reordered.planner == PlannerSource::provider,
                "provider evidence order is irrelevant");
        provider.output.proposal.parameters.clear();
        expectFailure(run(assisted,{Intent::review,{"failed","uncertain"}}),"proposal_rejected");
        provider.output = {CandidateStatus::malformed, {}};
        expectFailure(run(assisted, review), "provider_malformed");
        provider.output = {CandidateStatus::unavailable, {}};
        const auto fallback = run(assisted, review);
        require(fallback.proposal == reviewed && fallback.planner == PlannerSource::fallback, "offline provider fallback");
        provider.output = {CandidateStatus::proposed, {"invented-skill", 1, {"invented-reference"}}};
        provider.waitUntilDeadline = true;
        const auto late = run(assisted, review);
        require(late.proposal == reviewed && late.planner == PlannerSource::fallback, "ignore provider after its deadline");
        provider.waitUntilDeadline = false;

        beforeRead = reader.reads;
        std::stop_source stopped;
        stopped.request_stop();
        const auto cancelled = run(rules, review, stopped.get_token());
        require(cancelled.run.state == State::cancelled && !cancelled.proposal && reader.reads == beforeRead,
                "cancelled before reading evidence");

        std::vector<std::string> events;
        const auto rejected = assisted.run(review, {.runId = "agent-test-" + std::to_string(++cases),
            .onEvent = [&](const Event& event) { events.push_back(observerJson(event)); }});
        expectFailure(rejected, "proposal_rejected");
        require(std::ranges::any_of(events, [](const auto& row) { return row.find("proposal_rejected") != row.npos; }) &&
                std::ranges::none_of(events, [](const auto& row) {
                    return row.find("invented-skill") != row.npos || row.find("invented-reference") != row.npos ||
                           row.find("precondition_failed") != row.npos;
                }), "Observer explains decisions without evidence or provider payloads");

        std::cout << "Fleet Agent contract passed: " << cases << " fixed proposal and evidence cases\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Fleet Agent contract failed: " << error.what() << '\n';
        return 1;
    }
}
