#pragma once

#include "FleetAgent.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace nebula::agent::records {

// Private checkpoint encoding. Values are already constrained by the domain.
inline std::string evidenceOf(std::span<const TaskEvidence> batch) {
    std::ostringstream out;
    out << batch.size();
    for (const auto& row : batch) {
        out << ' ' << std::quoted(row.taskId) << ' ' << row.traceId << ' ' << static_cast<unsigned>(row.phase)
            << ' ' << row.epoch << ' ' << row.assigned << ' ' << row.needsReconciliation << ' ' << std::quoted(row.outcome);
    }
    return out.str();
}

inline std::vector<TaskEvidence> evidenceFrom(std::string_view value) {
    std::istringstream in{std::string(value)};
    std::size_t size{};
    if (!(in >> size) || size > maxReviewTasks) throw std::runtime_error("invalid checkpoint evidence size");
    std::vector<TaskEvidence> rows;
    for (std::size_t i = 0; i < size; ++i) {
        TaskEvidence row;
        unsigned phase{};
        if (!(in >> std::quoted(row.taskId) >> row.traceId >> phase >> row.epoch >> row.assigned
                 >> row.needsReconciliation >> std::quoted(row.outcome))) {
            throw std::runtime_error("invalid checkpoint evidence");
        }
        row.phase = static_cast<fleet::Phase>(phase);
        if (!validEvidence(row, row.taskId)) throw std::runtime_error("invalid checkpoint evidence identity");
        rows.push_back(std::move(row));
    }
    if (!(in >> std::ws).eof()) throw std::runtime_error("unexpected checkpoint evidence");
    return rows;
}

inline std::string proposalOf(PlannerSource source, const Proposal& proposal) {
    std::ostringstream out;
    out << static_cast<unsigned>(source) << ' ' << std::quoted(proposal.skillId) << ' ' << proposal.version
        << ' ' << proposal.evidenceTaskIds.size();
    for (const auto& id : proposal.evidenceTaskIds) out << ' ' << std::quoted(id);
    return out.str();
}

inline std::pair<PlannerSource, Proposal> proposalFrom(std::string_view value) {
    std::istringstream in{std::string(value)};
    unsigned source{};
    Proposal proposal;
    std::size_t size{};
    if (!(in >> source >> std::quoted(proposal.skillId) >> proposal.version >> size) ||
        source == 0 || source > static_cast<unsigned>(PlannerSource::fallback) || size > maxReviewTasks) {
        throw std::runtime_error("invalid proposal checkpoint");
    }
    for (std::size_t i = 0; i < size; ++i) {
        std::string id;
        if (!(in >> std::quoted(id)) || !fleet::validId(id)) throw std::runtime_error("invalid proposal evidence key");
        proposal.evidenceTaskIds.push_back(std::move(id));
    }
    if (!(in >> std::ws).eof()) throw std::runtime_error("unexpected proposal checkpoint");
    return {static_cast<PlannerSource>(source), std::move(proposal)};
}

}  // namespace nebula::agent::records
