#include "tests/phase11/smoke/OnnxBenchmark.h"
#include "backend/ai/inference/inprocess/InProcessOnnxModelProvider/InProcessOnnxModelProvider.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>

using namespace webserver::phase11;

int main(int argc, char **argv)
{
    try {
        if (argc != 6 && argc != 7) {
            std::cerr << "usage: phase11_onnx_model_benchmark <model-root> <relative-model-dir> "
                         "<synthetic-cases.jsonl> <repeats:1-3> <timeout-seconds:1-180> [relative-adapter]\n";
            return 2;
        }
        const auto root = std::filesystem::canonical(argv[1]);
        const std::filesystem::path relative(argv[2]);
        if (relative.empty() || relative.is_absolute() || relative.has_root_path())
            throw std::invalid_argument("model directory must be relative");
        for (const auto &part : relative)
            if (part == "..") throw std::invalid_argument("model directory escape rejected");
        // Reject symlink components before reading or checksumming the model directory.
        auto directory = root;
        for (const auto &part : relative) {
            directory /= part;
            if (std::filesystem::is_symlink(directory))
                throw std::invalid_argument("model symlink rejected");
        }
        directory = std::filesystem::canonical(directory);
        if (directory == root || directory.lexically_relative(root).empty())
            throw std::invalid_argument("invalid model directory");
        const auto repeats = benchmark::boundedNumber(argv[4], 3);
        const auto timeout = std::chrono::seconds(benchmark::boundedNumber(argv[5], 180));
        std::ifstream input(argv[3], std::ios::binary);
        if (!input || std::filesystem::file_size(argv[3]) > 262144)
            throw std::invalid_argument("missing or oversized synthetic cases");
        std::vector<benchmark::Case> cases;
        std::set<std::string> ids;
        for (std::string line; std::getline(input, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto test = benchmark::parseCase(line);
            if (!ids.insert(test.id).second || cases.size() >= 64)
                throw std::invalid_argument("duplicate or too many benchmark cases");
            cases.push_back(std::move(test));
        }
        if (input.bad() || cases.empty()) throw std::invalid_argument("empty or unreadable cases");
        const auto checksumStarted = std::chrono::steady_clock::now();
        const auto checksum = computeOnnxArtifactChecksum(directory);
        const auto checksumUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - checksumStarted).count();
        std::cout.imbue(std::locale::classic());
        std::cout << "{\"kind\":\"metadata\",\"schema\":\"nebula-onnx-benchmark-v1\","
                     "\"checksumMicroseconds\":" << checksumUs << ",\"modelChecksum\":"
                  << transport::quoteJson(checksum) << ",\"prevalidated\":false}\n" << std::flush;
        InProcessOnnxModelProviderConfig config;
        config.modelRoot = root;
        config.workerCount = 1;
        config.maxQueued = 2;
        config.maxLoadedModels = 1;
        InProcessOnnxModelProvider provider(std::move(config));
        // No validateVersion(): the first measured request loads this process's model.
        // OS page cache is deliberately not dropped; this is not a disk-cold claim.
        std::size_t ordinal = 0;
        bool passed = true;
        for (std::size_t repeat = 1; repeat <= repeats; ++repeat) {
            for (const auto &test : cases) {
                GenerationRequest request;
                request.requestId = "benchmark-" + std::to_string(++ordinal);
                request.modelArtifact = relative.generic_string();
                request.adapterArtifact = argc == 7 ? argv[6] : "";
                request.modelChecksum = checksum;
                request.maxOutputTokens = test.maxTokens;
                request.messages = {{"system", test.system}, {"user", test.prompt}};
                const auto measured = benchmark::generate(provider, request, timeout);
                std::cout << benchmark::json(test, repeat, ordinal, measured) << '\n' << std::flush;
                passed = passed && measured.completion.success && !measured.timedOut &&
                    !measured.outputLimitExceeded && measured.completion.usage &&
                    measured.completion.usage->valid() && measured.completion.usage->outputTokens;
                if (!passed) return 1; // Fail closed, do not keep loading a broken candidate.
            }
        }
        return passed ? 0 : 1;
    } catch (const std::exception &) {
        // Paths and provider diagnostics may contain private deployment details.
        std::cerr << "ONNX benchmark failed; check bounded arguments, model and fixture locally\n";
        return 1;
    }
}
