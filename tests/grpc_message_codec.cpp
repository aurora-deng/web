#include "server/grpc/learn/GrpcMessageCodec.h"

#include <cassert>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace
{
std::string asString(const grpc_learn::Message &message)
{
    return {message.payload.begin(), message.payload.end()};
}
}

int main()
{
    using grpc_learn::FeedStatus;

    // 任意分片：即使 5 字节头和负载逐字节到达，也只能在完整后吐出消息。
    const auto encoded = grpc_learn::encodeMessage("fragmented");
    grpc_learn::MessageDecoder fragmented;
    std::vector<grpc_learn::Message> decoded;
    for (const auto byte : encoded)
    {
        const auto result = fragmented.feed(
            std::span<const std::uint8_t>(&byte, 1));
        assert(result.status != FeedStatus::ProtocolError);
        decoded.insert(decoded.end(), result.messages.begin(), result.messages.end());
    }
    assert(decoded.size() == 1);
    assert(asString(decoded.front()) == "fragmented");
    assert(fragmented.bufferedBytes() == 0);

    // 粘包：一次输入可以连续解析空消息、普通消息和带压缩标志的消息。
    auto wire = grpc_learn::encodeMessage("");
    const auto plain = grpc_learn::encodeMessage("plain");
    const auto compressed = grpc_learn::encodeMessage("gzip-bytes", true);
    wire.insert(wire.end(), plain.begin(), plain.end());
    wire.insert(wire.end(), compressed.begin(), compressed.end());
    grpc_learn::MessageDecoder coalesced;
    const auto many = coalesced.feed(wire);
    assert(many.status == FeedStatus::MessagesReady);
    assert(many.messages.size() == 3);
    assert(asString(many.messages[0]).empty());
    assert(asString(many.messages[1]) == "plain");
    assert(many.messages[2].compressed);
    assert(asString(many.messages[2]) == "gzip-bytes");

    // maxMessageBytes 限制“单盒大小”，不限制一次 feed 中合法盒子的数量。
    grpc_learn::MessageDecoder smallMessages(4);
    auto twoSmall = grpc_learn::encodeMessage("1234");
    const auto anotherSmall = grpc_learn::encodeMessage("5678");
    twoSmall.insert(twoSmall.end(), anotherSmall.begin(), anotherSmall.end());
    const auto two = smallMessages.feed(twoSmall);
    assert(two.status == FeedStatus::MessagesReady);
    assert(two.messages.size() == 2);

    // 非 0/1 压缩标志是协议错误；失败后必须 reset，避免错位继续解析。
    grpc_learn::MessageDecoder invalid;
    const std::vector<std::uint8_t> badFlag{2, 0, 0, 0, 0};
    assert(invalid.feed(badFlag).status == FeedStatus::ProtocolError);
    assert(invalid.failed());
    assert(invalid.feed({}).status == FeedStatus::ProtocolError);
    invalid.reset();
    assert(!invalid.failed());

    // 声明长度超过上限时，在负载到来前立即拒绝，避免无界缓存。
    grpc_learn::MessageDecoder limited(4);
    const std::vector<std::uint8_t> tooLarge{0, 0, 0, 0, 5};
    const auto rejected = limited.feed(tooLarge);
    assert(rejected.status == FeedStatus::ProtocolError);
    assert(!rejected.error.empty());

    return 0;
}
