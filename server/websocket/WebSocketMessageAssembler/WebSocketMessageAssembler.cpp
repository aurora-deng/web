#include "WebSocketMessageAssembler.h"

#include <utility>

bool WebSocketMessageAssembler::canAppend(std::size_t bytes) const noexcept
{
    return bytes <= maxMessageBytes_ &&
           buffer_.size() <= maxMessageBytes_ - bytes;
}

void WebSocketMessageAssembler::reset() noexcept
{
    fragmentActive_ = false;
    firstOpcode_ = WsOpcode::Text;
    buffer_.clear();
}

WsAssemblyResult WebSocketMessageAssembler::consume(
    WsFrame frame,
    WsFrame &completeMessage)
{
    if (frame.opcode == WsOpcode::Continuation)
    {
        if (!fragmentActive_)
            return WsAssemblyResult::ProtocolError;
        if (!canAppend(frame.payload.size()))
        {
            reset();
            return WsAssemblyResult::MessageTooBig;
        }

        buffer_.append(frame.payload);
        if (!frame.fin)
            return WsAssemblyResult::Incomplete;

        completeMessage.fin = true;
        completeMessage.opcode = firstOpcode_;
        completeMessage.masked = true;
        completeMessage.payload = std::move(buffer_);
        reset();
        return WsAssemblyResult::Complete;
    }

    if (frame.opcode != WsOpcode::Text && frame.opcode != WsOpcode::Binary)
        return WsAssemblyResult::ProtocolError;

    // 一条分片消息未结束时，不允许插入另一条数据消息的首帧。
    if (fragmentActive_)
    {
        reset();
        return WsAssemblyResult::ProtocolError;
    }
    if (!canAppend(frame.payload.size()))
        return WsAssemblyResult::MessageTooBig;

    if (frame.fin)
    {
        completeMessage = std::move(frame);
        return WsAssemblyResult::Complete;
    }

    fragmentActive_ = true;
    firstOpcode_ = frame.opcode;
    buffer_ = std::move(frame.payload);
    return WsAssemblyResult::Incomplete;
}
