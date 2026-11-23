#include "FleetReviewGrpc.h"

#include <chrono>
#include <grpcpp/client_context.h>

namespace nebula::agent {
namespace {

std::string_view reviewError(grpc::StatusCode code) {
    switch (code) {
    case grpc::StatusCode::UNAUTHENTICATED:
    case grpc::StatusCode::PERMISSION_DENIED: return "review_denied";
    case grpc::StatusCode::UNIMPLEMENTED: return "review_disabled";
    case grpc::StatusCode::NOT_FOUND: return "review_missing";
    case grpc::StatusCode::FAILED_PRECONDITION: return "review_conflict";
    case grpc::StatusCode::INVALID_ARGUMENT: return "invalid_review";
    case grpc::StatusCode::RESOURCE_EXHAUSTED: return "review_capacity";
    case grpc::StatusCode::UNAVAILABLE:
    case grpc::StatusCode::DEADLINE_EXCEEDED:
    case grpc::StatusCode::CANCELLED: return "review_outcome_unknown";
    default: return "review_failed";
    }
}

ReviewResult checked(const grpc::Status& result, const fleet::v1::SimulationReview& message, std::string_view traceId) {
    if (!result.ok()) return {{}, reviewError(result.error_code())};
    const auto& draft = message.draft();
    const auto station = static_cast<fleet::Station>(message.station());
    const auto state = static_cast<fleet::ReviewState>(message.state());
    const auto& replacement = message.replacement_task_id();
    if (draft.trace_id() != traceId || !fleet::validId(draft.source_task_id()) || draft.source_epoch() <= 0 ||
        !fleet::validStation(station) || fleet::name(state) == "invalid" || message.created_at() <= 0 ||
        message.expires_at() <= message.created_at() ||
        (state == fleet::ReviewState::approved ? replacement != std::string(fleet::replacementPrefix) + draft.trace_id()
                                              : !replacement.empty())) return {{}, "invalid_review_response"};
    return {fleet::SimulationReview{{draft.trace_id(), draft.source_task_id(), draft.source_epoch()}, station, state,
                                    message.created_at(), message.expires_at(), replacement}, {}};
}

}

FleetReviewGrpc::FleetReviewGrpc(std::string_view address, const char* tokenVariable)
    : stub_(fleet::v1::Fleet::NewStub(fleet::wire::localChannel(address))),
      token_(fleet::wire::environmentToken(tokenVariable)) {}

void FleetReviewGrpc::authenticate(grpc::ClientContext& context) const {
    context.AddMetadata(fleet::wire::tokenHeader, token_);
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds{1500});
}

ReviewResult FleetReviewGrpc::stage(const fleet::ReviewDraft& draft) {
    grpc::ClientContext context;
    authenticate(context);
    fleet::v1::ReviewDraft request;
    request.set_trace_id(draft.traceId);
    request.set_source_task_id(draft.sourceTaskId);
    request.set_source_epoch(draft.sourceEpoch);
    fleet::v1::SimulationReview reply;
    const auto status = stub_->StageReview(&context, request, &reply);
    auto result = checked(status, reply, draft.traceId);
    if (result.review && result.review->draft != draft) return {{}, "invalid_review_response"};
    return result;
}

ReviewResult FleetReviewGrpc::inspect(std::string_view traceId) {
    grpc::ClientContext context;
    authenticate(context);
    fleet::v1::ReviewQuery request;
    request.set_trace_id(traceId.data(), traceId.size());
    fleet::v1::SimulationReview reply;
    const auto status = stub_->InspectReview(&context, request, &reply);
    return checked(status, reply, traceId);
}

ReviewResult FleetReviewGrpc::decide(std::string_view traceId, fleet::Decision decision) {
    grpc::ClientContext context;
    authenticate(context);
    fleet::v1::ReviewRequest request;
    request.set_trace_id(traceId.data(), traceId.size());
    request.set_decision(static_cast<fleet::v1::ReviewDecision>(decision));
    fleet::v1::SimulationReview reply;
    const auto status = stub_->DecideReview(&context, request, &reply);
    return checked(status, reply, traceId);
}

}
