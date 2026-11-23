#pragma once

#include "../../ports/TaskEvidenceReader/TaskEvidenceReader.h"
#include "backend/fleet/transport/grpc/FleetWire/FleetWire.h"

#include <memory>

namespace nebula::agent {

class FleetStatusGrpc final : public TaskEvidenceReader {
public:
    explicit FleetStatusGrpc(std::string_view address, bool requireReader = false);
    EvidenceResult read(std::string_view taskId, std::stop_token stop,
                        std::chrono::steady_clock::time_point deadline) override;

private:
    std::unique_ptr<fleet::v1::Fleet::Stub> stub_;
    std::string token_;
};

}
