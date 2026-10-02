#include "web_learning.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

struct Options
{
    std::string target{"127.0.0.1:50051"};
    std::string mode{"echo"};
    std::string token;
    std::string tlsCaPath;
    std::size_t concurrency{8};
    int warmupSeconds{2};
    int durationSeconds{10};
};

std::size_t parseSize(const char *value, const char *name)
{
    char *end = nullptr;
    const auto parsed = std::strtoull(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0)
        throw std::invalid_argument(std::string(name) + " must be a positive integer");
    return static_cast<std::size_t>(parsed);
}

Options parseArguments(int argc, char **argv)
{
    Options options;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--help")
        {
            std::cout << "Usage: grpc_benchmark [--target host:port] "
                         "[--mode echo|count|upload|chat] [--concurrency N] "
                         "[--warmup SEC] [--duration SEC] [--token TOKEN] "
                         "[--tls-ca FILE]\n";
            std::exit(0);
        }
        if (index + 1 >= argc)
            throw std::invalid_argument("missing value for " + argument);
        const std::string value = argv[++index];
        if (argument == "--target")
            options.target = value;
        else if (argument == "--mode")
            options.mode = value;
        else if (argument == "--token")
            options.token = value;
        else if (argument == "--tls-ca")
            options.tlsCaPath = value;
        else if (argument == "--concurrency")
            options.concurrency = parseSize(value.c_str(), "concurrency");
        else if (argument == "--warmup")
            options.warmupSeconds = static_cast<int>(parseSize(value.c_str(), "warmup"));
        else if (argument == "--duration")
            options.durationSeconds = static_cast<int>(parseSize(value.c_str(), "duration"));
        else
            throw std::invalid_argument("unknown argument: " + argument);
    }
    if (options.mode != "echo" && options.mode != "count" &&
        options.mode != "upload" && options.mode != "chat")
        throw std::invalid_argument("mode must be echo, count, upload, or chat");
    return options;
}

void prepareContext(::grpc::ClientContext &context, const Options &options)
{
    if (!options.token.empty())
        context.AddMetadata("authorization", "Bearer " + options.token);
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
}

bool runEcho(::webtest::rpc::v1::LearningService::Stub &stub, const Options &options)
{
    ::grpc::ClientContext context;
    prepareContext(context, options);
    ::webtest::rpc::v1::EchoRequest request;
    ::webtest::rpc::v1::EchoReply reply;
    request.set_message("phase10-benchmark");
    return stub.Echo(&context, request, &reply).ok() &&
           reply.message() == request.message();
}

bool runCount(::webtest::rpc::v1::LearningService::Stub &stub, const Options &options)
{
    ::grpc::ClientContext context;
    prepareContext(context, options);
    ::webtest::rpc::v1::CountRequest request;
    request.set_limit(8);
    auto reader = stub.Count(&context, request);
    ::webtest::rpc::v1::CountReply reply;
    std::uint32_t received = 0;
    while (reader->Read(&reply))
        ++received;
    return reader->Finish().ok() && received == request.limit();
}

bool runUpload(::webtest::rpc::v1::LearningService::Stub &stub, const Options &options)
{
    ::grpc::ClientContext context;
    prepareContext(context, options);
    ::webtest::rpc::v1::UploadSummary summary;
    auto writer = stub.Upload(&context, &summary);
    for (std::uint64_t sequence = 1; sequence <= 4; ++sequence)
    {
        ::webtest::rpc::v1::UploadChunk chunk;
        chunk.set_sequence(sequence);
        chunk.set_payload(std::string(256, 'x'));
        if (!writer->Write(chunk))
            break;
    }
    writer->WritesDone();
    return writer->Finish().ok() && summary.chunk_count() == 4;
}

bool runChat(::webtest::rpc::v1::LearningService::Stub &stub, const Options &options)
{
    ::grpc::ClientContext context;
    prepareContext(context, options);
    auto stream = stub.Chat(&context);
    bool valid = true;
    for (std::uint64_t sequence = 1; sequence <= 4; ++sequence)
    {
        ::webtest::rpc::v1::ChatMessage outbound;
        outbound.set_sequence(sequence);
        outbound.set_text("benchmark");
        ::webtest::rpc::v1::ChatMessage inbound;
        if (!stream->Write(outbound) || !stream->Read(&inbound) ||
            inbound.sequence() != sequence)
        {
            valid = false;
            break;
        }
    }
    stream->WritesDone();
    ::webtest::rpc::v1::ChatMessage ignored;
    while (stream->Read(&ignored))
    {
    }
    return stream->Finish().ok() && valid;
}

bool runOnce(::webtest::rpc::v1::LearningService::Stub &stub, const Options &options)
{
    if (options.mode == "echo")
        return runEcho(stub, options);
    if (options.mode == "count")
        return runCount(stub, options);
    if (options.mode == "upload")
        return runUpload(stub, options);
    return runChat(stub, options);
}

std::string readFile(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read TLS CA: " + path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

double percentile(const std::vector<double> &values, double fraction)
{
    if (values.empty())
        return 0.0;
    const auto index = static_cast<std::size_t>(
        fraction * static_cast<double>(values.size() - 1));
    return values[index];
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        const auto options = parseArguments(argc, argv);
        std::shared_ptr<::grpc::ChannelCredentials> credentials;
        if (options.tlsCaPath.empty())
            credentials = ::grpc::InsecureChannelCredentials();
        else
        {
            ::grpc::SslCredentialsOptions tls;
            tls.pem_root_certs = readFile(options.tlsCaPath);
            credentials = ::grpc::SslCredentials(tls);
        }
        auto channel = ::grpc::CreateChannel(options.target, credentials);
        if (!channel->WaitForConnected(
                std::chrono::system_clock::now() + std::chrono::seconds(5)))
            throw std::runtime_error("gRPC target did not become ready");

        struct WorkerResult
        {
            std::uint64_t completed{};
            std::uint64_t errors{};
            std::vector<double> latencyMicros;
        };
        std::vector<WorkerResult> results(options.concurrency);
        std::vector<std::thread> workers;
        workers.reserve(options.concurrency);
        std::atomic<bool> start{false};
        const auto measureStart = Clock::now() +
                                  std::chrono::seconds(options.warmupSeconds);
        const auto finish = measureStart +
                            std::chrono::seconds(options.durationSeconds);

        for (std::size_t worker = 0; worker < options.concurrency; ++worker)
        {
            workers.emplace_back([&, worker]
            {
                auto stub = ::webtest::rpc::v1::LearningService::NewStub(channel);
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                while (Clock::now() < finish)
                {
                    const auto began = Clock::now();
                    const bool ok = runOnce(*stub, options);
                    const auto ended = Clock::now();
                    if (began >= measureStart)
                    {
                        if (ok)
                        {
                            ++results[worker].completed;
                            results[worker].latencyMicros.push_back(
                                std::chrono::duration<double, std::micro>(ended - began).count());
                        }
                        else
                        {
                            ++results[worker].errors;
                        }
                    }
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto &worker : workers)
            worker.join();

        std::uint64_t completed = 0;
        std::uint64_t errors = 0;
        std::vector<double> latencies;
        for (auto &result : results)
        {
            completed += result.completed;
            errors += result.errors;
            latencies.insert(latencies.end(), result.latencyMicros.begin(),
                             result.latencyMicros.end());
        }
        std::sort(latencies.begin(), latencies.end());
        const double qps = static_cast<double>(completed) / options.durationSeconds;
        std::cout << std::fixed << std::setprecision(2)
                  << "{\"mode\":\"" << options.mode << "\","
                  << "\"target\":\"" << options.target << "\","
                  << "\"concurrency\":" << options.concurrency << ','
                  << "\"completed\":" << completed << ','
                  << "\"errors\":" << errors << ','
                  << "\"qps\":" << qps << ','
                  << "\"p50_us\":" << percentile(latencies, 0.50) << ','
                  << "\"p95_us\":" << percentile(latencies, 0.95) << ','
                  << "\"p99_us\":" << percentile(latencies, 0.99) << "}\n";
        return errors == 0 ? 0 : 2;
    }
    catch (const std::exception &error)
    {
        std::cerr << "grpc_benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
