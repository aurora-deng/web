#include "server/phase11/ai/OllamaModelProvider/OllamaModelProvider.h"

#include "server/phase11/ai/OllamaStreamDecoder/OllamaStreamDecoder.h"
#include "server/phase11/transport/FlatJson/FlatJson.h"

#include <curl/curl.h>

#include <array>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace webserver::phase11
{
namespace
{

void ensureCurlGlobalInit()
{
    struct CurlRuntime final
    {
        CurlRuntime()
        {
            const auto code = curl_global_init(CURL_GLOBAL_DEFAULT);
            if (code != CURLE_OK)
                throw std::runtime_error("curl_global_init failed");
        }
        ~CurlRuntime() { curl_global_cleanup(); }
    };
    static CurlRuntime runtime;
    (void)runtime;
}

std::string normalizeBaseUrl(std::string value)
{
    const bool http = value.rfind("http://", 0) == 0;
    const bool https = value.rfind("https://", 0) == 0;
    if (!http && !https)
        throw std::invalid_argument("Ollama base URL must use http or https");
    const auto authority = value.find("//") + 2;
    if (authority >= value.size() || value.find_first_of("?#\r\n\t ") !=
                                       std::string::npos)
        throw std::invalid_argument("invalid Ollama base URL");
    while (value.size() > authority && value.back() == '/')
        value.pop_back();
    return value;
}

std::string buildRequestBody(const GenerationRequest &request)
{
    using transport::quoteJson;
    std::string body = "{\"model\":" + quoteJson(request.modelArtifact) +
                       ",\"messages\":[";
    bool first = true;
    for (const auto &message : request.messages)
    {
        if (!first) body.push_back(',');
        first = false;
        body += "{\"role\":" + quoteJson(message.role) +
                ",\"content\":" + quoteJson(message.content) + "}";
    }
    body += "],\"stream\":true,\"options\":{\"num_predict\":" +
            std::to_string(request.maxOutputTokens) + "}}";
    return body;
}

} // namespace

class OllamaModelProvider::Impl final
{
public:
    explicit Impl(OllamaModelProviderConfig config)
        : config_(std::move(config))
    {
        if (config_.maxConcurrent == 0 || config_.maxRequestBytes == 0 ||
            config_.maxResponseLineBytes == 0 ||
            config_.connectTimeout.count() <= 0 ||
            config_.requestTimeout.count() <= 0)
            throw std::invalid_argument("invalid Ollama provider limits");
        config_.baseUrl = normalizeBaseUrl(std::move(config_.baseUrl));
        ensureCurlGlobalInit();
        multi_ = curl_multi_init();
        if (!multi_)
            throw std::runtime_error("curl_multi_init failed");
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~Impl()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        curl_multi_wakeup(multi_);
        if (worker_.joinable())
            worker_.join();
        curl_multi_cleanup(multi_);
    }

    bool ready() const noexcept
    {
        std::lock_guard lock(mutex_);
        return !stopping_;
    }

    GenerationHandle generate(const GenerationRequest &request,
                              TokenSink onToken,
                              CompletionSink onComplete,
                              CancellationToken cancellation)
    {
        const GenerationHandle handle{request.requestId, cancellation};
        std::string validationError;
        if (request.requestId.empty())
            validationError = "Ollama request id is required";
        else if (request.modelArtifact.empty())
            validationError = "Ollama model artifact is required";
        else if (request.messages.empty())
            validationError = "Ollama prompt messages are required";
        else if (request.maxOutputTokens == 0)
            validationError = "Ollama max output tokens must be positive";
        else if (!onComplete)
            validationError = "Ollama completion callback is required";

        std::string body;
        if (validationError.empty())
        {
            body = buildRequestBody(request);
            if (body.size() > config_.maxRequestBytes)
                validationError = "Ollama request exceeds configured limit";
        }
        if (!validationError.empty())
        {
            completeDirect(onComplete, {false, false, validationError, {}});
            return handle;
        }

        auto transfer = std::make_shared<Transfer>(
            request.requestId, std::move(body), std::move(onToken),
            std::move(onComplete), cancellation, config_.maxResponseLineBytes);

        bool accepted = false;
        {
            std::lock_guard lock(mutex_);
            const auto capacity = config_.maxConcurrent + config_.maxQueued;
            if (!stopping_ && outstanding_ < capacity)
            {
                ++outstanding_;
                pending_.push_back(transfer);
                accepted = true;
            }
        }
        if (!accepted)
        {
            completeDirect(transfer->onComplete,
                           {false, cancellation.cancelled(),
                            "Ollama provider queue is full or stopping", {}});
            return handle;
        }
        ready_.notify_one();
        curl_multi_wakeup(multi_);
        return handle;
    }

private:
    struct Transfer final
    {
        Transfer(std::string id, std::string requestBody, TokenSink token,
                 CompletionSink complete, CancellationToken cancel,
                 std::size_t maxLineBytes)
            : requestId(std::move(id)), body(std::move(requestBody)),
              onToken(std::move(token)), onComplete(std::move(complete)),
              cancellation(std::move(cancel)), decoder(maxLineBytes)
        {
            errorBuffer.fill('\0');
        }

        std::string requestId;
        std::string body;
        TokenSink onToken;
        CompletionSink onComplete;
        CancellationToken cancellation;
        OllamaStreamDecoder decoder;
        CURL *easy = nullptr;
        curl_slist *headers = nullptr;
        std::array<char, CURL_ERROR_SIZE> errorBuffer{};
        std::string streamError;
    };

    static void completeDirect(const CompletionSink &sink,
                               GenerationCompletion completion) noexcept
    {
        if (!sink) return;
        try
        {
            sink(std::move(completion));
        }
        catch (...)
        {
            // 外部完成回调不能越过 libcurl Worker 的线程入口。
        }
    }

    static size_t writeCallback(char *data, size_t size, size_t count,
                                void *opaque) noexcept
    {
        auto *transfer = static_cast<Transfer *>(opaque);
        if (count != 0 && size > std::numeric_limits<size_t>::max() / count)
            return 0;
        const auto bytes = size * count;
        try
        {
            for (const auto &event : transfer->decoder.feed(
                     std::string_view(data, bytes)))
            {
                if (!event.error.empty())
                {
                    transfer->streamError = event.error;
                    continue;
                }
                if (!event.content.empty() && transfer->onToken)
                    transfer->onToken(event.content);
            }
            return bytes;
        }
        catch (const std::exception &error)
        {
            transfer->streamError = error.what();
            return 0;
        }
        catch (...)
        {
            transfer->streamError = "Ollama token callback failed";
            return 0;
        }
    }

    static int progressCallback(void *opaque, curl_off_t, curl_off_t,
                                curl_off_t, curl_off_t) noexcept
    {
        const auto *transfer = static_cast<const Transfer *>(opaque);
        return transfer->cancellation.cancelled() ? 1 : 0;
    }

    template <typename Value>
    static void setOption(CURL *easy, CURLoption option, Value value)
    {
        const auto code = curl_easy_setopt(easy, option, value);
        if (code != CURLE_OK)
            throw std::runtime_error(std::string("curl_easy_setopt failed: ") +
                                     curl_easy_strerror(code));
    }

    void startTransfer(const std::shared_ptr<Transfer> &transfer)
    {
        transfer->easy = curl_easy_init();
        if (!transfer->easy)
            throw std::runtime_error("curl_easy_init failed");
        transfer->headers = curl_slist_append(transfer->headers,
                                              "Content-Type: application/json");
        if (!transfer->headers)
            throw std::runtime_error("cannot allocate Ollama HTTP headers");
        const auto url = config_.baseUrl + "/api/chat";
        setOption(transfer->easy, CURLOPT_URL, url.c_str());
        setOption(transfer->easy, CURLOPT_HTTPHEADER, transfer->headers);
        setOption(transfer->easy, CURLOPT_POST, 1L);
        setOption(transfer->easy, CURLOPT_POSTFIELDS, transfer->body.data());
        setOption(transfer->easy, CURLOPT_POSTFIELDSIZE_LARGE,
                  static_cast<curl_off_t>(transfer->body.size()));
        setOption(transfer->easy, CURLOPT_WRITEFUNCTION, &writeCallback);
        setOption(transfer->easy, CURLOPT_WRITEDATA, transfer.get());
        setOption(transfer->easy, CURLOPT_XFERINFOFUNCTION, &progressCallback);
        setOption(transfer->easy, CURLOPT_XFERINFODATA, transfer.get());
        setOption(transfer->easy, CURLOPT_NOPROGRESS, 0L);
        setOption(transfer->easy, CURLOPT_ERRORBUFFER,
                  transfer->errorBuffer.data());
        setOption(transfer->easy, CURLOPT_CONNECTTIMEOUT_MS,
                  static_cast<long>(config_.connectTimeout.count()));
        setOption(transfer->easy, CURLOPT_TIMEOUT_MS,
                  static_cast<long>(config_.requestTimeout.count()));
        setOption(transfer->easy, CURLOPT_NOSIGNAL, 1L);
        setOption(transfer->easy, CURLOPT_TCP_KEEPALIVE, 1L);
        setOption(transfer->easy, CURLOPT_FOLLOWLOCATION, 0L);
        setOption(transfer->easy, CURLOPT_PROTOCOLS_STR, "http,https");
        setOption(transfer->easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        setOption(transfer->easy, CURLOPT_ACCEPT_ENCODING, "");
        setOption(transfer->easy, CURLOPT_PRIVATE, transfer.get());

        const auto code = curl_multi_add_handle(multi_, transfer->easy);
        if (code != CURLM_OK)
            throw std::runtime_error(std::string("curl_multi_add_handle failed: ") +
                                     curl_multi_strerror(code));
        active_.emplace(transfer->easy, transfer);
    }

    void cleanupEasy(const std::shared_ptr<Transfer> &transfer) noexcept
    {
        if (transfer->easy)
        {
            curl_multi_remove_handle(multi_, transfer->easy);
            curl_easy_cleanup(transfer->easy);
            transfer->easy = nullptr;
        }
        if (transfer->headers)
        {
            curl_slist_free_all(transfer->headers);
            transfer->headers = nullptr;
        }
    }

    void finish(const std::shared_ptr<Transfer> &transfer, CURLcode code,
                bool forcedShutdown = false) noexcept
    {
        long status = 0;
        if (transfer->easy)
            curl_easy_getinfo(transfer->easy, CURLINFO_RESPONSE_CODE, &status);
        if (code == CURLE_OK && transfer->streamError.empty())
        {
            try
            {
                for (const auto &event : transfer->decoder.finish())
                {
                    if (!event.error.empty())
                        transfer->streamError = event.error;
                    else if (!event.content.empty() && transfer->onToken)
                        transfer->onToken(event.content);
                }
            }
            catch (const std::exception &error)
            {
                transfer->streamError = error.what();
            }
            catch (...)
            {
                transfer->streamError = "Ollama token callback failed";
            }
        }
        cleanupEasy(transfer);

        GenerationCompletion result;
        result.cancelled = forcedShutdown || transfer->cancellation.cancelled();
        if (result.cancelled)
            result.error = forcedShutdown ? "Ollama provider is shutting down"
                                          : "Ollama generation cancelled";
        else if (!transfer->streamError.empty())
            result.error = transfer->streamError;
        else if (code != CURLE_OK)
        {
            result.error = transfer->errorBuffer.front() != '\0'
                               ? transfer->errorBuffer.data()
                               : curl_easy_strerror(code);
        }
        else if (status < 200 || status >= 300)
            result.error = "Ollama returned HTTP status " +
                           std::to_string(status);
        else if (!transfer->decoder.sawTerminalEvent())
            result.error = "Ollama stream ended without done event";
        else
            result.success = true;

        {
            std::lock_guard lock(mutex_);
            if (outstanding_ > 0) --outstanding_;
        }
        completeDirect(transfer->onComplete, std::move(result));
    }

    void processCompleted()
    {
        int messages = 0;
        while (auto *message = curl_multi_info_read(multi_, &messages))
        {
            if (message->msg != CURLMSG_DONE)
                continue;
            const auto found = active_.find(message->easy_handle);
            if (found == active_.end())
                continue;
            auto transfer = found->second;
            active_.erase(found);
            finish(transfer, message->data.result);
        }
    }

    void workerLoop() noexcept
    {
        for (;;)
        {
            std::vector<std::shared_ptr<Transfer>> starting;
            std::vector<std::shared_ptr<Transfer>> cancelled;
            bool stopping = false;
            {
                std::unique_lock lock(mutex_);
                if (!stopping_ && pending_.empty() && active_.empty())
                    ready_.wait(lock, [this] { return stopping_ ||
                                                       !pending_.empty(); });
                stopping = stopping_;
                if (stopping)
                {
                    while (!pending_.empty())
                    {
                        cancelled.push_back(std::move(pending_.front()));
                        pending_.pop_front();
                    }
                }
                else
                {
                    while (!pending_.empty() &&
                           active_.size() + starting.size() <
                               config_.maxConcurrent)
                    {
                        starting.push_back(std::move(pending_.front()));
                        pending_.pop_front();
                    }
                }
            }

            for (auto &transfer : cancelled)
                finish(transfer, CURLE_ABORTED_BY_CALLBACK, true);
            if (stopping)
            {
                std::vector<std::shared_ptr<Transfer>> active;
                active.reserve(active_.size());
                for (auto &[easy, transfer] : active_)
                    active.push_back(transfer);
                active_.clear();
                for (auto &transfer : active)
                    finish(transfer, CURLE_ABORTED_BY_CALLBACK, true);
                return;
            }

            for (auto &transfer : starting)
            {
                if (transfer->cancellation.cancelled())
                {
                    finish(transfer, CURLE_ABORTED_BY_CALLBACK);
                    continue;
                }
                try
                {
                    startTransfer(transfer);
                }
                catch (const std::exception &error)
                {
                    transfer->streamError = error.what();
                    finish(transfer, CURLE_FAILED_INIT);
                }
                catch (...)
                {
                    transfer->streamError = "cannot start Ollama transfer";
                    finish(transfer, CURLE_FAILED_INIT);
                }
            }

            int running = 0;
            const auto perform = curl_multi_perform(multi_, &running);
            if (perform != CURLM_OK)
            {
                std::vector<std::shared_ptr<Transfer>> failed;
                for (auto &[easy, transfer] : active_)
                    failed.push_back(transfer);
                active_.clear();
                for (auto &transfer : failed)
                {
                    transfer->streamError = curl_multi_strerror(perform);
                    finish(transfer, CURLE_RECV_ERROR);
                }
                continue;
            }
            processCompleted();

            if (!active_.empty())
            {
                int descriptors = 0;
                (void)curl_multi_poll(multi_, nullptr, 0, 100, &descriptors);
            }
        }
    }

    OllamaModelProviderConfig config_;
    CURLM *multi_ = nullptr;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::shared_ptr<Transfer>> pending_;
    std::unordered_map<CURL *, std::shared_ptr<Transfer>> active_;
    std::size_t outstanding_ = 0;
    bool stopping_ = false;
    std::thread worker_;
};

OllamaModelProvider::OllamaModelProvider(OllamaModelProviderConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

OllamaModelProvider::~OllamaModelProvider() = default;

bool OllamaModelProvider::ready() const noexcept
{
    return impl_ && impl_->ready();
}

GenerationHandle OllamaModelProvider::generate(
    const GenerationRequest &request, TokenSink onToken,
    CompletionSink onComplete, CancellationToken cancellation)
{
    return impl_->generate(request, std::move(onToken),
                           std::move(onComplete), std::move(cancellation));
}

} // namespace webserver::phase11
