#include "server/phase11/ai/OllamaModelProvider/OllamaModelProvider.h"
#include "server/phase11/ai/OllamaStreamDecoder/OllamaStreamDecoder.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

#define CHECK(condition)                                                        \
    do                                                                          \
    {                                                                           \
        if (!(condition))                                                       \
        {                                                                       \
            std::cerr << "CHECK failed at line " << __LINE__ << ": "          \
                      << #condition << '\n';                                    \
            std::exit(1);                                                       \
        }                                                                       \
    } while (false)

using namespace std::chrono_literals;
using namespace webserver::phase11;

void sendAll(int socket, std::string_view bytes)
{
    while (!bytes.empty())
    {
        const auto count = ::send(socket, bytes.data(), bytes.size(),
                                  MSG_NOSIGNAL);
        if (count <= 0) return;
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
}

class FakeOllamaServer final
{
public:
    FakeOllamaServer(int status, std::vector<std::string> responseParts,
                     std::chrono::milliseconds partDelay = 0ms)
        : status_(status), responseParts_(std::move(responseParts)),
          partDelay_(partDelay)
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) throw std::runtime_error("socket failed");
        int reuse = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) != 0 ||
            ::listen(listener_, 1) != 0)
            throw std::runtime_error("bind/listen failed");
        socklen_t length = sizeof(address);
        if (::getsockname(listener_, reinterpret_cast<sockaddr *>(&address),
                          &length) != 0)
            throw std::runtime_error("getsockname failed");
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~FakeOllamaServer()
    {
        ::shutdown(listener_, SHUT_RDWR);
        ::close(listener_);
        if (thread_.joinable()) thread_.join();
    }

    std::string baseUrl() const
    {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    bool waitAccepted(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return stateChanged_.wait_for(lock, timeout,
                                      [this] { return accepted_; });
    }

    std::string request() const
    {
        std::lock_guard lock(mutex_);
        return request_;
    }

private:
    void serve() noexcept
    {
        sockaddr_in peer{};
        socklen_t peerLength = sizeof(peer);
        const int client = ::accept(listener_, reinterpret_cast<sockaddr *>(&peer),
                                    &peerLength);
        if (client < 0) return;
        {
            std::lock_guard lock(mutex_);
            accepted_ = true;
        }
        stateChanged_.notify_all();

        std::string request;
        std::size_t expected = std::string::npos;
        char buffer[4096];
        while (true)
        {
            const auto count = ::recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            request.append(buffer, static_cast<std::size_t>(count));
            const auto headerEnd = request.find("\r\n\r\n");
            if (headerEnd != std::string::npos && expected == std::string::npos)
            {
                expected = headerEnd + 4;
                const auto name = request.find("Content-Length:");
                if (name != std::string::npos)
                    expected += static_cast<std::size_t>(std::stoul(
                        request.substr(name + std::strlen("Content-Length:"))));
            }
            if (expected != std::string::npos && request.size() >= expected)
                break;
        }
        {
            std::lock_guard lock(mutex_);
            request_ = request;
        }

        std::size_t bodySize = 0;
        for (const auto &part : responseParts_) bodySize += part.size();
        const std::string reason = status_ == 200 ? "OK" : "Not Found";
        sendAll(client, "HTTP/1.1 " + std::to_string(status_) + " " + reason +
                        "\r\nContent-Type: application/x-ndjson\r\n"
                        "Content-Length: " + std::to_string(bodySize) +
                        "\r\nConnection: close\r\n\r\n");
        for (std::size_t index = 0; index < responseParts_.size(); ++index)
        {
            sendAll(client, responseParts_[index]);
            if (index + 1 < responseParts_.size())
                std::this_thread::sleep_for(partDelay_);
        }
        ::shutdown(client, SHUT_RDWR);
        ::close(client);
    }

    int listener_ = -1;
    std::uint16_t port_ = 0;
    int status_;
    std::vector<std::string> responseParts_;
    std::chrono::milliseconds partDelay_;
    mutable std::mutex mutex_;
    std::condition_variable stateChanged_;
    bool accepted_ = false;
    std::string request_;
    std::thread thread_;
};

struct CompletionWaiter final
{
    void set(GenerationCompletion value)
    {
        {
            std::lock_guard lock(mutex);
            completion = std::move(value);
            done = true;
        }
        changed.notify_one();
    }

    GenerationCompletion wait()
    {
        std::unique_lock lock(mutex);
        CHECK(changed.wait_for(lock, 5s, [this] { return done; }));
        return completion;
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    GenerationCompletion completion;
};

GenerationRequest requestFor(std::string id)
{
    GenerationRequest request;
    request.requestId = std::move(id);
    request.requester = 7;
    request.messages = {{"system", "answer briefly"},
                        {"user", "say \"hello\"\nnow"}};
    request.maxOutputTokens = 23;
    request.modelArtifact = "qwen2.5:0.5b";
    return request;
}

void verifyIncrementalDecoder()
{
    OllamaStreamDecoder decoder(256);
    CHECK(decoder.feed("{\"message\":{\"content\":\"he").empty());
    const auto first = decoder.feed("llo\"},\"done\":false}\n"
                                    "{\"message\":{\"content\":\"\\u4f60");
    CHECK(first.size() == 1 && first.front().content == "hello" &&
          !first.front().done);
    const auto second = decoder.feed("\\u597d\"},\"done\":false}\n"
                                     "{\"done\":true}\n");
    CHECK(second.size() == 2 && second[0].content == "你好" &&
          second[1].done && decoder.sawTerminalEvent());
    CHECK(decoder.finish().empty());

    bool rejected = false;
    try
    {
        OllamaStreamDecoder malformed(32);
        (void)malformed.feed("{\"message\":{\"content\":false}}\n");
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    CHECK(rejected);

    rejected = false;
    try
    {
        OllamaStreamDecoder oversized(8);
        (void)oversized.feed("{\"done\":true}\n");
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    CHECK(rejected);

    rejected = false;
    try
    {
        OllamaStreamDecoder afterDone(64);
        (void)afterDone.feed("{\"done\":true}\n{\"done\":false}\n");
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    CHECK(rejected);
}

void verifyConfigurationAndInputLimits()
{
    bool rejected = false;
    try
    {
        OllamaModelProviderConfig invalid;
        invalid.baseUrl = "file:///tmp/model";
        OllamaModelProvider provider(invalid);
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    CHECK(rejected);

    OllamaModelProviderConfig limited;
    limited.baseUrl = "http://127.0.0.1:1";
    limited.maxRequestBytes = 16;
    OllamaModelProvider provider(limited);
    CompletionWaiter waiter;
    provider.generate(requestFor("too-large"), {},
                      [&](GenerationCompletion result) {
                          waiter.set(std::move(result));
                      });
    const auto completion = waiter.wait();
    CHECK(!completion.success &&
          completion.error.find("exceeds configured limit") !=
              std::string::npos);
}

void verifySuccessfulStreaming()
{
    FakeOllamaServer server(
        200,
        {"{\"message\":{\"role\":\"assistant\",\"content\":\"phase\"},",
         "\"done\":false}\n{\"message\":{\"content\":\" eleven\"},"
         "\"done\":false}\n{\"done\":true,\"eval_count\":2}\n"},
        10ms);
    OllamaModelProviderConfig config;
    config.baseUrl = server.baseUrl();
    config.maxConcurrent = 2;
    config.maxQueued = 2;
    OllamaModelProvider provider(config);

    std::mutex tokenMutex;
    std::string tokens;
    CompletionWaiter waiter;
    provider.generate(
        requestFor("success"),
        [&](std::string_view token) {
            std::lock_guard lock(tokenMutex);
            tokens.append(token);
        },
        [&](GenerationCompletion result) { waiter.set(std::move(result)); });
    const auto completion = waiter.wait();
    CHECK(completion.success && !completion.cancelled && completion.error.empty());
    {
        std::lock_guard lock(tokenMutex);
        CHECK(tokens == "phase eleven");
    }
    CHECK(server.waitAccepted(1s));
    const auto wire = server.request();
    CHECK(wire.find("POST /api/chat HTTP/1.1") != std::string::npos);
    CHECK(wire.find("\"model\":\"qwen2.5:0.5b\"") != std::string::npos);
    CHECK(wire.find("say \\\"hello\\\"\\nnow") != std::string::npos);
    CHECK(wire.find("\"num_predict\":23") != std::string::npos);
}

void verifyRemoteError()
{
    FakeOllamaServer server(404, {"{\"error\":\"model not found\"}\n"});
    OllamaModelProviderConfig config;
    config.baseUrl = server.baseUrl();
    OllamaModelProvider provider(config);
    CompletionWaiter waiter;
    provider.generate(requestFor("error"), {},
                      [&](GenerationCompletion result) {
                          waiter.set(std::move(result));
                      });
    const auto completion = waiter.wait();
    CHECK(!completion.success && !completion.cancelled);
    CHECK(completion.error == "model not found");
}

void verifyCancellationAndAdmissionBound()
{
    FakeOllamaServer server(
        200,
        {"{\"message\":{\"content\":\"first\"},\"done\":false}\n",
         "{\"message\":{\"content\":\"late\"},\"done\":false}\n"
         "{\"done\":true}\n"},
        1200ms);
    OllamaModelProviderConfig config;
    config.baseUrl = server.baseUrl();
    config.maxConcurrent = 1;
    config.maxQueued = 0;
    config.requestTimeout = 5s;
    OllamaModelProvider provider(config);

    CancellationToken cancellation;
    CompletionWaiter first;
    provider.generate(
        requestFor("cancel"),
        [&](std::string_view) { cancellation.cancel(); },
        [&](GenerationCompletion result) { first.set(std::move(result)); },
        cancellation);
    CHECK(server.waitAccepted(2s));

    CompletionWaiter rejected;
    provider.generate(requestFor("queue-full"), {},
                      [&](GenerationCompletion result) {
                          rejected.set(std::move(result));
                      });
    const auto rejectedResult = rejected.wait();
    CHECK(!rejectedResult.success &&
          rejectedResult.error.find("queue is full") != std::string::npos);
    const auto cancelledResult = first.wait();
    CHECK(!cancelledResult.success && cancelledResult.cancelled);
    CHECK(cancelledResult.error == "Ollama generation cancelled");
}

} // namespace

int main()
{
    verifyIncrementalDecoder();
    verifyConfigurationAndInputLimits();
    verifySuccessfulStreaming();
    verifyRemoteError();
    verifyCancellationAndAdmissionBound();
    std::cout << "phase11 Ollama provider tests passed\n";
    return 0;
}
