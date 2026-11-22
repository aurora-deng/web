#include "../FleetWire/FleetWire.h"

#include <chrono>
#include <iomanip>
#include <iostream>

using namespace nebula::fleet;
using namespace nebula::fleet::wire;
using namespace std::chrono_literals;

int main(int argc, char* argv[]) {
    try {
        const auto command = argc >= 3 ? std::string_view(argv[2]) : "";
        if (!((argc == 4 && command == "status") || (argc == 5 && command == "submit"))) {
            throw Error(ErrorCode::invalid, "usage: nebula_fleet_ctl 127.0.0.1:PORT (status ID | submit ID alpha|beta)");
        }
        grpc::ClientContext context;
        context.AddMetadata(tokenHeader, environmentToken("NEBULA_FLEET_OPERATOR_TOKEN"));
        context.set_deadline(std::chrono::system_clock::now() + 2s);
        auto stub = v1::Fleet::NewStub(localChannel(argv[1]));
        v1::TaskStatus task;
        grpc::Status result;
        if (command == "status") {
            v1::TaskQuery query;
            query.set_task_id(argv[3]);
            result = stub->Status(&context, query, &task);
        } else {
            v1::TaskRequest request;
            request.set_task_id(argv[3]);
            const std::string_view station(argv[4]);
            if (station != "alpha" && station != "beta") throw Error(ErrorCode::invalid, "invalid station selection");
            request.set_station(station == "alpha" ? v1::STATION_ALPHA : v1::STATION_BETA);
            result = stub->Submit(&context, request, &task);
        }
        if (!result.ok()) {
            std::cerr << "rpc_status=" << result.error_code() << ' ' << result.error_message() << '\n';
            return 2;
        }
        std::cout << "{\"taskId\":" << std::quoted(task.command().task_id())
                  << ",\"traceId\":" << std::quoted(task.command().trace_id())
                  << ",\"epoch\":" << task.command().epoch() << ",\"phase\":\"" << name(static_cast<Phase>(task.phase()))
                  << "\",\"robotId\":" << std::quoted(task.robot_id()) << ",\"outcome\":" << std::quoted(task.outcome())
                  << ",\"connected\":" << (task.connected() ? "true" : "false")
                  << ",\"needsReconciliation\":" << (task.needs_reconciliation() ? "true" : "false") << '}' << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fleet client: " << error.what() << '\n';
        return 1;
    }
}
