#include "tools/observability/gateway_plugins/StreamHandshake/StreamHandshake.h"

#include <cassert>

using nebula::gateway::StreamHandshake;

int main()
{
    const std::uint64_t started = 1'000'000'000ULL;
    const std::uint64_t ended = started + 9'000'000;
    StreamHandshake socket;
    socket.begin("/ws?uid=12", started);
    assert(!socket.acceptedMs("103", "", ended));
    assert(socket.acceptedMs("101", "", ended) == 9);
    assert(!socket.acceptedMs("101", "", ended));

    StreamHandshake sse;
    sse.begin("/events", started);
    assert(!sse.acceptedMs("200", "application/json", ended));
    assert(!sse.acceptedMs("200", "text/event-stream", started - 1));
    assert(sse.acceptedMs("200", "text/event-stream; charset=utf-8", started) == 0);

    StreamHandshake ordinary;
    ordinary.begin("/api/conversations?mode=/events", started);
    assert(!ordinary.acceptedMs("200", "text/event-stream", ended));
}
