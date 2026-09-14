#include "WebSocketValidation.h"

#include "server/websocket/WebSocketTypes/WebSocketLimits.h"

#include <cstddef>

namespace {

bool isContinuation(uint8_t byte) noexcept
{
    return byte >= 0x80 && byte <= 0xBF;
}

} // namespace

bool isValidWebSocketUtf8(std::string_view text) noexcept
{
    const auto *bytes = reinterpret_cast<const uint8_t *>(text.data());
    size_t i = 0;
    while (i < text.size())
    {
        const uint8_t first = bytes[i];
        if (first <= 0x7F)
        {
            ++i;
            continue;
        }

        // 2 字节：C0/C1 会产生过长编码，因此首字节必须从 C2 开始。
        if (first >= 0xC2 && first <= 0xDF)
        {
            if (i + 1 >= text.size() || !isContinuation(bytes[i + 1]))
                return false;
            i += 2;
            continue;
        }

        if (first >= 0xE0 && first <= 0xEF)
        {
            if (i + 2 >= text.size())
                return false;
            const uint8_t second = bytes[i + 1];
            const uint8_t third = bytes[i + 2];
            // E0 A0..BF 排除过长编码；ED 80..9F 排除 UTF-16 surrogate。
            const bool secondOk =
                (first == 0xE0 && second >= 0xA0 && second <= 0xBF) ||
                (first >= 0xE1 && first <= 0xEC && isContinuation(second)) ||
                (first == 0xED && second >= 0x80 && second <= 0x9F) ||
                (first >= 0xEE && first <= 0xEF && isContinuation(second));
            if (!secondOk || !isContinuation(third))
                return false;
            i += 3;
            continue;
        }

        if (first >= 0xF0 && first <= 0xF4)
        {
            if (i + 3 >= text.size())
                return false;
            const uint8_t second = bytes[i + 1];
            const bool secondOk =
                (first == 0xF0 && second >= 0x90 && second <= 0xBF) ||
                (first >= 0xF1 && first <= 0xF3 && isContinuation(second)) ||
                (first == 0xF4 && second >= 0x80 && second <= 0x8F);
            if (!secondOk ||
                !isContinuation(bytes[i + 2]) ||
                !isContinuation(bytes[i + 3]))
            {
                return false;
            }
            i += 4;
            continue;
        }

        // 孤立续字节、F5..FF、以及其他未覆盖首字节均非法。
        return false;
    }
    return true;
}

bool isValidWebSocketCloseCode(uint16_t code) noexcept
{
    // 当前标准区间；1004/1005/1006 是保留或仅本地使用，不能上 wire。
    if (code >= 1000 && code <= 1014)
        return code != 1004 && code != 1005 && code != 1006;

    // 3000..3999 供已注册库/框架使用，4000..4999 供私有应用使用。
    return code >= 3000 && code <= 4999;
}

WsClosePayloadResult parseWebSocketClosePayload(
    std::string_view payload,
    WsCloseInfo &out)
{
    out = {};
    if (payload.empty())
        return WsClosePayloadResult::Ok;
    if (payload.size() == 1 || payload.size() > kWsMaxControlPayloadBytes)
        return WsClosePayloadResult::ProtocolError;

    const auto high = static_cast<uint8_t>(payload[0]);
    const auto low = static_cast<uint8_t>(payload[1]);
    const uint16_t code = static_cast<uint16_t>(
        (static_cast<uint16_t>(high) << 8) | low);
    if (!isValidWebSocketCloseCode(code))
        return WsClosePayloadResult::ProtocolError;

    const auto reason = payload.substr(2);
    if (!isValidWebSocketUtf8(reason))
        return WsClosePayloadResult::InvalidUtf8;

    out.hasCode = true;
    out.code = code;
    out.reason.assign(reason.data(), reason.size());
    return WsClosePayloadResult::Ok;
}
