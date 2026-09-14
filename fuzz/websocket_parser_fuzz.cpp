#include "server/Buffer/Buffer.h"
#include "server/websocket/WebSocketParser/WebSocketParser.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, std::size_t size)
{
    Buffer buffer;
    buffer.append(
        reinterpret_cast<const char *>(data),
        size);

    WebSocketParser parser;
    WsFrame frame;
    for (int frames = 0; frames < 32 && buffer.readableBytes() > 0; ++frames)
    {
        const auto before = buffer.readableBytes();
        const auto result = parser.parse(buffer, frame);
        if (result == WsDecodeResult::NeedMore ||
            result == WsDecodeResult::Error)
            break;
        parser.reset();
        frame = {};
        if (buffer.readableBytes() >= before)
            break;
    }
    return 0;
}
