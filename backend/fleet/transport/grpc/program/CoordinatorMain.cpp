#include "../FleetService/FleetService.h"
#include "../../../infrastructure/SqliteFleetLedger/SqliteFleetLedger.h"
#include "ProcessStop.h"

#include <chrono>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <set>
#include <thread>
#include <utility>

using namespace nebula::fleet;
using namespace nebula::fleet::wire;
using namespace std::chrono_literals;

namespace {
FleetService::Robots registrations(int argc, char* argv[], int first, std::set<std::string>& tokens) {
    if (argc - first < 3 || (argc - first) % 3 != 0 || (argc - first) / 3 > 16) {
        throw Error(ErrorCode::invalid, "usage: nebula_fleet_coordinator 127.0.0.1:PORT LEDGER [--agent-sim] [ROBOT STATIONS TOKEN_ENV]...");
    }
    FleetService::Robots robots;
    for (int i = first; i < argc; i += 3) {
        const std::string_view id(argv[i]);
        const std::string_view mask(argv[i + 1]);
        if (!validId(id) || (mask != "1" && mask != "2" && mask != "3") || !validId(argv[i + 2])) {
            throw Error(ErrorCode::invalid, "invalid robot grant");
        }
        auto token = environmentToken(argv[i + 2]);
        if (!tokens.insert(token).second ||
            !robots.emplace(id, RobotCredential{static_cast<unsigned>(mask.front() - '0'), std::move(token)}).second) {
            throw Error(ErrorCode::conflict, "robot IDs and fleet credentials must be distinct");
        }
    }
    return robots;
}

AgentCredentials simulationCredentials(bool enabled, std::set<std::string>& tokens) {
    if (!enabled) return {};
    AgentCredentials agent{environmentToken(agentReadEnv), environmentToken(agentDraftEnv), environmentToken(agentReviewEnv)};
    if (!tokens.insert(agent.read).second || !tokens.insert(agent.draft).second || !tokens.insert(agent.review).second) {
        throw Error(ErrorCode::conflict, "fleet credentials must be distinct");
    }
    return agent;
}
}

int main(int argc, char* argv[]) {
    try {
        auto token = environmentToken("NEBULA_FLEET_OPERATOR_TOKEN");
        const bool agentSim = argc > 3 && std::string_view(argv[3]) == "--agent-sim";
        std::set<std::string> tokens{token};
        auto agent = simulationCredentials(agentSim, tokens);
        auto robots = registrations(argc, argv, agentSim ? 4 : 3, tokens);
        localPort(argv[1], true);
        SqliteFleetLedger ledger{std::filesystem::path(argv[2])};
        FleetCoordinator coordinator(ledger);
        FleetService service(coordinator, std::move(token), std::move(robots), std::move(agent));

        process::installStopHandlers();
        grpc::ServerBuilder builder;
        grpc::ResourceQuota resources;
        resources.SetMaxThreads(32);
        builder.SetResourceQuota(resources);
        builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS, 32);
        builder.SetMaxReceiveMessageSize(messageLimit);
        builder.SetMaxSendMessageSize(messageLimit);
        builder.RegisterService(&service);
        int port = 0;
        builder.AddListeningPort(argv[1], grpc::InsecureServerCredentials(), &port);
        auto server = builder.BuildAndStart();
        if (!server || !port) throw Error(ErrorCode::offline, "cannot bind fleet gRPC listener");
        std::cout << "READY 127.0.0.1:" << port << std::endl;
        while (!process::stopped) std::this_thread::sleep_for(25ms);
        server->Shutdown(std::chrono::system_clock::now() + 300ms);
        server->Wait();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fleet coordinator: " << error.what() << '\n';
        return 1;
    }
}
