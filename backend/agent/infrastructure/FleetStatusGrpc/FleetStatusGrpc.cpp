#include "FleetStatusGrpc.h"

#include <cstdlib>
#include <grpcpp/client_context.h>

namespace nebula::agent {
namespace {

EvidenceResult decodeEvidence(const fleet::v1::TaskStatus& task) {
    const auto& command = task.command();
    if (!task.has_command() || !fleet::validStation(static_cast<fleet::Station>(command.station())) ||
        (!task.robot_id().empty() && !fleet::validId(task.robot_id()))) return {};
    return {ReadStatus::found,
            {command.task_id(), command.trace_id(), static_cast<fleet::Phase>(task.phase()), command.epoch(),
             !task.robot_id().empty(), task.needs_reconciliation(), task.outcome()}};
}

}

FleetStatusGrpc::FleetStatusGrpc(std::string_view address, bool requireReader)
    : stub_(fleet::v1::Fleet::NewStub(fleet::wire::localChannel(address))),
      token_(fleet::wire::environmentToken(requireReader || std::getenv(fleet::wire::agentReadEnv)
                                              ? fleet::wire::agentReadEnv : "NEBULA_FLEET_OPERATOR_TOKEN")) {}

EvidenceResult FleetStatusGrpc::read(std::string_view taskId, std::stop_token stop,
                                    std::chrono::steady_clock::time_point deadline) {
    using namespace std::chrono;
    const auto remaining = deadline - steady_clock::now();
    if (stop.stop_requested() || remaining <= steady_clock::duration::zero()) {
        return {ReadStatus::unavailable, {}};
    }

    grpc::ClientContext context;
    context.AddMetadata(fleet::wire::tokenHeader, token_);
    context.set_deadline(system_clock::now() + ceil<milliseconds>(remaining));
    const std::stop_callback cancel(stop, [&context] { context.TryCancel(); });
    fleet::v1::TaskQuery query;
    query.set_task_id(taskId.data(), taskId.size());
    fleet::v1::TaskStatus task;
    const auto status = stub_->Status(&context, query, &task);
    switch (status.error_code()) {
    case grpc::StatusCode::OK: break;
    case grpc::StatusCode::NOT_FOUND: return {ReadStatus::missing, {}};
    case grpc::StatusCode::UNAUTHENTICATED:
    case grpc::StatusCode::PERMISSION_DENIED: return {ReadStatus::denied, {}};
    case grpc::StatusCode::UNAVAILABLE:
    case grpc::StatusCode::DEADLINE_EXCEEDED:
    case grpc::StatusCode::CANCELLED: return {ReadStatus::unavailable, {}};
    default: return {ReadStatus::invalid, {}};
    }

    return decodeEvidence(task);
}

std::pair<std::vector<TaskEvidence>, std::int64_t> FleetStatusGrpc::list(std::int64_t beforeEpoch, unsigned limit) {
    if (beforeEpoch < 0 || limit < 1 || limit > 16) throw std::invalid_argument("invalid task page");
    grpc::ClientContext context;
    context.AddMetadata(fleet::wire::tokenHeader, token_);
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds{800});
    fleet::v1::TaskPageRequest query;
    query.set_before_epoch(beforeEpoch);
    query.set_limit(limit);
    fleet::v1::TaskPage reply;
    if (!stub_->ListTasks(&context, query, &reply).ok() || reply.tasks_size() > static_cast<int>(limit)) {
        throw std::runtime_error("task list unavailable or denied");
    }
    std::vector<TaskEvidence> items;
    for (const auto& row : reply.tasks()) {
        auto decoded = decodeEvidence(row);
        if (decoded.status != ReadStatus::found || !validEvidence(decoded.evidence, row.command().task_id()) ||
            (beforeEpoch && decoded.evidence.epoch >= beforeEpoch)) throw std::runtime_error("invalid task page");
        beforeEpoch = decoded.evidence.epoch;
        items.push_back(std::move(decoded.evidence));
    }
    if (reply.next_before_epoch() && (items.size() != limit || items.back().epoch != reply.next_before_epoch())) {
        throw std::runtime_error("invalid task page cursor");
    }
    return {std::move(items), reply.next_before_epoch()};
}

}
