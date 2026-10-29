#include "Connection.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace learn_h2;

namespace {
void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

template <typename Fn>
void check_rejected(Fn action, const char* why) {
    try { action(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error(why);
}

// 不依赖 Linux socket：把一端产生的原始二进制字节切成小片喂给另一端。
// chunk 是本次模拟的“每次 TCP 读到多少字节”，与 HTTP/2 帧边界无关。
void deliver(Connection& from, Connection& to, size_t chunk = 5) {
    const std::string bytes = from.take_output();
    for (size_t i = 0; i < bytes.size(); i += chunk)
        to.receive(std::string_view(bytes).substr(i, chunk));
}

void print_trace(Connection& endpoint) {
    for (const auto& line : endpoint.take_trace()) std::cout << line << '\n';
}

void test_frame_and_hpack() {
    FrameParser parser;
    const std::string wire = encode_frame(Frame{PING, 0, 0, "12345678"});
    // 先只给 4 字节，再给 7 字节；只有 9 字节帧头和全部 payload 凑齐才出帧。
    check(parser.feed(wire.substr(0, 4)).empty(), "partial frame header should wait");
    check(parser.feed(wire.substr(4, 7)).empty(), "partial payload should wait");
    auto frames = parser.feed(wire.substr(11));
    check(frames.size() == 1 && frames[0].payload == "12345678", "frame roundtrip");

    Hpack encoder, decoder;
    const std::vector<Header> fields{{":method", "GET"}, {":path", "/learn"},
                                     {"x-lab", "hpack"}};
    const std::string first = encoder.encode(fields);
    check(decoder.decode(first).size() == fields.size(), "HPACK first block");
    // 第二次同一方向发送相同首部，动态表已经“记住”字面量，线上字节应更短。
    const std::string second = encoder.encode(fields);
    check(second.size() < first.size(), "dynamic table should shorten repeated block");
    check(decoder.decode(second)[2].value == "hpack", "HPACK dynamic index");
    check(encoder.dynamic_entries() == 2, "two literal fields stored dynamically");

    encoder.set_encoder_table_limit(0);
    // 把动态表容量压到 0：编码端先发容量更新，解码端也必须同步清空。
    const std::string after_reset = encoder.encode(fields);
    check(decoder.decode(after_reset).size() == fields.size(), "HPACK size update");
    check(decoder.dynamic_entries() == 0, "HPACK table eviction");
    check_rejected([&] { Hpack bad; bad.decode(std::string("\xbe", 1)); },
                   "invalid HPACK index should fail");
    check_rejected([&] { Hpack bad; bad.decode(std::string("\x80", 1)); },
                   "HPACK index zero should fail");
}

void test_connection() {
    Connection client(false), server(true);
    client.start();
    server.start();
    // 建连：前言可能被拆成很多片，之后双方互换 SETTINGS/ACK。
    deliver(client, server, 3);
    deliver(server, client, 2);
    deliver(client, server, 4);

    const std::vector<Header> req1{{":method", "GET"}, {":scheme", "http"},
                                    {":path", "/one"}, {":authority", "localhost"},
                                    {"x-course", "http2"}};
    const std::vector<Header> req3{{":method", "POST"}, {":scheme", "http"},
                                    {":path", "/two"}, {":authority", "localhost"},
                                    {"x-course", "http2"}};
    client.send_request(1, req1);
    client.send_request(3, req3, "body-3");
    // 两个请求共用同一 Connection，只靠 stream ID 区分；POST 要等 DATA 的
    // END_STREAM 到达，才能作为完整请求交给服务端“业务层”。
    deliver(client, server, 7);
    auto requests = server.take_messages();
    check(requests.size() == 2 && requests[0].stream_id == 1 &&
          requests[1].stream_id == 3 && requests[1].body == "body-3",
          "two independent request streams");
    check(requests[0].headers[2].value == "/one", "decoded path on stream 1");
    deliver(server, client); // 服务端消费 POST body 后补回连接和 stream 两级窗口。

    // 故意让 stream 3 先于 stream 1 返回：响应顺序不必等于请求顺序。
    server.send_response(3, {{":status", "200"}, {"content-type", "text/plain"}}, "second");
    server.send_response(1, {{":status", "200"}, {"content-type", "text/plain"}}, "first");
    deliver(server, client, 1);
    auto responses = client.take_messages();
    check(responses.size() == 2 && responses[0].stream_id == 3 &&
          responses[0].body == "second" && responses[1].stream_id == 1 &&
          responses[1].body == "first", "out-of-order multiplexed responses");
    deliver(client, server); // 客户端消费响应 DATA 后也会补回窗口。

    // PING 是连接级控制帧，stream ID 必须是 0；服务端回 ACK。
    client.send_ping("abcdefgh");
    deliver(client, server);
    deliver(server, client); // PING ACK

    const std::string split_body(20000, 'x');
    // 20,000 字节 body 必须拆成两帧 DATA，再按同一 stream ID 拼回。
    client.send_request(5, req3, split_body);
    deliver(client, server);
    auto split_request = server.take_messages();
    check(split_request.size() == 1 && split_request[0].body == split_body,
          "large body reassembled from multiple DATA frames");
    client.reset(5);
    deliver(client, server); // RST_STREAM 只取消 stream 5，不影响整条连接。
    check_rejected([&] { server.send_response(5, {{":status", "200"}}); },
                   "reset stream should not accept response");
    server.send_goaway();
    // 本例所有 stream 已完结；服务端 GOAWAY 后，客户端不能再创建 stream 7。
    deliver(server, client); // 先前待发的 WINDOW_UPDATE 与 GOAWAY 一起到达。
    check(client.received_goaway(), "client should receive graceful shutdown");
    check_rejected([&] { client.send_request(7, req1); },
                   "new stream after GOAWAY should fail");
    client.send_goaway();
    deliver(client, server);
    // 真实网络程序随后会关 TCP；这里两个内存对象离开作用域即可。
    check(server.received_goaway(), "server should receive peer GOAWAY");

    Connection invalid(true);
    invalid.start();
    check_rejected([&] { invalid.receive("X"); }, "invalid preface should fail");

    print_trace(client);
    print_trace(server);
}
}

int main() {
    try {
        test_frame_and_hpack();
        test_connection();
        std::cout << "PASS: frame fragmentation, HPACK static/dynamic/eviction, "
                     "SETTINGS/ACK, multiplexed streams, DATA, windows, PING, RST, GOAWAY\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
