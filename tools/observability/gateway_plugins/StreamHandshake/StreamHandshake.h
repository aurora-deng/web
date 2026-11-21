#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace nebula::gateway {

enum class StreamKind { Other, WebSocket, Sse };

class StreamHandshake final {
public:
    void begin(std::string_view path, std::uint64_t now) noexcept;
    StreamKind kind() const noexcept { return kind_; }
    std::optional<std::uint64_t> acceptedMs(std::string_view status, std::string_view contentType,
                                           std::uint64_t now) noexcept;

private:
    StreamKind kind_ = StreamKind::Other;
    std::uint64_t started_ = 0;
    bool recorded_ = false;
};

}
