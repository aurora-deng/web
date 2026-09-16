#pragma once

#include "server/sse/SseEvent.h"

#include <string>
#include <string_view>

/**
 * SSE 的纯编码层。
 *
 * 第一层把业务事件编码成 event-stream 文本，第二层把文本包装成 HTTP chunk。
 * 两层分开后可以独立测试，也不会把 SSE 语义塞进 TransportWriter。
 */
class SseCodec
{
public:
    static std::string encodeEvent(const SseEvent &event);
    static std::string encodeComment(std::string_view comment);
    static std::string encodeChunk(std::string_view payload);
};
