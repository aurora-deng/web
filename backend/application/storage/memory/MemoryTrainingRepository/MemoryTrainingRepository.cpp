#include "backend/application/storage/memory/MemoryTrainingRepository/MemoryTrainingRepository.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>
namespace webserver::phase11 {
using storage_detail::orderedUsers;
using storage_detail::UserPair;
using storage_detail::MemberKey;
using storage_detail::IdempotencyKey;
TrainingCandidate MemoryTrainingRepository::createTrainingCandidate(storage_detail::MemoryState &memory, TrainingCandidate candidate)
{
    for (const auto &[_, current] : memory.trainingCandidates)
        if (current.ownerUserId == candidate.ownerUserId &&
            current.sourceMessageId == candidate.sourceMessageId &&
            current.state != TrainingCandidateState::Revoked &&
            candidate.state != TrainingCandidateState::Revoked)
            throw std::logic_error(
                "an active training candidate already exists for this source");
    candidate.id = memory.nextTrainingCandidateId++;
    memory.trainingCandidates.emplace(candidate.id, candidate);
    return candidate;
}

std::optional<TrainingCandidate> MemoryTrainingRepository::findTrainingCandidate(const storage_detail::MemoryState &memory, TrainingCandidateId id)
{
    const auto found = memory.trainingCandidates.find(id);
    return found == memory.trainingCandidates.end()
               ? std::nullopt
               : std::optional<TrainingCandidate>{found->second};
}

std::optional<TrainingCandidate> MemoryTrainingRepository::findActiveTrainingCandidate(const storage_detail::MemoryState &memory, UserId owner, MessageId sourceMessage)
{
    for (const auto &[_, candidate] : memory.trainingCandidates)
        if (candidate.ownerUserId == owner &&
            candidate.sourceMessageId == sourceMessage &&
            candidate.state != TrainingCandidateState::Revoked)
            return candidate;
    return std::nullopt;
}

std::vector<TrainingCandidate> MemoryTrainingRepository::trainingCandidatesForOwner(const storage_detail::MemoryState &memory, UserId owner)
{
    std::vector<TrainingCandidate> result;
    for (const auto &[_, candidate] : memory.trainingCandidates)
        if (candidate.ownerUserId == owner)
            result.push_back(candidate);
    std::ranges::sort(result, {}, &TrainingCandidate::id);
    return result;
}

std::vector<TrainingCandidate> MemoryTrainingRepository::trainingCandidatesByState(const storage_detail::MemoryState &memory, TrainingCandidateState state)
{
    std::vector<TrainingCandidate> result;
    for (const auto &[id, candidate] : memory.trainingCandidates)
        if (candidate.state == state) result.push_back(candidate);
    std::ranges::sort(result, {}, &TrainingCandidate::id);
    return result;
}

bool MemoryTrainingRepository::updateTrainingCandidate(storage_detail::MemoryState &memory, TrainingCandidate candidate)
{
    const auto found = memory.trainingCandidates.find(candidate.id);
    if (found == memory.trainingCandidates.end())
        return false;
    found->second = std::move(candidate);
    return true;
}

bool MemoryTrainingRepository::transitionTrainingCandidate(storage_detail::MemoryState &memory, TrainingCandidateId id, TrainingCandidateState expected, TrainingCandidateState next, std::optional<DatasetVersionId> dataset)
{
    const auto found = memory.trainingCandidates.find(id);
    if (found == memory.trainingCandidates.end() ||
        found->second.state != expected)
        return false;
    found->second.state = next;
    found->second.datasetVersionId = dataset;
    return true;
}

DatasetVersion MemoryTrainingRepository::createDatasetVersion(storage_detail::MemoryState &memory, DatasetVersion version)
{
    version.id = memory.nextDatasetVersionId++;
    memory.datasets.emplace(version.id, version);
    return version;
}

std::optional<DatasetVersion> MemoryTrainingRepository::findDatasetVersion(const storage_detail::MemoryState &memory, DatasetVersionId id)
{
    const auto found = memory.datasets.find(id);
    return found == memory.datasets.end()
               ? std::nullopt
               : std::optional<DatasetVersion>{found->second};
}

}
