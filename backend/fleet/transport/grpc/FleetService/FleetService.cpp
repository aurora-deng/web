#include "FleetService.h"

#include "backend/workflow/WorkflowGraph/WorkflowGraph.h"

#include <utility>

namespace nebula::fleet::wire {
using grpc::Status;
using grpc::StatusCode;
using namespace std::chrono_literals;

namespace {
std::string_view metadata(grpc::ServerContext& context, const char* key) {
    const auto [first, last] = context.client_metadata().equal_range(key);
    if (first == last || std::next(first) != last) return {};
    return {first->second.data(), first->second.size()};
}

Status failure(const Error& error) {
    switch (error.code()) {
    case ErrorCode::invalid: return {StatusCode::INVALID_ARGUMENT, error.what()};
    case ErrorCode::missing: return {StatusCode::NOT_FOUND, error.what()};
    case ErrorCode::conflict: return {StatusCode::FAILED_PRECONDITION, error.what()};
    case ErrorCode::exhausted: return {StatusCode::RESOURCE_EXHAUSTED, error.what()};
    case ErrorCode::offline: return {StatusCode::UNAVAILABLE, error.what()};
    case ErrorCode::storage: return {StatusCode::INTERNAL, "fleet ledger unavailable"};
    }
    return {StatusCode::INTERNAL, "fleet request failed"};
}

template<class Call>
Status handle(Call&& call) {
    try { return call(); }
    catch (const Error& error) { return failure(error); }
    catch (const std::exception&) { return {StatusCode::INTERNAL, "fleet request failed"}; }
}

struct Connected {
    FleetCoordinator& coordinator;
    Session session;
    ~Connected() { coordinator.disconnect(session); }
};
}

FleetService::FleetService(FleetCoordinator& coordinator, std::string operatorToken, Robots robots)
    : coordinator_(coordinator), operatorToken_(std::move(operatorToken)), robots_(std::move(robots)) {}

grpc::Status FleetService::Submit(grpc::ServerContext* context, const v1::TaskRequest* in, v1::TaskStatus* out) {
    if (!tokenMatches(metadata(*context, tokenHeader), operatorToken_)) {
        return {StatusCode::UNAUTHENTICATED, "operator credential required"};
    }
    return handle([&] {
        encode(coordinator_.submit(in->task_id(), static_cast<Station>(in->station()), workflow::newTraceId()), *out);
        return grpc::Status::OK;
    });
}

grpc::Status FleetService::Status(grpc::ServerContext* context, const v1::TaskQuery* in, v1::TaskStatus* out) {
    if (!tokenMatches(metadata(*context, tokenHeader), operatorToken_)) {
        return {StatusCode::UNAUTHENTICATED, "operator credential required"};
    }
    return handle([&] {
        encode(coordinator_.status(in->task_id()), *out);
        return grpc::Status::OK;
    });
}

grpc::Status FleetService::Exchange(grpc::ServerContext* context,
                                     grpc::ServerReaderWriter<v1::CoordinatorFrame, v1::EdgeFrame>* stream) {
    const auto id = metadata(*context, robotHeader);
    const auto robot = robots_.find(id);
    if (robot == robots_.end() || !tokenMatches(metadata(*context, tokenHeader), robot->second.token)) {
        return {StatusCode::UNAUTHENTICATED, "robot credential required"};
    }
    if (context->deadline() > std::chrono::system_clock::now() + 5s) {
        return {StatusCode::INVALID_ARGUMENT, "robot stream deadline is required"};
    }
    return handle([&]() -> grpc::Status {
        v1::EdgeFrame frame;
        if (!stream->Read(&frame) || !frame.has_hello() || frame.hello().version() != protocolVersion ||
            !validId(frame.hello().boot_id()) || !validCapabilities(frame.hello().stations()) ||
            (frame.hello().stations() & ~robot->second.allowed) != 0) {
            return {StatusCode::INVALID_ARGUMENT, "invalid robot handshake"};
        }
        const auto attach = [&] {
            try { return coordinator_.connect(std::string(id), frame.hello().stations()); }
            catch (const Error& error) {
                if (error.code() == ErrorCode::conflict) {
                    throw Error(ErrorCode::offline, "previous stream still owns robot session");
                }
                throw;
            }
        };
        Connected connection{coordinator_, attach()};
        do {
            std::optional<Receipt> receipt;
            if (frame.has_receipt()) receipt = decode(frame.receipt());
            v1::CoordinatorFrame response;
            response.set_version(protocolVersion);
            const auto command = coordinator_.exchange(connection.session, receipt);
            if (receipt) encode(*receipt, *response.mutable_confirmed());
            if (command) encode(*command, *response.mutable_command());
            if (!stream->Write(response)) break;
            if (!stream->Read(&frame)) break;
            if (!frame.has_heartbeat() && !frame.has_receipt()) {
                return {StatusCode::INVALID_ARGUMENT, "unexpected frame after handshake"};
            }
        } while (!context->IsCancelled());
        return grpc::Status::OK;
    });
}
}
