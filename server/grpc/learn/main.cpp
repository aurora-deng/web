#include "GrpcMessageCodec.h"

#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

int main()
{
    using grpc_learn::FeedStatus;

    const auto first = grpc_learn::encodeMessage("hello");
    const auto second = grpc_learn::encodeMessage("world");
    std::vector<std::uint8_t> wire = first;
    wire.insert(wire.end(), second.begin(), second.end());

    grpc_learn::MessageDecoder decoder;
    // 故意逐字节投喂，模拟“一个消息跨多个 HTTP/2 DATA 帧/TCP 包”。
    for (const auto byte : wire)
    {
        const auto result = decoder.feed(std::span<const std::uint8_t>(&byte, 1));
        if (result.status == FeedStatus::ProtocolError)
        {
            std::cerr << result.error << '\n';
            return 1;
        }
        for (const auto &message : result.messages)
        {
            std::cout << "compressed=" << message.compressed
                      << " payload="
                      << std::string(message.payload.begin(), message.payload.end())
                      << '\n';
        }
    }
    return decoder.bufferedBytes() == 0 ? 0 : 2;
}
