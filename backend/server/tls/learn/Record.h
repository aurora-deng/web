#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// 只拆 TLS 记录的外层 5 字节信封，不解密/验证 payload。
// TLS 1.3 加密记录外层 type 通常为 application_data；真实内部类型被加密。
struct Record {
    std::uint8_t outerType;
    std::uint16_t legacyVersion;
    std::string payload;
};

class RecordParser {
public:
    std::vector<Record> feed(std::string_view bytes) {
        pending_.append(bytes);
        std::vector<Record> ready;
        while (pending_.size() >= 5) {
            const auto *p = reinterpret_cast<const unsigned char *>(pending_.data());
            const auto length = (static_cast<std::size_t>(p[3]) << 8) | p[4];
            // RFC 8446: TLS 1.3 密文记录的最大长度是 2^14 + 256。
            if (length > 16384 + 256)
                throw std::length_error("TLS record exceeds teaching limit");
            if (pending_.size() < length + 5)
                break; // TCP 可以把 5 字节头或 payload 拆成任意片段。
            ready.push_back(Record{
                p[0],
                static_cast<std::uint16_t>((p[1] << 8) | p[2]),
                pending_.substr(5, length)});
            pending_.erase(0, length + 5);
        }
        return ready;
    }

private:
    std::string pending_;
};
