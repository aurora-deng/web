#include "server/phase11/storage/postgres/PostgresStore/PostgresStore.h"
#include "server/phase11/training/JsonlTrainingDataExporter/JsonlTrainingDataExporter.h"
#include "server/phase11/training/TrainingDataService/TrainingDataService.h"
#include "server/phase11/transport/FlatJson/FlatJson.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace webserver::phase11;
using webserver::phase11::transport::quoteJson;

namespace
{

TrainingCandidateState parseState(std::string_view value)
{
    if (value == "submitted") return TrainingCandidateState::Submitted;
    if (value == "approved") return TrainingCandidateState::Approved;
    if (value == "rejected") return TrainingCandidateState::Rejected;
    if (value == "exported") return TrainingCandidateState::Exported;
    if (value == "revoked") return TrainingCandidateState::Revoked;
    throw std::invalid_argument("invalid candidate state");
}

std::string_view stateName(TrainingCandidateState state)
{
    switch (state)
    {
    case TrainingCandidateState::Submitted: return "submitted";
    case TrainingCandidateState::Approved: return "approved";
    case TrainingCandidateState::Rejected: return "rejected";
    case TrainingCandidateState::Exported: return "exported";
    case TrainingCandidateState::Revoked: return "revoked";
    }
    return "unknown";
}

std::uint64_t id(std::string_view value)
{
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                        parsed);
    if (value.empty() || result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() || parsed == 0)
        throw std::invalid_argument("candidate ID must be a positive integer");
    return parsed;
}

std::string sha256File(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open dataset artifact");
    EVP_MD_CTX *raw = EVP_MD_CTX_new();
    if (!raw) throw std::runtime_error("cannot allocate SHA-256 context");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>
        context(raw, EVP_MD_CTX_free);
    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("cannot initialize SHA-256");
    std::array<char, 64U * 1024U> buffer{};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (input.gcount() > 0 &&
            EVP_DigestUpdate(context.get(), buffer.data(),
                             static_cast<std::size_t>(input.gcount())) != 1)
            throw std::runtime_error("cannot hash dataset artifact");
    }
    if (!input.eof()) throw std::runtime_error("cannot read dataset artifact");
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1 ||
        size != 32)
        throw std::runtime_error("cannot finish dataset SHA-256");
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < size; ++index)
        result << std::setw(2) << static_cast<unsigned int>(digest[index]);
    return result.str();
}

void usage()
{
    std::cerr
        << "usage:\n"
        << "  phase11-training-admin list <state>\n"
        << "  phase11-training-admin review <candidate-id> approve|reject\n"
        << "  phase11-training-admin export <dataset-name> <output-root> "
           "<candidate-id>...\n"
        << "  phase11-training-admin authorize <dataset-id> <dataset-root> "
           "<manifest-output>\n";
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        const char *database = std::getenv("WEB_PHASE11_DATABASE");
        if (!database || !*database)
            throw std::invalid_argument("WEB_PHASE11_DATABASE is required");
        if (argc < 3)
        {
            usage();
            return 2;
        }

        PostgresStore store({database, 2, std::chrono::seconds(5)});
        if (!store.healthy()) throw std::runtime_error("PostgreSQL is not healthy");
        TrainingDataService service(store, store, store, store, store);
        const std::string_view command(argv[1]);

        if (command == "list" && argc == 3)
        {
            for (const auto &candidate :
                 store.trainingCandidatesByState(parseState(argv[2])))
                std::cout << "id=" << candidate.id
                          << " owner=" << candidate.ownerUserId
                          << " source_message=" << candidate.sourceMessageId
                          << " state=" << stateName(candidate.state) << '\n';
            return 0;
        }
        if (command == "review" && argc == 4)
        {
            const std::string_view decision(argv[3]);
            if (decision != "approve" && decision != "reject")
                throw std::invalid_argument("review decision must be approve or reject");
            const auto candidate = service.reviewTrainingCandidate(
                id(argv[2]), decision == "approve");
            std::cout << "id=" << candidate.id
                      << " state=" << stateName(candidate.state) << '\n';
            return 0;
        }
        if (command == "export" && argc >= 5)
        {
            std::vector<TrainingCandidateId> candidates;
            for (int index = 4; index < argc; ++index)
                candidates.push_back(id(argv[index]));
            JsonlTrainingDataExporter exporter({
                std::filesystem::path(argv[3]), 64U * 1024U * 1024U});
            const auto dataset = service.createDataset(
                argv[2], std::move(candidates), std::chrono::system_clock::now(),
                exporter);
            std::cout << "dataset_id=" << dataset.id
                      << " records=" << dataset.recordCount
                      << " sha256=" << dataset.checksum
                      << " path=" << dataset.artifactPath << '\n';
            return 0;
        }
        if (command == "authorize" && argc == 5)
        {
            const auto dataset = store.findDatasetVersion(id(argv[2]));
            if (!dataset) throw std::invalid_argument("dataset version was not found");
            for (const auto candidateId : dataset->candidates)
            {
                const auto candidate = store.findTrainingCandidate(candidateId);
                if (!candidate ||
                    candidate->state != TrainingCandidateState::Exported ||
                    candidate->datasetVersionId != dataset->id)
                    throw std::runtime_error(
                        "dataset contains a revoked or non-exported candidate");
            }

            const auto root = std::filesystem::canonical(argv[3]);
            const std::filesystem::path relative(dataset->artifactPath);
            if (relative.empty() || relative.is_absolute() ||
                std::ranges::find(relative, "..") != relative.end())
                throw std::runtime_error("dataset artifact path is unsafe");
            const auto artifact = std::filesystem::canonical(root / relative);
            const auto relativeCheck = std::filesystem::relative(artifact, root);
            if (relativeCheck.empty() || relativeCheck.is_absolute() ||
                std::ranges::find(relativeCheck, "..") != relativeCheck.end())
                throw std::runtime_error("dataset artifact escaped its root");
            if (sha256File(artifact) != dataset->checksum)
                throw std::runtime_error("dataset artifact checksum changed");
            std::ifstream lines(artifact);
            std::uint64_t recordCount = 0;
            std::string line;
            while (std::getline(lines, line)) if (!line.empty()) ++recordCount;
            if (!lines.eof() || recordCount != dataset->recordCount)
                throw std::runtime_error("dataset artifact record count changed");

            const auto output = std::filesystem::absolute(argv[4]);
            if (std::filesystem::exists(output))
                throw std::runtime_error("authorization manifest already exists");
            const auto temporary = output.string() + ".tmp";
            const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            try {
                {
                    std::ofstream manifest(temporary,
                        std::ios::binary | std::ios::trunc);
                    if (!manifest)
                        throw std::runtime_error("cannot create authorization manifest");
                    manifest << "{\n"
                             << "  \"schema\": \"phase22-training-authorization-v1\",\n"
                             << "  \"dataset_id\": " << dataset->id << ",\n"
                             << "  \"dataset_sha256\": "
                             << quoteJson(dataset->checksum) << ",\n"
                             << "  \"dataset_file\": "
                             << quoteJson(artifact.string()) << ",\n"
                             << "  \"record_count\": " << recordCount << ",\n"
                             << "  \"authorized_at_unix\": " << timestamp << "\n"
                             << "}\n";
                    manifest.flush();
                    if (!manifest)
                        throw std::runtime_error("cannot write authorization manifest");
                }
                std::filesystem::rename(temporary, output);
            } catch (...) {
                std::error_code cleanupError;
                std::filesystem::remove(temporary, cleanupError);
                throw;
            }
            std::cout << "dataset_id=" << dataset->id
                      << " authorization=" << output.string() << '\n';
            return 0;
        }

        usage();
        return 2;
    }
    catch (const std::exception &failure)
    {
        std::cerr << "phase11-training-admin: " << failure.what() << '\n';
        return 1;
    }
}
