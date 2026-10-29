#pragma once
#ifndef WEBSOCKET_VALIDATION_H
#define WEBSOCKET_VALIDATION_H

#include <cstdint>
#include <string>
#include <string_view>

// Close payload 的语义结果要细分：格式/状态码错误回 1002，UTF-8 错误回 1007。
enum class WsClosePayloadResult
{
    Ok,
    ProtocolError,
    InvalidUtf8
};

struct WsCloseInfo
{
    bool hasCode = false;
    uint16_t code = 0;
    std::string reason;
};

/** 检查一段完整文本是否为最短、合法且位于 Unicode 范围内的 UTF-8。 */
bool isValidWebSocketUtf8(std::string_view text) noexcept;

/** 检查状态码是否允许出现在 Close 帧的 wire payload 中。 */
bool isValidWebSocketCloseCode(uint16_t code) noexcept;

/** 解析并校验 Close payload；空 payload 是合法的“未携带状态码”。 */
WsClosePayloadResult parseWebSocketClosePayload(
    std::string_view payload,
    WsCloseInfo &out);

#endif
