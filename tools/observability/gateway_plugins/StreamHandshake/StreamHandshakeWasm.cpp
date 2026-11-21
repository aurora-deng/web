#include "StreamHandshake.h"
#include "proxy_wasm_intrinsics.h"

namespace {
using nebula::gateway::StreamHandshake;
using nebula::gateway::StreamKind;

std::string_view view(WasmDataPtr const &header)
{
    return header ? header->view() : std::string_view{};
}

class HandshakeRoot final : public RootContext {
public:
    using RootContext::RootContext;

    bool onStart(std::size_t) override
    {
        return defineMetric(MetricType::Histogram, "nebula_sse_handshake_ms", &sseMetric_) == WasmResult::Ok &&
               defineMetric(MetricType::Histogram, "nebula_websocket_handshake_ms", &socketMetric_) == WasmResult::Ok;
    }

    void record(StreamKind stream, std::uint64_t ms) const
    {
        (void)recordMetric(stream == StreamKind::Sse ? sseMetric_ : socketMetric_, ms);
    }

private:
    std::uint32_t sseMetric_ = 0;
    std::uint32_t socketMetric_ = 0;
};

class HandshakeContext final : public Context {
public:
    using Context::Context;

    FilterHeadersStatus onRequestHeaders(std::uint32_t, bool) override
    {
        auto path = getRequestHeader(":path");
        std::uint64_t now = 0;
        if (proxy_get_current_time_nanoseconds(&now) == WasmResult::Ok)
            handshake_.begin(view(path), now);
        return FilterHeadersStatus::Continue;
    }

    FilterHeadersStatus onResponseHeaders(std::uint32_t, bool) override
    {
        if (handshake_.kind() != StreamKind::Other)
        {
            auto status = getResponseHeader(":status");
            auto mediaType = getResponseHeader("content-type");
            std::uint64_t now = 0;
            if (proxy_get_current_time_nanoseconds(&now) == WasmResult::Ok)
            {
                auto elapsed = handshake_.acceptedMs(view(status), view(mediaType), now);
                if (elapsed)
                    static_cast<HandshakeRoot *>(root())->record(handshake_.kind(), *elapsed);
            }
        }
        return FilterHeadersStatus::Continue;
    }

private:
    StreamHandshake handshake_;
};

RegisterContextFactory factory(CONTEXT_FACTORY(HandshakeContext), ROOT_FACTORY(HandshakeRoot), "nebula_stream_probe");
}
