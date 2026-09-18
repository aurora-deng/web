#include "Connection.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace learn_h2 {
namespace {
std::string frame_name(uint8_t type) {
    switch (type) {
    case DATA: return "DATA";
    case HEADERS: return "HEADERS";
    case RST_STREAM: return "RST_STREAM";
    case SETTINGS: return "SETTINGS";
    case PING: return "PING";
    case GOAWAY: return "GOAWAY";
    case WINDOW_UPDATE: return "WINDOW_UPDATE";
    default: return "unknown";
    }
}
}

Connection::Connection(bool server) : server_(server), preface_complete_(!server) {}

void Connection::log(std::string text) {
    trace_.push_back(std::string(server_ ? "server " : "client ") + std::move(text));
}

void Connection::start() {
    if (started_) throw std::runtime_error("connection already started");
    started_ = true;
    // 前言不是 Frame，所以直接写入输出字节流；SETTINGS 才走 encode_frame。
    if (!server_) { output_.append(CLIENT_PREFACE); log("send connection preface"); }
    send(Frame{SETTINGS, 0, 0, {}});
}

void Connection::send(Frame frame) {
    log("send " + frame_name(frame.type) + " stream=" + std::to_string(frame.stream_id) +
        " flags=" + std::to_string(frame.flags) + " bytes=" + std::to_string(frame.payload.size()));
    output_ += encode_frame(frame);
}

void Connection::receive(std::string_view bytes) {
    if (!started_) throw std::runtime_error("call start() first");
    if (!preface_complete_) {
        // TCP 可能把 24 字节前言拆成很多次到达，逐段比对已收到的前缀。
        const size_t needed = CLIENT_PREFACE.size() - preface_bytes_.size();
        const size_t count = std::min(needed, bytes.size());
        preface_bytes_.append(bytes.substr(0, count));
        bytes.remove_prefix(count);
        if (CLIENT_PREFACE.substr(0, preface_bytes_.size()) != preface_bytes_)
            throw std::runtime_error("invalid client connection preface");
        if (preface_bytes_.size() != CLIENT_PREFACE.size()) return;
        preface_complete_ = true;
        log("receive complete connection preface");
    }
    // 前言后剩下的字节（可能还是半帧）由 FrameParser 自己缓存。
    for (const Frame& frame : parser_.feed(bytes)) handle(frame);
}

void Connection::handle(const Frame& frame) {
    log("recv " + frame_name(frame.type) + " stream=" + std::to_string(frame.stream_id) +
        " flags=" + std::to_string(frame.flags) + " bytes=" + std::to_string(frame.payload.size()));
    if (first_frame_) {
        // 两端对连接参数的第一次声明是协议建连步骤，不能被普通请求抢先。
        first_frame_ = false;
        if (frame.type != SETTINGS || frame.stream_id != 0 || (frame.flags & ACK))
            throw std::runtime_error("first frame must be non-ACK SETTINGS");
    }
    switch (frame.type) {
    case SETTINGS: {
        if (frame.stream_id != 0 || (frame.flags & ~ACK)) throw std::runtime_error("bad SETTINGS");
        if (frame.flags & ACK) {
            if (!frame.payload.empty()) throw std::runtime_error("SETTINGS ACK has payload");
            return;
        }
        if (frame.payload.size() % 6) throw std::runtime_error("bad SETTINGS length");
        // 每项参数占 6 字节：2 字节 ID + 4 字节值；未知 ID 可以忽略。
        for (size_t i = 0; i < frame.payload.size(); i += 6) {
            const uint16_t id = (uint16_t(uint8_t(frame.payload[i])) << 8) |
                                uint8_t(frame.payload[i + 1]);
            const uint32_t value = read_u32(std::string_view(frame.payload).substr(i + 2, 4));
            if (id == 1) encoder_.set_encoder_table_limit(value);
            if (id == 4) {
                // 对端调整初始 stream 窗口时，已存在 stream 的发送额度也要平移。
                if (value > 0x7fffffff) throw std::runtime_error("invalid initial window");
                const int64_t delta = int64_t(value) - initial_stream_send_window_;
                for (auto& [_, stream] : streams_) stream.send_window += delta;
                initial_stream_send_window_ = value;
            }
            if (id == 5) {
                if (value < MAX_FRAME_SIZE || value > 16777215)
                    throw std::runtime_error("invalid max frame size");
                peer_max_frame_ = value;
            }
        }
        // ACK 是一个空 SETTINGS 帧：只确认收到了参数，不能再带参数 payload。
        send(Frame{SETTINGS, ACK, 0, {}});
        return;
    }
    case HEADERS: {
        // 教学版要求一个 HEADERS 帧装下完整首部块；缺 END_HEADERS 意味着
        // 还需要 CONTINUATION，而本实验有意不实现这条扩展路径。
        if (frame.stream_id == 0 || !(frame.flags & END_HEADERS) ||
            (frame.flags & ~(END_HEADERS | END_STREAM)))
            throw std::runtime_error("unsupported HEADERS flags/CONTINUATION");
        if (frame.payload.size() > 16384) throw std::runtime_error("header block too large");
        if (server_) {
            // 客户端新建 stream 必须用更大的奇数 ID；GOAWAY 后不能再开新单。
            if (goaway_sent_ || goaway_received_ || !(frame.stream_id & 1) ||
                frame.stream_id <= last_new_stream_ ||
                streams_.contains(frame.stream_id))
                throw std::runtime_error("invalid new client stream id");
            last_new_stream_ = frame.stream_id;
            streams_[frame.stream_id].send_window = initial_stream_send_window_;
        } else if (!streams_.contains(frame.stream_id) ||
                   !streams_[frame.stream_id].headers.empty()) {
            throw std::runtime_error("unexpected response HEADERS");
        }
        Stream& stream = streams_.at(frame.stream_id);
        // HEADERS payload 不是文本行，而是 HPACK 编码后的首部块。
        stream.headers = decoder_.decode(frame.payload);
        if (frame.flags & END_STREAM) finish_remote(frame.stream_id);
        return;
    }
    case DATA: {
        auto it = streams_.find(frame.stream_id);
        if (frame.stream_id == 0 || it == streams_.end() || it->second.remote_end ||
            (frame.flags & ~END_STREAM))
            throw std::runtime_error("DATA on invalid stream");
        Stream& stream = it->second;
        const int64_t n = int64_t(frame.payload.size());
        // “两级水表”：连接总额度与本 stream 额度都必须支付本帧的字节数。
        connection_recv_window_ -= n;
        stream.recv_window -= n;
        if (connection_recv_window_ < 0 || stream.recv_window < 0 ||
            stream.body.size() + frame.payload.size() > 65535)
            throw std::runtime_error("receive window/body limit exceeded");
        stream.body += frame.payload;
        if (n) {
            // 为突出主流程，这里假设应用立刻消费数据，马上给两级窗口充值。
            // 真实服务器要在缓冲/业务确实接纳数据后再补，避免内存背压失效。
            std::string amount;
            append_u32(amount, uint32_t(n));
            send(Frame{WINDOW_UPDATE, 0, 0, amount});
            send(Frame{WINDOW_UPDATE, 0, frame.stream_id, amount});
            connection_recv_window_ += n;
            stream.recv_window += n;
        }
        if (frame.flags & END_STREAM) finish_remote(frame.stream_id);
        return;
    }
    case WINDOW_UPDATE: {
        // 收到额度更新后，后续 DATA 才有更多可发送字节；0 是非法增量。
        if (frame.payload.size() != 4) throw std::runtime_error("bad WINDOW_UPDATE");
        const uint32_t delta = read_u32(frame.payload) & 0x7fffffff;
        if (!delta) throw std::runtime_error("zero WINDOW_UPDATE");
        int64_t* window = nullptr;
        if (frame.stream_id == 0) window = &connection_send_window_;
        else if (streams_.contains(frame.stream_id)) window = &streams_.at(frame.stream_id).send_window;
        else return; // 该 stream 已关闭，本教学流程忽略迟到的窗口更新。
        *window += delta;
        if (*window > 0x7fffffff) throw std::runtime_error("window overflow");
        return;
    }
    case PING:
        // PING 固定 8 字节；非 ACK 时照抄 payload 回一个 ACK。
        if (frame.stream_id != 0 || frame.payload.size() != 8 || (frame.flags & ~ACK))
            throw std::runtime_error("bad PING");
        if (!(frame.flags & ACK)) send(Frame{PING, ACK, 0, frame.payload});
        return;
    case RST_STREAM:
        // 只删除目标 stream 的状态；其他 stream 仍可继续使用该连接。
        if (frame.stream_id == 0 || frame.payload.size() != 4)
            throw std::runtime_error("bad RST_STREAM");
        streams_.erase(frame.stream_id);
        return;
    case GOAWAY: {
        // GOAWAY 告诉对端不要再创建新 stream；前 4 字节是最后处理的 ID，
        // 后 4 字节是错误码。本例只演示 NO_ERROR 的干净收尾。
        if (frame.stream_id != 0 || frame.payload.size() < 8 || goaway_received_)
            throw std::runtime_error("bad GOAWAY");
        const uint32_t last = read_u32(frame.payload);
        const uint32_t error = read_u32(std::string_view(frame.payload).substr(4, 4));
        if ((last & 0x80000000) || error != 0)
            throw std::runtime_error("teaching GOAWAY supports NO_ERROR only");
        goaway_received_ = true;
        log("peer will not accept new streams after stream=" +
            std::to_string(last));
        return;
    }
    default:
        // 对未知帧类型保持向前兼容：忽略该帧，继续解析后续帧。
        return;
    }
}

void Connection::finish_remote(uint32_t stream_id) {
    Stream& stream = streams_.at(stream_id);
    stream.remote_end = true;
    // END_STREAM 才表示请求/响应已完整；之前只能继续积累 HEADERS/DATA。
    messages_.push_back(Message{stream_id, stream.headers, stream.body});
    if (stream.local_end) streams_.erase(stream_id);
}

void Connection::send_message(uint32_t id, const std::vector<Header>& headers,
                              std::string_view body) {
    auto it = streams_.find(id);
    if (it == streams_.end() || it->second.local_end)
        throw std::runtime_error("cannot send on closed/unknown stream");
    Stream& stream = it->second;
    const std::string block = encoder_.encode(headers);
    if (block.size() > MAX_FRAME_SIZE || body.size() > 65535)
        throw std::runtime_error("teaching message too large");
    // 教学版不做“窗口不足先挂起、恢复后续发”，而是明确报错。
    if (int64_t(body.size()) > connection_send_window_ ||
        int64_t(body.size()) > stream.send_window)
        throw std::runtime_error("send blocked by flow-control window");
    // 空 body：HEADERS 自己就是消息终点；有 body：最后一帧 DATA 才设 END_STREAM。
    send(Frame{HEADERS, uint8_t(END_HEADERS | (body.empty() ? END_STREAM : 0)), id, block});
    size_t pos = 0;
    while (pos < body.size()) {
        // HTTP/2 body 可拆成多帧；例如 20,000 字节拆成 16,384 + 3,616。
        const size_t count = std::min<size_t>(
            MAX_FRAME_SIZE, std::min<size_t>(peer_max_frame_, body.size() - pos));
        const bool last = pos + count == body.size();
        send(Frame{DATA, uint8_t(last ? END_STREAM : 0), id,
                   std::string(body.substr(pos, count))});
        pos += count;
    }
    connection_send_window_ -= body.size();
    stream.send_window -= body.size();
    stream.local_end = true;
    if (stream.remote_end) streams_.erase(it);
}

void Connection::send_request(uint32_t id, const std::vector<Header>& headers,
                              std::string_view body) {
    if (server_ || !started_ || goaway_sent_ || goaway_received_ ||
        !(id & 1) || id <= last_new_stream_)
        throw std::runtime_error("client must open increasing odd stream ids");
    last_new_stream_ = id;
    // 一条新 stream 从此加入连接表，和已有 stream 1/3 并存。
    streams_[id].send_window = initial_stream_send_window_;
    send_message(id, headers, body);
}

void Connection::send_response(uint32_t id, const std::vector<Header>& headers,
                               std::string_view body) {
    if (!server_ || !streams_.contains(id) || !streams_.at(id).remote_end)
        throw std::runtime_error("response needs a complete request stream");
    send_message(id, headers, body);
}

void Connection::send_ping(std::string_view eight_bytes) {
    if (eight_bytes.size() != 8) throw std::runtime_error("PING payload must be 8 bytes");
    send(Frame{PING, 0, 0, std::string(eight_bytes)});
}

void Connection::reset(uint32_t id) {
    if (!streams_.contains(id)) throw std::runtime_error("unknown stream for RST_STREAM");
    std::string error;
    append_u32(error, 0x8); // 0x8 是 HTTP/2 的 CANCEL 错误码。
    send(Frame{RST_STREAM, 0, id, error});
    streams_.erase(id);
}

void Connection::send_goaway() {
    if (!started_ || goaway_sent_ || !streams_.empty())
        throw std::runtime_error("teaching GOAWAY requires no active streams");
    std::string payload;
    // 教学版先要求活动 stream 全部结束，再发送最后 ID + NO_ERROR。
    append_u32(payload, last_new_stream_); // highest known client stream ID
    append_u32(payload, 0); // NO_ERROR
    send(Frame{GOAWAY, 0, 0, payload});
    goaway_sent_ = true;
}

std::string Connection::take_output() { return std::exchange(output_, {}); }
std::vector<Message> Connection::take_messages() { return std::exchange(messages_, {}); }
std::vector<std::string> Connection::take_trace() { return std::exchange(trace_, {}); }

} // namespace learn_h2
