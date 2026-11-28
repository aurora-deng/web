#include "AiGenerationAudit.h"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <openssl/evp.h>
namespace webserver::phase11::ai_chat_detail {
namespace {
constexpr std::array<std::uint64_t, 12> kLatencyBucketMicroseconds{
    10'000, 25'000, 50'000, 100'000, 250'000, 500'000,
    1'000'000, 2'500'000, 5'000'000, 10'000'000, 30'000'000, 60'000'000};
}
TimePoint currentTime()
{
    return std::chrono::system_clock::now();
}

std::string sha256(std::string_view value)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_Digest(value.data(), value.size(), digest.data(), &size,
                   EVP_sha256(), nullptr) != 1 || size != 32)
        throw std::runtime_error("SHA-256 digest failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string encoded;
    encoded.resize(size * 2U);
    for (unsigned int index = 0; index < size; ++index)
    {
        encoded[index * 2U] = hex[digest[index] >> 4U];
        encoded[index * 2U + 1U] = hex[digest[index] & 0x0FU];
    }
    return encoded;
}

std::pair<std::string, std::uint64_t> promptDigest(
    const std::vector<PromptMessage> &messages)
{
    std::string canonical;
    std::uint64_t bytes = 0;
    for (const auto &message : messages)
    {
        // 长度前缀让 ("ab","c") 与 ("a","bc") 不会产生同一规范串。
        canonical.append(std::to_string(message.role.size()));
        canonical.push_back(':');
        canonical.append(message.role);
        canonical.append(std::to_string(message.content.size()));
        canonical.push_back(':');
        canonical.append(message.content);
        bytes += message.role.size() + message.content.size();
    }
    return {sha256(canonical), bytes};
}

void observeLatency(AiChatMetricsSnapshot &metrics,
                    const AiGenerationAudit &audit) noexcept
{
    const auto micros = [](TimePoint start, TimePoint finish)
    {
        const auto value = std::chrono::duration_cast<std::chrono::microseconds>(
            finish - start).count();
        return static_cast<std::uint64_t>(std::max<std::int64_t>(0, value));
    };
    const auto observe = [](auto &buckets, std::uint64_t value)
    {
        for (std::size_t index = 0; index < kLatencyBucketMicroseconds.size();
             ++index)
            if (value <= kLatencyBucketMicroseconds[index])
                ++buckets[index];
        ++buckets.back(); // +Inf
    };
    if (audit.startedAt && audit.firstTokenAt)
    {
        const auto value = micros(*audit.startedAt, *audit.firstTokenAt);
        observe(metrics.firstTokenLatencyBuckets, value);
        ++metrics.firstTokenLatencyCount;
        metrics.firstTokenLatencyMicrosecondsTotal += value;
    }
    if (audit.completedAt)
    {
        const auto value = micros(audit.requestedAt, *audit.completedAt);
        observe(metrics.totalLatencyBuckets, value);
        ++metrics.totalLatencyCount;
        metrics.totalLatencyMicrosecondsTotal += value;
    }
}

}
