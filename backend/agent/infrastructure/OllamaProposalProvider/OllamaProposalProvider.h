#pragma once

#include "../../ports/ProposalProvider/ProposalProvider.h"
#include "backend/ai/models/OllamaModelProvider/OllamaModelProvider.h"

namespace nebula::agent {

class OllamaProposalProvider final : public ProposalProvider {
public:
    OllamaProposalProvider(std::string endpoint, std::string model);
    std::string checkpointIdentity() const override;
    Candidate plan(Intent intent, std::span<const TaskEvidence> evidence, std::stop_token stop,
                   std::chrono::steady_clock::time_point deadline) override;

private:
    std::string endpoint_;
    std::string model_;
    webserver::phase11::OllamaModelProvider client_;
};

}  // namespace nebula::agent
