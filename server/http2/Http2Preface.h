#pragma once

#include <algorithm>
#include <cstring>
#include <string_view>

inline constexpr std::string_view kHttp2ClientPreface =
    "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

enum class Http2PrefaceMatch { None, Partial, Full };

inline Http2PrefaceMatch matchHttp2Preface(const char *bytes, size_t length)
{
    if (length == 0 ||
        std::memcmp(bytes, kHttp2ClientPreface.data(),
                    std::min(length, kHttp2ClientPreface.size())) != 0)
        return Http2PrefaceMatch::None;
    return length >= kHttp2ClientPreface.size()
               ? Http2PrefaceMatch::Full
               : Http2PrefaceMatch::Partial;
}
