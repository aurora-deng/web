#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grpc_learn
{

/**
 * @brief gRPC DATA 负载中的一条消息。
 *
 * 可以把它想成快递盒：1 字节贴纸说明是否压缩，4 字节写盒内货物长度，
 * 后面才是 Protobuf 字节。HTTP/2 DATA 帧是货车，盒子可以跨车，也可以一车多盒。
 */
struct Message
{
    bool compressed{false};
    std::vector<std::uint8_t> payload;
};

enum class FeedStatus
{
    NeedMore,
    MessagesReady,
    ProtocolError,
};

struct FeedResult
{
    FeedStatus status{FeedStatus::NeedMore};
    std::vector<Message> messages;
    std::string error;
};

std::vector<std::uint8_t> encodeMessage(
    std::span<const std::uint8_t> payload,
    bool compressed = false);

inline std::vector<std::uint8_t> encodeMessage(
    std::string_view payload,
    bool compressed = false)
{
    return encodeMessage(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t *>(payload.data()),
            payload.size()),
        compressed);
}

/**
 * @brief 支持任意网络分片的 gRPC 消息增量解析器。
 *
 * 这里只教学 5 字节消息封装，不解析 Protobuf、不解压，也不实现 HTTP/2。
 * 一旦遇到非法压缩标志或超长消息，解析器进入失败态，必须 reset() 才能复用。
 */
class MessageDecoder
{
public:
    explicit MessageDecoder(std::size_t maxMessageBytes = 1024 * 1024);

    FeedResult feed(std::span<const std::uint8_t> bytes);
    void reset() noexcept;

    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] std::size_t bufferedBytes() const noexcept;

private:
    FeedResult fail(std::string error);
    void compact();

    std::size_t maxMessageBytes_;
    std::vector<std::uint8_t> buffer_;
    std::size_t readOffset_{0};
    bool failed_{false};
};

} // namespace grpc_learn
