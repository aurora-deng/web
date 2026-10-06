#include "server/phase11/ai/GrpcOnnxModelProvider/GrpcOnnxModelProvider.h"
#include "server/phase11/ai/GrpcOnnxWorker/GrpcOnnxWorkerService.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace
{

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

class FakeOnnxProvider final : public webserver::phase11::IModelProvider,
                               public webserver::phase11::IModelArtifactValidator
{
public:
    ~FakeOnnxProvider() override
    {
        std::vector<std::thread> threads;
        {
            std::lock_guard lock(mutex_);
            threads.swap(threads_);
        }
        for (auto &thread : threads)
            if (thread.joinable()) thread.join();
    }

    std::string name() const override { return "fake-onnx"; }
    bool ready() const noexcept override { return true; }
    std::string supportedRuntime() const override { return "onnx-inprocess"; }

    webserver::phase11::ModelArtifactValidation validateVersion(
        const webserver::phase11::ModelVersion &version) override
    {
        return {version.runtime == "onnx-inprocess" &&
                    version.modelArtifact == "model" &&
                    version.checksum == "sha256-tree-v1:test",
                version.modelArtifact == "model" ? "" : "unexpected model"};
    }

    webserver::phase11::GenerationHandle generate(
        const webserver::phase11::GenerationRequest &request,
        webserver::phase11::TokenSink onToken,
        webserver::phase11::CompletionSink onComplete,
        webserver::phase11::CancellationToken cancellation) override
    {
        const auto id = request.requestId;
        std::thread worker([id, onToken = std::move(onToken),
                            onComplete = std::move(onComplete),
                            cancellation]() mutable
        {
            onToken("hello");
            if (id == "cancel-request")
            {
                const auto deadline = std::chrono::steady_clock::now() + 2s;
                while (!cancellation.cancelled() &&
                       std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(5ms);
                onComplete({false, cancellation.cancelled(),
                            cancellation.cancelled() ? "cancelled" : "timeout", {}});
                return;
            }
            onToken(" world");
            onComplete({true, false, {}, {}});
        });
        {
            std::lock_guard lock(mutex_);
            threads_.push_back(std::move(worker));
        }
        return {request.requestId, std::move(cancellation)};
    }

private:
    std::mutex mutex_;
    std::vector<std::thread> threads_;
};

struct CompletionWaiter final
{
    void complete(webserver::phase11::GenerationCompletion value)
    {
        std::lock_guard lock(mutex);
        completion = std::move(value);
        done = true;
        changed.notify_all();
    }

    webserver::phase11::GenerationCompletion wait()
    {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, 5s, [&] { return done; }),
                "generation did not complete");
        return completion;
    }

    std::mutex mutex;
    std::condition_variable changed;
    webserver::phase11::GenerationCompletion completion;
    bool done = false;
};

webserver::phase11::GenerationRequest request(std::string id)
{
    webserver::phase11::GenerationRequest value;
    value.requestId = std::move(id);
    value.requester = 7;
    value.modelNode = 8;
    value.messages.push_back({"user", "test"});
    value.maxOutputTokens = 16;
    value.modelArtifact = "model";
    value.modelChecksum = "sha256-tree-v1:test";
    return value;
}

} // namespace

int main()
{
    try
    {
        auto backend = std::make_shared<FakeOnnxProvider>();
        webserver::phase11::GrpcOnnxWorkerService service(
            backend, backend,
            {.authenticationToken = "phase11-internal-test-token",
             .maxBufferedTokens = 4,
             .maxBufferedBytes = 1024});

        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0",
                                 grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        auto server = builder.BuildAndStart();
        require(server && port > 0, "cannot start fake ONNX worker");

        {
            webserver::phase11::GrpcOnnxModelProvider client(
                {.target = "127.0.0.1:" + std::to_string(port),
                 .authenticationToken = "phase11-internal-test-token",
                 .workerCount = 1,
                 .maxQueued = 4,
                 .maxStreamBytes = 1024,
                 .rpcTimeout = 5s,
                 .validationTimeout = 5s});

            webserver::phase11::ModelVersion version;
            version.id = 3;
            version.nodeId = 8;
            version.runtime = "onnx-grpc";
            version.modelArtifact = "model";
            version.checksum = "sha256-tree-v1:test";
            const auto validation = client.validateVersion(version);
            require(validation.valid, "remote model validation failed");

            std::string output;
            CompletionWaiter success;
            client.generate(
                request("success-request"),
                [&](std::string_view token) { output.append(token); },
                [&](webserver::phase11::GenerationCompletion completion)
                {
                    success.complete(std::move(completion));
                });
            const auto completed = success.wait();
            require(completed.success && !completed.cancelled,
                    "remote generation did not succeed");
            require(output == "hello world", "remote token stream changed order");

            webserver::phase11::CancellationToken cancellation;
            CompletionWaiter cancelled;
            std::size_t tokenCount = 0;
            client.generate(
                request("cancel-request"),
                [&, cancellation](std::string_view)
                {
                    ++tokenCount;
                    cancellation.cancel();
                },
                [&](webserver::phase11::GenerationCompletion completion)
                {
                    cancelled.complete(std::move(completion));
                },
                cancellation);
            const auto cancelledResult = cancelled.wait();
            require(tokenCount == 1, "cancel test should receive exactly one token");
            require(!cancelledResult.success && cancelledResult.cancelled,
                    "remote cancellation was not propagated");

            webserver::phase11::GrpcOnnxModelProvider badToken(
                {.target = "127.0.0.1:" + std::to_string(port),
                 .authenticationToken = "wrong-token",
                 .workerCount = 1,
                 .maxQueued = 1,
                 .rpcTimeout = 2s,
                 .validationTimeout = 2s});
            const auto rejected = badToken.validateVersion(version);
            require(!rejected.valid,
                    "worker accepted an invalid authentication token");
        }

        server->Shutdown();
        server->Wait();
        std::cout << "PHASE11_GRPC_ONNX_PROVIDER_OK\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "phase11 gRPC ONNX test failed: " << error.what() << '\n';
        return 1;
    }
}
