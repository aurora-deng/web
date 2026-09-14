#pragma once
#ifndef WEBSOCKET_MESSAGE_ASSEMBLER_H
#define WEBSOCKET_MESSAGE_ASSEMBLER_H

#include "server/websocket/WebSocketTypes/WebSocketLimits.h"
#include "server/websocket/WebSocketTypes/WebSocketTypes.h"

#include <cstddef>
#include <string>

/** Parser 回答“这一帧是否合法”，Assembler 回答“这帧放进当前消息是否合法”。 */
enum class WsAssemblyResult
{
    Incomplete,
    Complete,
    ProtocolError,
    MessageTooBig
};

/**
 * @brief 把 Text/Binary 首帧和 Continuation 帧组装成一条业务消息。
 *
 * fragmentActive_ 是明确状态，不能用 buffer_.empty() 代替：合法的第一片可以是空串。
 * 控制帧由 Session 旁路处理，因此 Ping/Pong 可以穿插在分片消息之间。
 */
class WebSocketMessageAssembler
{
public:
    explicit WebSocketMessageAssembler(
        std::size_t maxMessageBytes = kWsMaxMessageBytes)
        : maxMessageBytes_(maxMessageBytes)
    {
    }

    WsAssemblyResult consume(WsFrame frame, WsFrame &completeMessage);
    void reset() noexcept;

    bool hasPendingMessage() const noexcept { return fragmentActive_; }
    std::size_t pendingBytes() const noexcept { return buffer_.size(); }

private:
    bool canAppend(std::size_t bytes) const noexcept;

    std::size_t maxMessageBytes_ = kWsMaxMessageBytes;
    bool fragmentActive_ = false;
    WsOpcode firstOpcode_ = WsOpcode::Text;
    std::string buffer_;
};

#endif
