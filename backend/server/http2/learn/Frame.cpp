#include "Frame.h"
#include <stdexcept>

namespace learn_h2 {

uint32_t read_u32(std::string_view bytes) {
    if (bytes.size() < 4) throw std::runtime_error("truncated 32-bit field");
    // 转成 uint8_t 后再移位，避免 char 的有符号性把 0x80..0xff 当成负数。
    return (uint32_t(uint8_t(bytes[0])) << 24) |
           (uint32_t(uint8_t(bytes[1])) << 16) |
           (uint32_t(uint8_t(bytes[2])) << 8) | uint8_t(bytes[3]);
}

void append_u32(std::string& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(char((value >> shift) & 0xff));
}

std::string encode_frame(const Frame& frame) {
    if (frame.payload.size() > MAX_FRAME_SIZE || frame.stream_id > 0x7fffffff)
        throw std::runtime_error("frame too large or invalid stream id");
    std::string out;
    const uint32_t length = uint32_t(frame.payload.size());
    out.reserve(9 + length);
    // 线上帧头布局：3 字节长度、1 字节类型、1 字节标志、4 字节 stream ID。
    out.push_back(char(length >> 16));
    out.push_back(char(length >> 8));
    out.push_back(char(length));
    out.push_back(char(frame.type));
    out.push_back(char(frame.flags));
    append_u32(out, frame.stream_id);
    out += frame.payload;
    return out;
}

std::vector<Frame> FrameParser::feed(std::string_view bytes) {
    pending_.append(bytes);
    std::vector<Frame> frames;
    // 至少拿到完整帧头，才知道还要等待多少 payload。
    while (pending_.size() >= 9) {
        const uint32_t length = (uint32_t(uint8_t(pending_[0])) << 16) |
                                (uint32_t(uint8_t(pending_[1])) << 8) | uint8_t(pending_[2]);
        if (length > MAX_FRAME_SIZE) throw std::runtime_error("frame exceeds teaching limit");
        if (pending_.size() < 9 + length) break;
        const uint32_t raw_id = read_u32(std::string_view(pending_).substr(5, 4));
        // stream ID 实际只有 31 位；最高位是协议保留位，本实验拒绝置位。
        if (raw_id & 0x80000000) throw std::runtime_error("reserved stream-id bit set");
        frames.push_back(Frame{uint8_t(pending_[3]), uint8_t(pending_[4]), raw_id,
                               pending_.substr(9, length)});
        pending_.erase(0, 9 + length);
        // 继续循环：本次 TCP 读取的余下字节里可能还有第二、第三帧。
    }
    return frames;
}

} // namespace learn_h2
