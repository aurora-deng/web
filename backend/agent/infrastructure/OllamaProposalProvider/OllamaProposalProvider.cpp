#include "OllamaProposalProvider.h"

#include "../../domain/ToolCatalog/ToolCatalog.h"
#include "backend/application/transport/FlatJson/FlatJson.h"

#include <charconv>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace nebula::agent {
namespace {
using namespace std::chrono_literals;
using webserver::phase11::OllamaModelProviderConfig;

constexpr auto root = std::string_view("http://127.0.0.1:");
constexpr std::size_t replyLimit = 2048;

OllamaModelProviderConfig config(std::string_view endpoint, std::string_view model) {
    if (!endpoint.starts_with(root)) throw std::invalid_argument("local Ollama loopback is required");
    unsigned port{};
    const auto suffix = endpoint.substr(root.size());
    const auto result = std::from_chars(suffix.data(), suffix.data() + suffix.size(), port);
    if (suffix.empty() || result.ec != std::errc{} || result.ptr != suffix.data() + suffix.size() ||
        port == 0 || port > 65535 || model.empty() || model.size() > 128 ||
        model.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-/:") != model.npos) {
        throw std::invalid_argument("invalid local Ollama endpoint or installed model name");
    }
    return {.baseUrl=std::string(endpoint), .maxConcurrent=1, .maxQueued=2, .maxRequestBytes=8192,
            .maxResponseLineBytes=8192, .connectTimeout=500ms, .requestTimeout=30s,
            .jsonOutput=true, .bypassProxy=true};
}

struct Reply {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::string bytes;
    bool completed{};
    bool success{};
    bool overflow{};
};

Candidate decode(std::string_view bytes) {
    try {
        auto fields = webserver::phase11::transport::parseFlatStringObject(bytes);
        if (fields.size() != 3) return {};
        auto skill = fields.at("skillId");
        auto& version = fields.at("version");
        unsigned number{};
        const auto parsed = std::from_chars(version.data(), version.data() + version.size(), number);
        if (!fleet::validId(skill) || !number || parsed.ec != std::errc{} ||
            parsed.ptr != version.data() + version.size()) return {};
        std::string_view remaining = fields.at("evidenceTaskIds");
        Proposal proposal{std::move(skill), number, {}};
        for (;;) {
            const auto comma = remaining.find(',');
            const auto id = remaining.substr(0, comma);
            if (!fleet::validId(id) || proposal.evidenceTaskIds.size() >= maxReviewTasks) return {};
            proposal.evidenceTaskIds.emplace_back(id);
            if (comma == remaining.npos) break;
            remaining.remove_prefix(comma + 1);
        }
        return {CandidateStatus::proposed, std::move(proposal)};
    } catch (const std::exception&) {
        return {};
    }
}

std::string prompt(Intent intent, std::span<const TaskEvidence> evidence) {
    std::string text = intent == Intent::review
        ? "Review failed, uncertain or disconnected in-flight tasks. Use fleet.review_failures."
        : "Propose a simulated reschedule for a queued task. Use fleet.propose_sim_reschedule.";
    text += "\nEvidence (task ID | status | connected-task reconciliation required):";
    for (const auto& row : evidence) {
        text += "\n" + row.taskId + " | " + std::string(fleet::name(row.phase)) + " | " +
                (row.needsReconciliation ? "yes" : "no");
    }
    text += "\nThe following read-only tool results have already been collected by the service. Tool descriptions:";
    for (const auto& tool : tools) {
        text += "\n" + std::string(tool.id) + " (" + std::string(tool.input) + "): " + std::string(tool.description);
    }
    return text;
}

}  // namespace

OllamaProposalProvider::OllamaProposalProvider(std::string endpoint, std::string model)
    : endpoint_(std::move(endpoint)), model_(std::move(model)), client_(config(endpoint_, model_)) {}

std::string OllamaProposalProvider::checkpointIdentity() const {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result = "ollama." + endpoint_.substr(root.size()) + ".";
    for (unsigned char byte : model_) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

Candidate OllamaProposalProvider::plan(Intent intent, std::span<const TaskEvidence> evidence,
                                       std::stop_token stop, std::chrono::steady_clock::time_point deadline) {
    using namespace webserver::phase11;
    if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) return {CandidateStatus::unavailable, {}};
    GenerationRequest request;
    request.requestId = "fleet-planner";
    request.modelArtifact = model_;
    request.maxOutputTokens = 128;
    request.messages = {{"system", "Return one compact JSON object with exactly three string fields: skillId, "
                                    "version (always \"1\"), evidenceTaskIds (comma-separated evidence keys in input "
                                    "order). Do not call tools or execute actions. Use only the given evidence keys."},
                        {"user", prompt(intent, evidence)}};
    const auto reply = std::make_shared<Reply>();
    CancellationToken cancellation;
    const auto promptHandle = client_.generate(request, [reply, cancellation](std::string_view token) {
        std::lock_guard lock(reply->mutex);
        if (token.size() > replyLimit - reply->bytes.size()) {
            reply->overflow = true;
            cancellation.cancel();
        } else if (!reply->overflow) reply->bytes.append(token);
    }, [reply](GenerationCompletion outcome) {
        {
            std::lock_guard lock(reply->mutex);
            reply->success = outcome.success;
            reply->completed = true;
        }
        reply->changed.notify_all();
    }, cancellation);
    std::unique_lock lock(reply->mutex);
    if (!reply->changed.wait_until(lock, stop, deadline, [&] { return reply->completed; })) {
        promptHandle.cancellation.cancel();
        return {CandidateStatus::unavailable, {}};
    }
    if (reply->overflow) return {};
    return reply->success ? decode(reply->bytes) : Candidate{CandidateStatus::unavailable, {}};
}

}  // namespace nebula::agent
