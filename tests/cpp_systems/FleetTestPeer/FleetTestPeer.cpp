#include "backend/fleet/infrastructure/SqliteEdgeLedger/SqliteEdgeLedger.h"
#include "backend/fleet/domain/RobotSkills/RobotSkills.h"
#include "backend/fleet/transport/grpc/FleetWire/FleetWire.h"
#include "backend/workflow/WorkflowGraph/WorkflowGraph.h"

#include <chrono>
#include <iostream>
#include <optional>
#include <thread>

using namespace nebula::fleet;
using namespace nebula::fleet::wire;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void checkVersions(v1::Fleet::Stub& stub, const char* robot) {
    const auto token = environmentToken("NEBULA_FLEET_ROBOT_TOKEN");
    for (const auto wrongProtocol : {true, false}) {
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 3s);
        context.AddMetadata(robotHeader, robot);
        context.AddMetadata(tokenHeader, token);
        auto stream = stub.Exchange(&context);
        v1::EdgeFrame request;
        auto* hello = request.mutable_hello();
        hello->set_version(wrongProtocol ? protocolVersion + 1 : protocolVersion);
        hello->set_boot_id(nebula::workflow::newTraceId());
        hello->set_stations(1);
        declareRobotSkills(*hello);
        if (!wrongProtocol) hello->mutable_skills(0)->set_version(2);
        stream->Write(request);
        stream->WritesDone();
        v1::CoordinatorFrame response;
        require(!stream->Read(&response) && stream->Finish().error_code() == grpc::StatusCode::INVALID_ARGUMENT,
                "incompatible edge received a response");
    }

    const Command expected{"test", nebula::workflow::newTraceId(), Station::alpha, 6};
    v1::Command frame;
    encode(expected, frame);
    require(frame.has_skill() && decode(frame) == expected, "v1 skill declaration missing");
    frame.mutable_skill()->set_version(navigateToSimStation.version + 1);
    bool rejected = false;
    try { (void)decode(frame); }
    catch (const Error& error) { rejected = error.code() == ErrorCode::invalid; }
    require(rejected, "a new unsupported command version was decoded");
    frame.clear_skill();
    require(decode(frame) == expected, "legacy command was not accepted");
}

void watch(v1::Fleet::Stub& stub, bool expectFull) {
    const auto token = environmentToken("NEBULA_FLEET_OPERATOR_TOKEN");
    const auto until = std::chrono::steady_clock::now() + 30s;
    unsigned samples = 0;
    unsigned skipped = 0;
    do {
        grpc::ClientContext context;
        context.AddMetadata(tokenHeader, token);
        context.set_deadline(std::chrono::system_clock::now() + 4s);
        auto stream = stub.WatchLoad(&context);
        v1::LoadWatch credit;
        credit.set_interval_ms(100);
        v1::LoadUpdate sample;
        if (expectFull) {
            stream->Write(credit);
            stream->WritesDone();
            require(!stream->Read(&sample) && stream->Finish().error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED,
                    "third watch was not refused");
            return;
        }
        unsigned last = 0;
        for (unsigned i = 0; i < 5; ++i) {
            require(stream->Write(credit) && stream->Read(&sample), "watch credit did not receive a snapshot");
            require(sample.sequence() > last && sample.skipped_samples() == sample.sequence() - last - 1 &&
                    sample.queued() <= sample.queue_limit() && sample.watchers() <= 2, "watch budget or sampling invariant");
            skipped += sample.skipped_samples();
            last = sample.sequence();
            ++samples;
            if (i < 4) std::this_thread::sleep_for(600ms);
        }
        stream->WritesDone();
        require(!stream->Read(&sample) && stream->Finish().ok(), "watch did not drain on close");
    } while (std::chrono::steady_clock::now() < until);
    require(skipped > samples, "unused credits did not coalesce scheduled snapshots");
    std::cout << "{\"samples\":" << samples << ",\"coalesced\":" << skipped << "}\n";
}

grpc::Status loseAcceptanceEcho(v1::Fleet::Stub& stub, const char* robot, SqliteEdgeLedger& edge,
                                std::string_view taskId) {
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 4s);
    context.AddMetadata(robotHeader, robot);
    context.AddMetadata(tokenHeader, environmentToken("NEBULA_FLEET_ROBOT_TOKEN"));
    auto stream = stub.Exchange(&context);
    v1::EdgeFrame frame;
    auto* hello = frame.mutable_hello();
    hello->set_version(protocolVersion);
    hello->set_boot_id(nebula::workflow::newTraceId());
    hello->set_stations(1U);
    v1::CoordinatorFrame response;
    std::optional<Receipt> outstanding;
    while (stream->Write(frame) && stream->Read(&response)) {
        require(response.has_confirmed() == outstanding.has_value(), "unexpected replay confirmation");
        if (outstanding) {
            const auto echoed = decode(response.confirmed());
            require(echoed.taskId == outstanding->taskId && echoed.epoch == outstanding->epoch &&
                    echoed.phase == outstanding->phase && echoed.outcome == outstanding->outcome,
                    "test peer received confirmation for a different receipt");
            edge.confirm(*outstanding);
        }
        if (response.has_command()) {
            const auto command = decode(response.command());
            require(command.taskId == taskId, "test peer received a different command");
            edge.stage(command);
        }
        frame.Clear();
        outstanding = edge.pendingReceipt();
        if (outstanding) {
            encode(*outstanding, *frame.mutable_receipt());
        } else {
            frame.mutable_heartbeat();
            std::this_thread::sleep_for(15ms);
            continue;
        }
        // The previous edge may have stopped after the coordinator committed
        // its terminal receipt and before the echo reached the edge ledger.
        if (outstanding->taskId == taskId && outstanding->phase == Phase::accepted) {
            require(stream->Write(frame), "test peer did not send ACCEPTED");
            // Flush the upstream direction without ever reading the downstream echo.
            require(stream->WritesDone(), "test peer could not finish sending ACCEPTED");
            // The parent kills this process after the coordinator commits
            // acceptance. The echo stays unread in the abandoned stream.
            std::cout << "ACCEPTED_SENT" << std::endl;
            // The parent's ten-second assertion owns the test deadline.
            std::this_thread::sleep_for(12s);
            context.TryCancel();
            stream->Finish();
            return grpc::Status::OK;
        }
    }
    stream->WritesDone();
    const auto status = stream->Finish();
    if (!status.ok()) return status;
    return {grpc::StatusCode::FAILED_PRECONDITION, "stream closed before a command was received"};
}

}

int main(int argc, char* argv[]) {
    try {
        if (argc == 4 && std::string_view(argv[1]) == "--versions") {
            checkVersions(*v1::Fleet::NewStub(localChannel(argv[2])), argv[3]);
            return 0;
        }
        if (argc == 3 && (std::string_view(argv[1]) == "--slow-watch" || std::string_view(argv[1]) == "--watch-full")) {
            watch(*v1::Fleet::NewStub(localChannel(argv[2])), std::string_view(argv[1]) == "--watch-full");
            return 0;
        }
        if (argc != 5) throw std::invalid_argument("test peer requires endpoint, robot, ledger and task ID");
        auto stub = v1::Fleet::NewStub(localChannel(argv[1]));
        SqliteEdgeLedger edge(std::filesystem::path(argv[3]), argv[2]);
        // A just-terminated edge may still own the previous server session.
        // Retry only that transient handshake error, before staging a command.
        const auto until = std::chrono::steady_clock::now() + 2s;
        do {
            const auto status = loseAcceptanceEcho(*stub, argv[2], edge, argv[4]);
            if (status.ok()) return 0;
            if (status.error_code() != grpc::StatusCode::UNAVAILABLE) {
                throw std::runtime_error("test peer did not receive a command: " + status.error_message());
            }
            std::this_thread::sleep_for(40ms);
        } while (std::chrono::steady_clock::now() < until);
        throw std::runtime_error("test peer could not attach after the previous session closed");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
