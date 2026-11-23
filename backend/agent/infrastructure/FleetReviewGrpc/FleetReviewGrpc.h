#pragma once

#include "backend/fleet/transport/grpc/FleetWire/FleetWire.h"

#include <memory>
#include <optional>

namespace nebula::agent {

struct ReviewResult {
    std::optional<fleet::SimulationReview> review;
    std::string_view errorCode;
};

class FleetReviewGrpc {
public:
    FleetReviewGrpc(std::string_view address, const char* tokenVariable);
    ReviewResult stage(const fleet::ReviewDraft& draft);
    ReviewResult inspect(std::string_view traceId);
    ReviewResult decide(std::string_view traceId, fleet::Decision decision);

private:
    void authenticate(grpc::ClientContext& context) const;
    std::unique_ptr<fleet::v1::Fleet::Stub> stub_;
    std::string token_;
};

}
