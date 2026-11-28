#pragma once

#include "backend/ai/inference/contract/ModelProvider/ModelProvider.h"
#include "backend/application/modules/shared/transport/FlatJson/FlatJson.h"

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace webserver::phase11::benchmark
{
inline std::size_t boundedNumber(std::string_view value, std::size_t maximum)
{
    std::size_t number = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size() || !number || number > maximum)
        throw std::invalid_argument("benchmark integer outside allowed range");
    return number;
}

struct Case
{
    std::string id, category, system, prompt;
    std::size_t maxTokens = 0;
};

inline Case parseCase(std::string_view line)
{
    if (line.empty() || line.size() > 16384)
        throw std::invalid_argument("benchmark case exceeds input limit");
    const auto fields = transport::parseFlatStringObject(line);
    if (fields.size() != 5 || !fields.contains("id") || !fields.contains("category") ||
        !fields.contains("system") || !fields.contains("prompt") || !fields.contains("maxTokens"))
        throw std::invalid_argument("benchmark case has unexpected fields");
    Case result{fields.at("id"), fields.at("category"), fields.at("system"),
                fields.at("prompt"), boundedNumber(fields.at("maxTokens"), 512)};
    const auto safeId = [](const std::string &s) {
        return !s.empty() && s.size() <= 64 &&
            s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") == s.npos;
    };
    if (!safeId(result.id) || !safeId(result.category) || result.system.size() > 2048 ||
        result.prompt.empty() || result.prompt.size() > 8192)
        throw std::invalid_argument("benchmark case violates bounds");
    return result;
}

struct Measurement
{
    GenerationCompletion completion;
    std::string text;
    std::size_t textChunks = 0;
    std::uint64_t wallMicroseconds = 0;
    std::optional<std::uint64_t> clientFirstTokenMicroseconds;
    bool timedOut = false, outputLimitExceeded = false;
};

inline Measurement generate(IModelProvider &provider, GenerationRequest request,
                            std::chrono::milliseconds timeout,
                            std::chrono::milliseconds cancelGrace = std::chrono::seconds(30))
{
    struct Pending
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::optional<GenerationCompletion> completion;
        std::string text;
        std::size_t chunks = 0;
        std::optional<std::chrono::steady_clock::time_point> first, finished;
        bool overflow = false;
    };
    auto pending = std::make_shared<Pending>();
    CancellationToken cancellation;
    const auto started = std::chrono::steady_clock::now();
    provider.generate(request,
        [pending, cancellation](std::string_view token) {
            std::lock_guard lock(pending->mutex);
            if (pending->completion || pending->overflow) return;
            if (token.size() > 65536 - pending->text.size()) {
                pending->overflow = true;
                cancellation.cancel();
                return;
            }
            if (!pending->first) pending->first = std::chrono::steady_clock::now();
            pending->text.append(token);
            ++pending->chunks;
        },
        [pending](GenerationCompletion completion) {
            {
                std::lock_guard lock(pending->mutex);
                if (pending->completion) return;
                pending->finished = std::chrono::steady_clock::now();
                pending->completion = std::move(completion);
            }
            pending->changed.notify_all();
        }, cancellation);
    std::unique_lock lock(pending->mutex);
    const auto done = [&] { return pending->completion.has_value(); };
    bool timedOut = !pending->changed.wait_for(lock, timeout, done);
    if (timedOut) {
        cancellation.cancel();
        if (!pending->changed.wait_for(lock, cancelGrace, done))
            throw std::runtime_error("benchmark cancellation grace exceeded");
    }
    Measurement measured;
    measured.completion = *pending->completion;
    measured.text = pending->text;
    measured.textChunks = pending->chunks;
    measured.timedOut = timedOut;
    measured.outputLimitExceeded = pending->overflow;
    measured.wallMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(*pending->finished - started).count());
    if (pending->first)
        measured.clientFirstTokenMicroseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(*pending->first - started).count());
    return measured;
}

inline std::string json(const Case &test, std::size_t repeat, std::size_t ordinal,
                        const Measurement &m)
{
    const auto q = transport::quoteJson;
    const auto &c = m.completion;
    const bool validUsage = c.usage && c.usage->valid() && c.usage->outputTokens;
    std::string out = "{\"kind\":\"generation\",\"id\":" + q(test.id) +
        ",\"category\":" + q(test.category) + ",\"repeat\":" + std::to_string(repeat) +
        ",\"ordinal\":" + std::to_string(ordinal) +
        ",\"maxTokens\":" + std::to_string(test.maxTokens) +
        ",\"success\":" + (c.success && !m.timedOut && !m.outputLimitExceeded ? "true" : "false") +
        ",\"cancelled\":" + (c.cancelled ? "true" : "false") +
        ",\"timedOut\":" + (m.timedOut ? "true" : "false") +
        ",\"outputLimitExceeded\":" + (m.outputLimitExceeded ? "true" : "false") +
        ",\"validUsage\":" + (validUsage ? "true" : "false") +
        ",\"wallMicroseconds\":" + std::to_string(m.wallMicroseconds) +
        ",\"clientFirstTokenMicroseconds\":" +
        (m.clientFirstTokenMicroseconds ? std::to_string(*m.clientFirstTokenMicroseconds) : "null") +
        ",\"textChunks\":" + std::to_string(m.textChunks) + ",\"text\":" + q(m.text);
    if (validUsage) {
        const auto &u = *c.usage;
        out += ",\"usage\":{\"promptTokens\":" + std::to_string(u.promptTokens) +
            ",\"outputTokens\":" + std::to_string(u.outputTokens) +
            ",\"decodeTokens\":" + std::to_string(u.decodeTokens) +
            ",\"decodeMicroseconds\":" + std::to_string(u.decodeMicroseconds) +
            ",\"firstTokenMicroseconds\":" + std::to_string(u.firstTokenMicroseconds) +
            ",\"modelLoadPerformed\":" + (u.modelLoadPerformed ? "true" : "false") +
            ",\"modelLoadMicroseconds\":" + std::to_string(u.modelLoadMicroseconds) +
            ",\"queueWaitMicroseconds\":" + std::to_string(u.queueWaitMicroseconds) + "}";
    }
    return out + "}";
}
} // namespace webserver::phase11::benchmark
