#include "../../application/FleetAgent/FleetAgent.h"
#include "../../infrastructure/FleetStatusGrpc/FleetStatusGrpc.h"
#include "FleetApprovalCommand.h"
#include "backend/workflow/ObserverEvents/ObserverEvents.h"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <utility>

namespace agent = nebula::agent;
namespace fleet = nebula::fleet;
namespace workflow = nebula::workflow;

namespace {

class SuppliedCandidate final : public agent::ProposalProvider {
public:
    explicit SuppliedCandidate(agent::Proposal proposal) : proposal_(std::move(proposal)) {}
    agent::Candidate plan(agent::Intent, std::span<const agent::TaskEvidence>, std::stop_token,
                          workflow::Clock::time_point) override {
        return {agent::CandidateStatus::proposed, proposal_};
    }
private:
    agent::Proposal proposal_;
};

agent::Proposal candidateFrom(std::string_view skill, std::string_view version, std::string_view ids) {
    unsigned number{};
    const auto end = version.data() + version.size();
    const auto parsed = std::from_chars(version.data(), end, number);
    if (!fleet::validId(skill) || parsed.ec != std::errc{} || parsed.ptr != end || !number) {
        throw fleet::Error(fleet::ErrorCode::invalid, "invalid candidate skill and version");
    }
    agent::Proposal proposal{std::string(skill), number, {}};
    for (;;) {
        const auto separator = ids.find(',');
        const auto id = ids.substr(0, separator);
        if (proposal.evidenceTaskIds.size() >= agent::maxReviewTasks || !fleet::validId(id)) {
            throw fleet::Error(fleet::ErrorCode::invalid, "invalid candidate evidence list");
        }
        proposal.evidenceTaskIds.emplace_back(id);
        if (separator == std::string_view::npos) return agent::fleetProposal(proposal.skillId, proposal.version, std::move(proposal.evidenceTaskIds));
        ids.remove_prefix(separator + 1);
    }
}
void printAnswer(const agent::Answer& answer) {
    const auto& run = answer.run;
    const auto failed = std::ranges::find_if(run.nodes, [](const auto& node) { return !node.errorCode.empty(); });
    const auto error = failed == run.nodes.end() ? std::string{} : failed->errorCode;
    std::cout << "{\"state\":" << std::quoted(workflow::stateName(run.state))
              << ",\"traceId\":" << std::quoted(run.traceId)
              << ",\"planner\":" << std::quoted(agent::plannerName(answer.planner))
              << ",\"errorCode\":" << std::quoted(error) << ",\"evidence\":[";
    bool first = true;
    for (const auto& task : answer.evidence) {
        if (!std::exchange(first, false)) std::cout << ',';
        std::cout << "{\"taskId\":" << std::quoted(task.taskId) << ",\"traceId\":" << std::quoted(task.traceId)
                  << ",\"phase\":" << std::quoted(fleet::name(task.phase)) << ",\"epoch\":" << task.epoch
                  << ",\"needsReconciliation\":" << (task.needsReconciliation ? "true" : "false")
                  << ",\"outcome\":" << std::quoted(task.outcome) << '}';
    }
    std::cout << "],\"proposal\":";
    if (const auto& proposal = answer.proposal) {
        std::cout << "{\"skillId\":" << std::quoted(proposal->skillId) << ",\"version\":" << proposal->version
                  << ",\"evidenceTaskIds\":[";
        first = true;
        for (const auto& taskId : proposal->evidenceTaskIds) {
            if (!std::exchange(first, false)) std::cout << ',';
            std::cout << std::quoted(taskId);
        }
        std::cout << "]}";
    } else {
        std::cout << "null";
    }
    std::cout << "}\n";
}

}

namespace {

int invoke(int argc, char* argv[], workflow::RunOptions options) {
    int first = 1;
    std::optional<SuppliedCandidate> supplied;
    if (first < argc && std::string_view(argv[first]) == "--events") ++first;
    if (first + 3 < argc && std::string_view(argv[first]) == "--candidate") {
        supplied.emplace(candidateFrom(argv[first + 1], argv[first + 2], argv[first + 3]));
        first += 4;
    }
    if (first + 1 < argc && std::string_view(argv[first]) == "--trace-id") {
        options.traceId = argv[first + 1];
        if (!fleet::validTrace(options.traceId)) throw fleet::Error(fleet::ErrorCode::invalid, "invalid Agent trace ID");
        first += 2;
    }
    const auto action = argc > first + 1 ? std::string_view(argv[first + 1]) : "";
    const bool approval = action == "inspect" || action == "approve" || action == "reject";
    if (argc == first + 3 && approval && !supplied && options.traceId.empty()) {
        options.traceId = argv[first + 2];
        if (!fleet::validTrace(options.traceId)) throw fleet::Error(fleet::ErrorCode::invalid, "invalid proposal ID");
        return agent::cli::review(argv[first], action, std::move(options));
    }
    if (argc < first + 3 || argc > first + 5 || (action != "review" && action != "propose" && action != "stage")) {
        std::cerr << "usage: nebula_fleet_agent [--events] [--candidate SKILL VERSION ID[,ID]] [--trace-id TRACE] "
                     "127.0.0.1:PORT (review ID [ID ID] | propose ID | stage ID | inspect|approve|reject PROPOSAL)\n";
        return 1;
    }

    agent::Request request{action == "review" ? agent::Intent::review : agent::Intent::simulateReschedule, {}};
    for (int i = first + 2; i < argc; ++i) request.taskIds.emplace_back(argv[i]);
    agent::FleetStatusGrpc reader(argv[first], action == "stage");
    agent::FleetAgent runner(reader, supplied ? &*supplied : nullptr);
    const auto answer = runner.run(request, options);
    if (action == "stage" && answer.proposal) {
        const auto& source = answer.evidence.front();
        options.runId = "stage-" + workflow::newTraceId();
        return agent::cli::stageReview(argv[first], {answer.run.traceId, source.taskId, source.epoch}, std::move(options));
    }
    printAnswer(answer);
    return answer.proposal ? 0 : 2;
}

}

int main(int argc, char* argv[]) {
    try {
        workflow::RunOptions options{.runId = "agent-" + workflow::newTraceId()};
        std::vector<std::string> events;
        if (argc > 1 && std::string_view(argv[1]) == "--events") {
            events.reserve(64);
            options.onEvent = [&](const workflow::Event& event) {
                if (events.size() < 64) events.push_back(workflow::observerJson(event));
            };
        }
        const auto code = invoke(argc, argv, std::move(options));
        for (const auto& event : events) std::cerr << event << '\n';
        return code;
    } catch (const std::exception& error) {
        std::cerr << "fleet agent: " << error.what() << '\n';
        return 1;
    }
}
