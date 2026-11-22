#include "backend/fleet/infrastructure/SqliteEdgeLedger/SqliteEdgeLedger.h"
#include "backend/fleet/transport/grpc/FleetWire/FleetWire.h"
#include "backend/workflow/WorkflowGraph/WorkflowGraph.h"

#include <chrono>
#include <iostream>
#include <thread>

using namespace nebula::fleet;
using namespace nebula::fleet::wire;
using namespace std::chrono_literals;

int main(int argc, char* argv[]) {
    try {
        if (argc != 4) throw std::invalid_argument("test peer requires endpoint, robot and ledger");
        auto stub = v1::Fleet::NewStub(localChannel(argv[1]));
        SqliteEdgeLedger edge(std::filesystem::path(argv[3]), argv[2]);
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 3s);
        context.AddMetadata(robotHeader, argv[2]);
        context.AddMetadata(tokenHeader, environmentToken("NEBULA_FLEET_ROBOT_TOKEN"));
        auto stream = stub->Exchange(&context);
        v1::EdgeFrame frame;
        auto* hello = frame.mutable_hello();
        hello->set_version(protocolVersion);
        hello->set_boot_id(nebula::workflow::newTraceId());
        hello->set_stations(1U);
        v1::CoordinatorFrame response;
        for (;;) {
            if (!stream->Write(frame) || !stream->Read(&response)) {
                throw std::runtime_error("test peer did not receive a command");
            }
            if (response.has_command()) break;
            frame.mutable_heartbeat();
            std::this_thread::sleep_for(15ms);
        }
        edge.stage(decode(response.command()));
        encode(*edge.pendingReceipt(), *frame.mutable_receipt());
        if (!stream->Write(frame)) throw std::runtime_error("test peer did not send ACCEPTED");
        // Leave the echoed receipt unread. The parent observes the durable
        // server state and terminates this process while the echo is unconsumed.
        std::cout << "ACCEPTED_SENT" << std::endl;
        std::this_thread::sleep_for(3s);
        context.TryCancel();
        stream->Finish();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
