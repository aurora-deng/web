#pragma once

#include <cstdint>
#include <optional>
#include <string>

using SseClientId = std::uint64_t;

/** 一条待发送的 SSE 业务事件；空字段不会写入协议文本。 */
struct SseEvent
{
    std::string data;
    std::string eventName;
    std::string id;
    std::optional<std::uint64_t> retryMilliseconds;
};
