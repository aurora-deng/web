#pragma once

#include "../../../application/FleetCoordinator/FleetCoordinator.h"
#include "../FleetWire/FleetWire.h"

#include <atomic>
#include <semaphore>

namespace nebula::fleet::wire {

struct RobotCredential {
    Capabilities allowed;
    std::string token;
};

struct AgentCredentials {
    std::string read;
    std::string draft;
    std::string review;
};

class FleetService final : public v1::Fleet::Service {
public:
    using Robots = std::map<std::string, RobotCredential, std::less<>>;
    FleetService(FleetCoordinator& coordinator, std::string operatorToken, Robots robots, AgentCredentials agent = {});

    grpc::Status ListTasks(grpc::ServerContext*,const v1::TaskListQuery*,v1::TaskList*) override;
    grpc::Status Submit(grpc::ServerContext* context, const v1::TaskRequest* in, v1::TaskStatus* out) override;
    grpc::Status Status(grpc::ServerContext* context, const v1::TaskQuery* in, v1::TaskStatus* out) override;
    grpc::Status StageReview(grpc::ServerContext* context, const v1::ReviewDraft* in, v1::SimulationReview* out) override;
    grpc::Status InspectReview(grpc::ServerContext* context, const v1::ReviewQuery* in, v1::SimulationReview* out) override;
    grpc::Status DecideReview(grpc::ServerContext* context, const v1::ReviewRequest* in, v1::SimulationReview* out) override;
    grpc::Status Load(grpc::ServerContext* context, const v1::LoadQuery* in, v1::LoadUpdate* out) override;
    grpc::Status WatchLoad(grpc::ServerContext* context,
                           grpc::ServerReaderWriter<v1::LoadUpdate, v1::LoadWatch>* stream) override;
    grpc::Status Exchange(grpc::ServerContext* context,
                          grpc::ServerReaderWriter<v1::CoordinatorFrame, v1::EdgeFrame>* stream) override;

private:
    bool mayRead(grpc::ServerContext& context) const;
    void sampleLoad(v1::LoadUpdate& out);
    FleetCoordinator& coordinator_;
    const std::string operatorToken_;
    const Robots robots_;
    const AgentCredentials agent_;
    std::counting_semaphore<2> watchSlots_{2};
    std::atomic_uint watchers_{};
    std::atomic_uint64_t watchRejections_{};
};
}
