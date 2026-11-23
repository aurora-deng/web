#include "EdgeSession.h"

#include <algorithm>
#include <chrono>
#include <semaphore>
#include <utility>

namespace nebula::fleet::wire {
namespace {
using namespace std::chrono_literals;

void waitInterruptible(std::stop_token stop, std::chrono::milliseconds elapsed) {
    std::binary_semaphore cancelled(0);
    std::stop_callback onStop(stop, [&] { cancelled.release(); });
    const auto until = workflow::Clock::now() + elapsed;
    while (!stop.stop_requested() && workflow::Clock::now() < until) cancelled.try_acquire_until(until);
}

bool matches(const Receipt& a, const Receipt& b) {
    return a.taskId == b.taskId && a.epoch == b.epoch && a.phase == b.phase && a.outcome == b.outcome;
}

struct Peer {
    std::string robot;
    std::string token;
    std::string boot;
    Capabilities capabilities;
};

grpc::Status session(v1::Fleet::Stub& coordinator, const Peer& peer, EdgeLedger& ledger, EdgeWorker& worker,
                     bool& connected, std::stop_token stop) {
    grpc::ClientContext context;
    // A short bound also retires connected but silent peers and limits the
    // time during which an old transport can retain resources.
    context.set_deadline(std::chrono::system_clock::now() + 3s);
    context.AddMetadata(robotHeader, peer.robot);
    context.AddMetadata(tokenHeader, peer.token);
    std::stop_callback cancelled(stop, [&] { context.TryCancel(); });
    auto stream = coordinator.Exchange(&context);
    const auto rotateAt = workflow::Clock::now() + 2700ms;

    v1::EdgeFrame request;
    request.mutable_hello()->set_version(protocolVersion);
    request.mutable_hello()->set_boot_id(peer.boot);
    request.mutable_hello()->set_stations(peer.capabilities);
    declareRobotSkills(*request.mutable_hello());
    std::optional<Receipt> outstanding;
    while (!stop.stop_requested() && worker.healthy() && stream->Write(request)) {
        v1::CoordinatorFrame reply;
        if (!stream->Read(&reply)) break;
        if (reply.version() != protocolVersion || reply.has_confirmed() != outstanding.has_value()) {
            throw Error(ErrorCode::invalid, "incompatible coordinator response");
        }
        connected = true;
        if (outstanding) {
            if (!matches(decode(reply.confirmed()), *outstanding)) {
                throw Error(ErrorCode::conflict, "coordinator confirmed another receipt");
            }
            ledger.confirm(*outstanding);
        }
        if (reply.has_command()) {
            auto command = decode(reply.command());
            if (!hasStation(peer.capabilities, command.station)) {
                throw Error(ErrorCode::conflict, "coordinator sent unsupported station");
            }
            ledger.stage(command);
        }
        worker.permit();
        if (workflow::Clock::now() >= rotateAt) break;
        if (!reply.has_command()) waitInterruptible(stop, 15ms);
        outstanding = ledger.pendingReceipt();
        request.Clear();
        if (outstanding) encode(*outstanding, *request.mutable_receipt());
        else request.mutable_heartbeat();
    }
    stream->WritesDone();
    return stream->Finish();
}
}

void runEdgeSessions(std::string_view address, std::string_view robotId, std::string_view token,
                     Capabilities capabilities, EdgeLedger& ledger, EdgeWorker& worker, std::stop_token stop) {
    using namespace std::chrono_literals;
    if (!validId(robotId) || !validCapabilities(capabilities)) throw Error(ErrorCode::invalid, "invalid edge settings");
    const Peer peer{std::string(robotId), std::string(token), workflow::newTraceId(), capabilities};
    const auto grpc = v1::Fleet::NewStub(localChannel(address));
    auto backoff = 40ms;
    while (!stop.stop_requested() && worker.healthy()) {
        bool connected = false;
        const auto result = session(*grpc, peer, ledger, worker, connected, stop);
        if (stop.stop_requested()) break;
        if (result.error_code() == grpc::StatusCode::UNAUTHENTICATED ||
            result.error_code() == grpc::StatusCode::INVALID_ARGUMENT ||
            result.error_code() == grpc::StatusCode::FAILED_PRECONDITION ||
            result.error_code() == grpc::StatusCode::INTERNAL) {
            throw Error(ErrorCode::conflict, "fleet stream rejected; check configuration or reconcile ledger");
        }
        if (connected) backoff = 40ms;
        // A second robot naturally receives a different retry cadence.
        const auto jitter = std::chrono::milliseconds{static_cast<unsigned char>(peer.boot[0]) % 29};
        if (connected && result.ok()) {
            waitInterruptible(stop, jitter / 4);
            continue;
        }
        waitInterruptible(stop, backoff + jitter);
        backoff = std::min(backoff * 2, 500ms);
    }
    if (!worker.healthy()) throw Error(ErrorCode::storage, "edge journal execution failed; reconciliation required");
}
}
