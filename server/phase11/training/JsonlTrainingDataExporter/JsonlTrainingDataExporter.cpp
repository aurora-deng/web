#include "server/phase11/training/JsonlTrainingDataExporter/JsonlTrainingDataExporter.h"

#include "server/phase11/transport/FlatJson/FlatJson.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace webserver::phase11
{
namespace
{

std::string sha256(std::string_view value)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_Digest(value.data(), value.size(), digest.data(), &size,
                   EVP_sha256(), nullptr) != 1 || size != 32)
        throw std::runtime_error("training dataset SHA-256 failed");
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < size; ++index)
        output << std::setw(2) << static_cast<unsigned int>(digest[index]);
    return output.str();
}

std::string renderDataset(std::vector<TrainingCandidate> candidates)
{
    std::ranges::sort(candidates, {}, &TrainingCandidate::id);
    std::string content;
    for (const auto &candidate : candidates)
    {
        if (candidate.state != TrainingCandidateState::Approved)
            throw std::invalid_argument(
                "training dataset exporter accepts only approved candidates");
        content += "{\"schema\":\"phase22-chat-jsonl-v1\",\"candidate_id\":";
        content += std::to_string(candidate.id);
        content += ",\"consent_version\":";
        content += transport::quoteJson(candidate.consentVersion);
        content += ",\"messages\":[{\"role\":\"user\",\"content\":";
        content += transport::quoteJson(candidate.sanitizedPrompt);
        content += "},{\"role\":\"assistant\",\"content\":";
        content += transport::quoteJson(candidate.sanitizedResponse);
        content += "}]}\n";
    }
    return content;
}

bool fileEquals(const std::filesystem::path &path, std::string_view expected)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::string actual(std::istreambuf_iterator<char>(input), {});
    return actual == expected;
}

} // namespace

JsonlTrainingDataExporter::JsonlTrainingDataExporter(
    JsonlTrainingDataExporterConfig config)
    : config_(std::move(config))
{
    if (config_.root.empty() || config_.maximumDatasetBytes == 0)
        throw std::invalid_argument("training dataset export config is invalid");
    std::filesystem::create_directories(config_.root);
    config_.root = std::filesystem::weakly_canonical(config_.root);
    if (!std::filesystem::is_directory(config_.root))
        throw std::invalid_argument("training dataset root is not a directory");
}

TrainingDatasetArtifact JsonlTrainingDataExporter::exportDataset(
    std::string_view datasetName,
    const std::vector<TrainingCandidate> &candidates)
{
    if (datasetName.empty() || candidates.empty())
        throw std::invalid_argument("training dataset cannot be empty");
    const auto content = renderDataset(candidates);
    if (content.empty() || content.size() > config_.maximumDatasetBytes)
        throw std::invalid_argument("training dataset exceeds the byte limit");

    const auto checksum = sha256(content);
    const auto fileName = "dataset-" + checksum + ".jsonl";
    const auto finalPath = config_.root / fileName;
    if (std::filesystem::exists(finalPath))
    {
        if (!fileEquals(finalPath, content))
            throw std::runtime_error(
                "existing training dataset does not match its checksum name");
        return {fileName, checksum,
                static_cast<std::uint64_t>(candidates.size())};
    }

    static std::atomic_uint64_t sequence{0};
    const auto nonce = sequence.fetch_add(1, std::memory_order_relaxed);
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto temporary = config_.root /
        ("." + fileName + ".tmp-" + std::to_string(thread) + "-" +
         std::to_string(nonce));
    try
    {
        {
            std::ofstream output(temporary,
                                 std::ios::binary | std::ios::trunc);
            if (!output)
                throw std::runtime_error(
                    "cannot create temporary training dataset");
            output.write(content.data(), static_cast<std::streamsize>(content.size()));
            output.flush();
            if (!output)
                throw std::runtime_error("cannot write training dataset");
        }
        // 同 checksum 的并发导出允许一个先完成；另一个随后核对正文并复用。
        if (std::filesystem::exists(finalPath))
        {
            if (!fileEquals(finalPath, content))
                throw std::runtime_error(
                    "concurrent training dataset checksum conflict");
            std::filesystem::remove(temporary);
        }
        else
        {
            std::error_code renameError;
            std::filesystem::rename(temporary, finalPath, renameError);
            if (renameError)
            {
                // exists() 与 rename() 之间可能有另一个线程先完成同一份导出。
                // 只有最终文件正文完全相同时才复用，否则保留错误而不是覆盖。
                if (!std::filesystem::exists(finalPath) ||
                    !fileEquals(finalPath, content))
                    throw std::filesystem::filesystem_error(
                        "cannot publish training dataset", temporary,
                        finalPath, renameError);
                std::filesystem::remove(temporary);
            }
        }
    }
    catch (...)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    return {fileName, checksum,
            static_cast<std::uint64_t>(candidates.size())};
}

} // namespace webserver::phase11
