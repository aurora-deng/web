#include "../../application/FleetAgent/FleetAgent.h"
#include "../../domain/ToolCatalog/ToolCatalog.h"
#include "../../infrastructure/FleetStatusGrpc/FleetStatusGrpc.h"
#include "../../infrastructure/SqliteAgentSession/SqliteAgentSession.h"
#ifdef NEBULA_AGENT_OLLAMA
#include "../../infrastructure/OllamaProposalProvider/OllamaProposalProvider.h"
#endif
#include "FleetApprovalCommand.h"
#include "backend/systems/SqliteWorkflowJournal/SqliteWorkflowJournal.h"
#include "backend/workflow/ObserverEvents/ObserverEvents.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
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
    std::string checkpointIdentity() const override {
        std::string result = "supplied." + std::to_string(proposal_.skillId.size()) + "." +
                             proposal_.skillId + "." + std::to_string(proposal_.version);
        for (const auto& id : proposal_.evidenceTaskIds) result += "." + std::to_string(id.size()) + "." + id;
        return result;
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
        if (separator == std::string_view::npos) return proposal;
        ids.remove_prefix(separator + 1);
    }
}
void printEvidence(const agent::TaskEvidence& task) {
    std::cout << "{\"taskId\":" << std::quoted(task.taskId) << ",\"traceId\":" << std::quoted(task.traceId)
              << ",\"phase\":" << std::quoted(fleet::name(task.phase)) << ",\"epoch\":" << task.epoch
              << ",\"needsReconciliation\":" << (task.needsReconciliation ? "true" : "false")
              << ",\"outcome\":" << std::quoted(task.outcome) << '}';
}

void printAnswer(const agent::Answer& answer, std::size_t remembered) {
    const auto& run = answer.run;
    const auto failed = std::ranges::find_if(run.nodes, [](const auto& node) { return !node.errorCode.empty(); });
    const auto error = failed == run.nodes.end() ? std::string{} : failed->errorCode;
    std::cout << "{\"state\":" << std::quoted(workflow::stateName(run.state))
              << ",\"traceId\":" << std::quoted(run.traceId)
              << ",\"planner\":" << std::quoted(agent::plannerName(answer.planner))
              << ",\"errorCode\":" << std::quoted(error) << ",\"recalledTaskCount\":" << remembered
              << ",\"evidence\":[";
    bool first = true;
    for (const auto& task : answer.evidence) {
        if (!std::exchange(first, false)) std::cout << ',';
        printEvidence(task);
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

template<class Number>
Number number(std::string_view value) {
    Number result{};
    const auto end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end) throw std::invalid_argument("invalid numeric argument");
    return result;
}

int readTool(int argc, char* argv[], int first, std::string_view action) {
    agent::FleetStatusGrpc reader(argv[first]);
    if (action == "list") {
        if (argc > first + 4) throw std::invalid_argument("list accepts at most a cursor and a limit");
        const auto cursor = argc > first + 2 ? number<std::int64_t>(argv[first + 2]) : 0;
        const auto limit = argc > first + 3 ? number<unsigned>(argv[first + 3]) : 16;
        const auto [items, next] = reader.list(cursor, limit);
        std::cout << "{\"nextBeforeEpoch\":" << next << ",\"items\":[";
        bool firstItem = true;
        for (const auto& item : items) {
            if (!std::exchange(firstItem, false)) std::cout << ',';
            printEvidence(item);
        }
        std::cout << "]}\n";
        return 0;
    }
    if (argc != first + 3 || !fleet::validId(argv[first + 2])) throw std::invalid_argument("invalid task ID");
    const auto result = reader.read(argv[first + 2], {}, workflow::Clock::now() + workflow::Milliseconds{800});
    if (result.status != agent::ReadStatus::found || !agent::validEvidence(result.evidence, argv[first + 2])) {
        throw std::runtime_error("task evidence unavailable or denied");
    }
    printEvidence(result.evidence);
    std::cout << '\n';
    return 0;
}

int invoke(int argc, char* argv[], workflow::RunOptions options) {
    int first = 1;
    std::unique_ptr<agent::ProposalProvider> provider;
    std::filesystem::path journalFile = "fleet-agent-workflow.sqlite";
    std::string session;
    bool resume = false;
    while (first < argc && std::string_view(argv[first]).starts_with("--")) {
        const std::string_view flag = argv[first++];
        if (flag == "--events") continue;
        if (first >= argc) throw std::invalid_argument("missing Agent option value");
        if (flag == "--journal") journalFile = argv[first++];
        else if (flag == "--session") session = argv[first++];
        else if (flag == "--trace-id") {
            options.traceId = argv[first++];
            if (!fleet::validTrace(options.traceId)) throw std::invalid_argument("invalid Agent trace ID");
        } else if (flag == "--run-id") {
            options.runId = argv[first++];
            resume = true;
        } else if (!provider && flag == "--candidate" && first + 2 < argc) {
            provider = std::make_unique<SuppliedCandidate>(candidateFrom(argv[first], argv[first + 1], argv[first + 2]));
            first += 3;
        } else if (!provider && flag == "--ollama") {
#ifdef NEBULA_AGENT_OLLAMA
            const auto* endpoint = std::getenv("NEBULA_AGENT_OLLAMA_URL");
            provider = std::make_unique<agent::OllamaProposalProvider>(endpoint ? endpoint : "http://127.0.0.1:11434",
                                                                       argv[first++]);
            options.timeout = workflow::Milliseconds{16000};
#else
            throw std::invalid_argument("build with NEBULA_AGENT_OLLAMA to configure this planner");
#endif
        } else throw std::invalid_argument("invalid or repeated Agent option");
    }
    if (argc == first + 1 && std::string_view(argv[first]) == "tools") {
        std::cout << '[';
        bool firstTool = true;
        for (const auto& tool : agent::tools) {
            if (!std::exchange(firstTool, false)) std::cout << ',';
            std::cout << "{\"id\":" << std::quoted(tool.id) << ",\"effect\":\"read\",\"description\":"
                      << std::quoted(tool.description) << ",\"input\":" << std::quoted(tool.input) << '}';
        }
        std::cout << "]\n";
        return 0;
    }
    const auto action = argc > first + 1 ? std::string_view(argv[first + 1]) : "";
    const bool approval = action == "inspect" || action == "approve" || action == "reject";
    if (action == "list" || action == "status" || action == "evidence") {
        if (provider || resume || !session.empty()) throw std::invalid_argument("unexpected option for a read tool");
        return readTool(argc, argv, first, action);
    }
    if (argc == first + 3 && (approval || action == "reconcile") && !provider && options.traceId.empty()) {
        options.traceId = argv[first + 2];
        if (!fleet::validTrace(options.traceId)) throw fleet::Error(fleet::ErrorCode::invalid, "invalid proposal ID");
        if (action == "reconcile") {
            nebula::systems::SqliteWorkflowJournal storage(journalFile);
            workflow::DurableWorkflow durable(storage, 1);
            return agent::cli::reconcileStage(argv[first], options.traceId, durable, storage);
        }
        return agent::cli::review(argv[first], action, std::move(options));
    }
    if (argc < first + 2 || argc > first + 5 || (action != "review" && action != "propose" && action != "stage") ||
        (argc == first + 2 && (action != "review" || session.empty()))) {
        std::cerr << "usage: nebula_fleet_agent [--events] [--ollama MODEL | --candidate SKILL VERSION IDS] "
                     "[--journal FILE] [--run-id ID] [--session SESSION] [--trace-id TRACE] HOST:PORT "
                     "(review [IDs] | propose ID | stage ID | inspect/approve/reject/reconcile PROPOSAL | "
                     "list [CURSOR LIMIT] | status/evidence ID)\n";
        return 1;
    }

    agent::Request request{action == "review" ? agent::Intent::review : agent::Intent::simulateReschedule, {}};
    for (int i = first + 2; i < argc; ++i) request.taskIds.emplace_back(argv[i]);
    agent::FleetStatusGrpc reader(argv[first], action == "stage");
    std::optional<agent::SqliteAgentSession> memory;
    if (!session.empty()) {
        auto memoryFile = journalFile;
        memoryFile += ".memory";
        memory.emplace(memoryFile, argv[first], session);
    }
    std::size_t recalled = 0;
    if (request.taskIds.empty() && memory) {
        request.taskIds = memory->context(agent::maxReviewTasks);
        recalled = request.taskIds.size();
    }
    // Restoring a checkpoint must not bypass a current read permission check.
    if (resume && agent::validRequest(request)) {
        for (const auto& id : request.taskIds) {
            const auto fresh = reader.read(id, {}, workflow::Clock::now() + workflow::Milliseconds{500});
            if (fresh.status != agent::ReadStatus::found) throw std::runtime_error("checkpoint evidence unavailable or denied");
        }
    }
    nebula::systems::SqliteWorkflowJournal storage(journalFile);
    workflow::DurableWorkflow durable(storage, 1);
    agent::FleetAgent runner(reader, durable, provider.get(), options.timeout > workflow::Milliseconds{5000} ?
                            workflow::Milliseconds{12000} : workflow::Milliseconds{700});
    const auto answer = runner.run(request, options);
    if (memory && answer.proposal) memory->remember(answer.run.traceId, answer.evidence);
    if (action == "stage" && answer.proposal) {
        const auto& source = answer.evidence.front();
        return agent::cli::stageReview(argv[first], {answer.run.traceId, source.taskId, source.epoch},
                                       std::move(options), durable);
    }
    printAnswer(answer, recalled);
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
