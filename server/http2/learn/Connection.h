#pragma once

#include "Frame.h"
#include "Hpack.h"
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace learn_h2 {

struct Message {
    // 一个完整 stream 方向上的 HTTP 消息；只有收到 END_STREAM 才交给上层。
    uint32_t stream_id;
    std::vector<Header> headers;
    std::string body;
};

// 内存中的 HTTP/2 一端。这里不直接操作 socket；take_output() 取走线上字节，
// receive() 喂入对端字节，main.cpp 可故意按任意大小切片来模拟 TCP。
class Connection {
public:
    explicit Connection(bool server);
    // 客户端写前言 + SETTINGS；服务端写自己的 SETTINGS。
    void start();
    void receive(std::string_view bytes);
    // 客户端创建奇数 stream；服务端在已收完请求的同一个 stream 上应答。
    void send_request(uint32_t stream_id, const std::vector<Header>& headers,
                      std::string_view body = {});
    void send_response(uint32_t stream_id, const std::vector<Header>& headers,
                       std::string_view body = {});
    void send_ping(std::string_view eight_bytes);
    // RST_STREAM 只取消单条 stream，不关闭整条 TCP 连接。
    void reset(uint32_t stream_id);
    // 教学版只演示所有 stream 完成后的干净关闭，不处理在途 stream 排空。
    void send_goaway();
    bool received_goaway() const { return goaway_received_; }
    std::string take_output();
    std::vector<Message> take_messages();
    std::vector<std::string> take_trace();

private:
    struct Stream {
        // remote_end/local_end 分别表示“对端已经发完/本端已经发完”；
        // 两者都为真时这条 stream 才能从连接表删除。
        std::vector<Header> headers;
        std::string body;
        bool remote_end = false;
        bool local_end = false;
        // 每条 stream 自己有窗口；连接级窗口在 Connection 中另存一份。
        int64_t send_window = 65535;
        int64_t recv_window = 65535;
    };
    void send(Frame frame);
    void handle(const Frame& frame);
    void send_message(uint32_t stream_id, const std::vector<Header>& headers,
                      std::string_view body);
    void finish_remote(uint32_t stream_id);
    void log(std::string text);

    bool server_;
    bool started_ = false;
    // 仅服务端需要先收满客户端 24 字节前言，才能交给帧解析器。
    bool preface_complete_ = false;
    // 前言之后的第一帧必须是非 ACK 的 SETTINGS。
    bool first_frame_ = true;
    bool goaway_sent_ = false;
    bool goaway_received_ = false;
    // 客户端新建 stream 的 ID 递增且为奇数；服务端用它拒绝倒退/重复编号。
    uint32_t last_new_stream_ = 0;
    // DATA 同时消耗连接窗口和 stream 窗口；只有 HEADERS 不消耗这些额度。
    int64_t connection_send_window_ = 65535;
    int64_t connection_recv_window_ = 65535;
    int64_t initial_stream_send_window_ = 65535;
    uint32_t peer_max_frame_ = MAX_FRAME_SIZE;
    std::string preface_bytes_;
    std::string output_;
    FrameParser parser_;
    // 发出方向与收到方向各自维护 HPACK 表，不能互换。
    Hpack encoder_;
    Hpack decoder_;
    // stream ID 是一条连接内的“订单号”，所有并发请求共享同一个 Connection。
    std::map<uint32_t, Stream> streams_;
    std::vector<Message> messages_;
    std::vector<std::string> trace_;
};

} // namespace learn_h2
