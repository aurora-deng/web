#include "StreamHandshake.h"

namespace nebula::gateway {

void StreamHandshake::begin(std::string_view path, std::uint64_t now) noexcept
{
    const auto route = path.substr(0, path.find('?'));
    kind_ = StreamKind::Other;
    if (route == "/ws")
        kind_ = StreamKind::WebSocket;
    else if (route == "/events")
        kind_ = StreamKind::Sse;
    started_ = now;
    recorded_ = false;
}

std::optional<std::uint64_t> StreamHandshake::acceptedMs(std::string_view status, std::string_view contentType,
                                                        std::uint64_t now) noexcept
{
    const auto mediaType = contentType.substr(0, contentType.find(';'));
    const bool accepted = (kind_ == StreamKind::WebSocket && status == "101") ||
                          (kind_ == StreamKind::Sse && status == "200" && mediaType == "text/event-stream");
    if (recorded_ || !accepted || !started_ || now < started_ || now - started_ > 120'000'000'000ULL)
        return std::nullopt;
    recorded_ = true;
    return (now - started_) / 1'000'000;
}

}
