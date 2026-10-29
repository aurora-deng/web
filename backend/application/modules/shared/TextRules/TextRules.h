#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
#include <cstddef>

namespace webserver::function3 {
/** 按 Unicode 标量计数，拒绝非法 UTF-8、控制字符和超长正文；不按字节误伤中文。 */
inline std::size_t textLength(std::string_view text, std::size_t maximum) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        std::uint32_t point = first;
        int more = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { more = 1; point &= 31; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { more = 2; point &= 15; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { more = 3; point &= 7; minimum = 0x10000; }
        else if (first >= 0x80) throw BusinessError(Failure::Invalid, "invalid UTF-8");
        while (more--) {
            if (i == text.size()) throw BusinessError(Failure::Invalid, "truncated UTF-8");
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) throw BusinessError(Failure::Invalid, "invalid UTF-8 continuation");
            point = (point << 6) | (next & 63);
        }
        if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff) ||
            (point < 32 && point != 10 && point != 9) || point == 127 || ++count > maximum)
            throw BusinessError(Failure::Invalid, "text is invalid or exceeds limit");
    }
    return count;
}
inline std::string trimText(std::string text) {
    const auto first = text.find_first_not_of(" \r\n\t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \r\n\t") - first + 1);
}
inline void requestKey(std::string_view key) {
    if (key.empty() || key.size() > 128) throw BusinessError(Failure::Invalid, "request key required");
    for (unsigned char c : key) if (c < 33 || c > 126)
        throw BusinessError(Failure::Invalid, "invalid request key");
}
}
