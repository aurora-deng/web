#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace learn_h2 {

// 这里只列教学流程实际处理的帧类型；type 是线上 1 字节编号。
enum FrameType : uint8_t {
    DATA = 0, HEADERS = 1, RST_STREAM = 3, SETTINGS = 4,
    PING = 6, GOAWAY = 7, WINDOW_UPDATE = 8
};
// 同一个 0x1 位在 DATA/HEADERS 表示 END_STREAM，在 SETTINGS/PING 表示 ACK；
// 必须先看帧类型，再解释 flags，不能把两者当成全局独立标志。
constexpr uint8_t END_STREAM = 0x1;
constexpr uint8_t ACK = 0x1;
constexpr uint8_t END_HEADERS = 0x4;
// 教学版始终使用默认的 16 KiB 上限，避免把帧长协商也混进首次拆帧学习。
constexpr uint32_t MAX_FRAME_SIZE = 16384;
// 客户端在首个 SETTINGS 之前发送的固定 24 字节；它本身不是普通 HTTP/2 帧。
constexpr std::string_view CLIENT_PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

// 一帧 = 9 字节帧头 + payload；stream_id 的最高位在线上是保留位。
struct Frame {
    uint8_t type = 0;
    uint8_t flags = 0;
    uint32_t stream_id = 0;
    std::string payload;
};

std::string encode_frame(const Frame& frame);
// HTTP/2 的多字节整数使用网络字节序（高位字节先传）。
uint32_t read_u32(std::string_view bytes);
void append_u32(std::string& out, uint32_t value);

// TCP 只保证有序字节流，一次读取可能只有半个帧头，也可能含多个完整帧。
class FrameParser {
public:
    std::vector<Frame> feed(std::string_view bytes);
private:
    // 未凑齐一帧的尾巴留到下一次 feed；不能丢弃，也不能提前交给上层。
    std::string pending_;
};

} // namespace learn_h2
