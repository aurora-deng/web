#include "server/grpc/learn/GrpcMessageCodec.h"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void check(bool condition, const char *expression)
{
    // assert 在 Release 中会被 NDEBUG 移除；测试检查必须在所有构建类型中生效。
    if (!condition)
        throw std::runtime_error(expression);
}
#define CHECK(expression) check((expression), #expression)

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
        CHECK(result.status != FeedStatus::ProtocolError);
        decoded.insert(decoded.end(), result.messages.begin(), result.messages.end());
    }
    CHECK(decoded.size() == 1);
    CHECK(asString(decoded.front()) == "fragmented");
    CHECK(fragmented.bufferedBytes() == 0);

    // 粘包：一次输入可以连续解析空消息、普通消息和带压缩标志的消息。
    auto wire = grpc_learn::encodeMessage("");
    const auto plain = grpc_learn::encodeMessage("plain");
    const auto compressed = grpc_learn::encodeMessage("gzip-bytes", true);
    wire.insert(wire.end(), plain.begin(), plain.end());
    wire.insert(wire.end(), compressed.begin(), compressed.end());
    grpc_learn::MessageDecoder coalesced;
    const auto many = coalesced.feed(wire);
    CHECK(many.status == FeedStatus::MessagesReady);
    CHECK(many.messages.size() == 3);
    CHECK(asString(many.messages[0]).empty());
    CHECK(asString(many.messages[1]) == "plain");
    CHECK(many.messages[2].compressed);
    CHECK(asString(many.messages[2]) == "gzip-bytes");

    // maxMessageBytes 限制“单盒大小”，不限制一次 feed 中合法盒子的数量。
    grpc_learn::MessageDecoder smallMessages(4);
    auto twoSmall = grpc_learn::encodeMessage("1234");
    const auto anotherSmall = grpc_learn::encodeMessage("5678");
    twoSmall.insert(twoSmall.end(), anotherSmall.begin(), anotherSmall.end());
    const auto two = smallMessages.feed(twoSmall);
    CHECK(two.status == FeedStatus::MessagesReady);
    CHECK(two.messages.size() == 2);

    // 非 0/1 压缩标志是协议错误；失败后必须 reset，避免错位继续解析。
    grpc_learn::MessageDecoder invalid;
    const std::vector<std::uint8_t> badFlag{2, 0, 0, 0, 0};
    CHECK(invalid.feed(badFlag).status == FeedStatus::ProtocolError);
    CHECK(invalid.failed());
    CHECK(invalid.feed({}).status == FeedStatus::ProtocolError);
    invalid.reset();
    CHECK(!invalid.failed());

    // 声明长度超过上限时，在负载到来前立即拒绝，避免无界缓存。
    grpc_learn::MessageDecoder limited(4);
    const std::vector<std::uint8_t> tooLarge{0, 0, 0, 0, 5};
    const auto rejected = limited.feed(tooLarge);
    CHECK(rejected.status == FeedStatus::ProtocolError);
    CHECK(!rejected.error.empty());

    return 0;
}
