#include "GrpcMessageCodec.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace grpc_learn
{
namespace
{
constexpr std::size_t kEnvelopeBytes = 5;

std::uint32_t readBigEndianLength(const std::uint8_t *bytes) noexcept
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}
} // namespace

std::vector<std::uint8_t> encodeMessage(
    std::span<const std::uint8_t> payload,
    bool compressed)
{
    if (payload.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("gRPC message exceeds uint32 length field");

    const auto length = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> encoded;
    encoded.reserve(kEnvelopeBytes + payload.size());
    encoded.push_back(compressed ? 1U : 0U);
    encoded.push_back(static_cast<std::uint8_t>((length >> 24U) & 0xffU));
    encoded.push_back(static_cast<std::uint8_t>((length >> 16U) & 0xffU));
    encoded.push_back(static_cast<std::uint8_t>((length >> 8U) & 0xffU));
    encoded.push_back(static_cast<std::uint8_t>(length & 0xffU));
    encoded.insert(encoded.end(), payload.begin(), payload.end());
    return encoded;
}

MessageDecoder::MessageDecoder(std::size_t maxMessageBytes)
    : maxMessageBytes_(maxMessageBytes)
{
    if (maxMessageBytes_ == 0 ||
        maxMessageBytes_ > std::numeric_limits<std::uint32_t>::max() ||
        maxMessageBytes_ > std::numeric_limits<std::size_t>::max() -
                               kEnvelopeBytes)
        throw std::invalid_argument("maxMessageBytes must fit uint32 and be positive");
}

FeedResult MessageDecoder::feed(std::span<const std::uint8_t> bytes)
{
    if (failed_)
        return {FeedStatus::ProtocolError, {}, "decoder is in failed state"};

    FeedResult result;
    std::size_t inputOffset = 0;
    do
    {
        // 每轮最多把“一条最大消息 + 信封”放进内部缓存。即使调用方一次
        // 传来很多粘连消息，也边解析边释放，不让未解析缓存跟输入总量一起长大。
        const auto capacity = maxMessageBytes_ + kEnvelopeBytes - bufferedBytes();
        const auto take = std::min(capacity, bytes.size() - inputOffset);
        buffer_.insert(buffer_.end(),
                       bytes.begin() + static_cast<std::ptrdiff_t>(inputOffset),
                       bytes.begin() + static_cast<std::ptrdiff_t>(inputOffset + take));
        inputOffset += take;

        while (buffer_.size() - readOffset_ >= kEnvelopeBytes)
        {
            const auto compressedFlag = buffer_[readOffset_];
            if (compressedFlag != 0U && compressedFlag != 1U)
                return fail("compressed flag must be 0 or 1");

            const auto length = readBigEndianLength(
                buffer_.data() + readOffset_ + 1);
            if (length > maxMessageBytes_)
                return fail("declared message length exceeds configured limit");

            const std::size_t completeBytes = kEnvelopeBytes + length;
            if (buffer_.size() - readOffset_ < completeBytes)
                break;

            Message message;
            message.compressed = compressedFlag == 1U;
            const auto payloadBegin = buffer_.begin() +
                                      static_cast<std::ptrdiff_t>(
                                          readOffset_ + kEnvelopeBytes);
            message.payload.assign(
                payloadBegin,
                payloadBegin + static_cast<std::ptrdiff_t>(length));
            result.messages.push_back(std::move(message));
            readOffset_ += completeBytes;
        }

        compact();
    } while (inputOffset < bytes.size());

    result.status = result.messages.empty()
                        ? FeedStatus::NeedMore
                        : FeedStatus::MessagesReady;
    return result;
}

void MessageDecoder::reset() noexcept
{
    buffer_.clear();
    readOffset_ = 0;
    failed_ = false;
}

std::size_t MessageDecoder::bufferedBytes() const noexcept
{
    return buffer_.size() - readOffset_;
}

FeedResult MessageDecoder::fail(std::string error)
{
    failed_ = true;
    buffer_.clear();
    readOffset_ = 0;
    return {FeedStatus::ProtocolError, {}, std::move(error)};
}

void MessageDecoder::compact()
{
    if (readOffset_ == 0)
        return;
    if (readOffset_ == buffer_.size())
    {
        buffer_.clear();
        readOffset_ = 0;
        return;
    }

    // 小量已读数据先留在前部，避免每解析一条消息都搬动整个 vector。
    if (readOffset_ >= 4096 || readOffset_ * 2 >= buffer_.size())
    {
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(readOffset_));
        readOffset_ = 0;
    }
}

} // namespace grpc_learn
