#pragma once

#include "../../../application/FleetCoordinator/FleetCoordinator.h"
#include "../FleetWire/FleetWire.h"

namespace nebula::fleet::wire {

struct RobotCredential {
    Capabilities allowed;
    std::string token;
};

class FleetService final : public v1::Fleet::Service {
public:
    using Robots = std::map<std::string, RobotCredential, std::less<>>;
    FleetService(FleetCoordinator& coordinator, std::string operatorToken, Robots robots);

    grpc::Status Submit(grpc::ServerContext* context, const v1::TaskRequest* in, v1::TaskStatus* out) override;
    grpc::Status Status(grpc::ServerContext* context, const v1::TaskQuery* in, v1::TaskStatus* out) override;
    grpc::Status Exchange(grpc::ServerContext* context,
                          grpc::ServerReaderWriter<v1::CoordinatorFrame, v1::EdgeFrame>* stream) override;

private:
    FleetCoordinator& coordinator_;
    const std::string operatorToken_;
    const Robots robots_;
};
}
