#include "server/http2/Http2Codec.h"
#include "server/http2/Http2Preface.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

void check(bool condition, const char *expression)
{
    if (!condition)
        throw std::runtime_error(expression);
}
#define CHECK(expression) check((expression), #expression)

struct ClientObserved
{
    std::unordered_map<int32_t, std::string> status;
    std::unordered_map<int32_t, std::string> body;
};

int onHeader(nghttp2_session *, const nghttp2_frame *frame,
             const uint8_t *name, size_t nameLen, const uint8_t *value,
             size_t valueLen, uint8_t, void *user)
{
    if (std::string(reinterpret_cast<const char *>(name), nameLen) == ":status")
        static_cast<ClientObserved *>(user)->status[frame->hd.stream_id] =
            std::string(reinterpret_cast<const char *>(value), valueLen);
    return 0;
}

int onData(nghttp2_session *, uint8_t, int32_t streamId,
           const uint8_t *data, size_t len, void *user)
{
    static_cast<ClientObserved *>(user)->body[streamId].append(
        reinterpret_cast<const char *>(data), len);
    return 0;
}

nghttp2_nv nv(const char *name, const std::string &value)
{
    return {reinterpret_cast<uint8_t *>(const_cast<char *>(name)),
            reinterpret_cast<uint8_t *>(const_cast<char *>(value.data())),
            std::strlen(name), value.size(), NGHTTP2_NV_FLAG_NONE};
}

int submitGet(nghttp2_session *client, const std::string &path)
{
    const std::string method = "GET", scheme = "http", authority = "localhost";
    nghttp2_nv headers[] = {
        nv(":method", method), nv(":scheme", scheme),
        nv(":authority", authority), nv(":path", path)};
    return nghttp2_submit_request(client, nullptr, headers, 4, nullptr, nullptr);
}

struct RequestBody
{
    std::string bytes;
    size_t offset = 0;
};

ssize_t readRequestBody(nghttp2_session *, int32_t, uint8_t *buffer,
                         size_t capacity, uint32_t *flags,
                         nghttp2_data_source *source, void *)
{
    auto *body = static_cast<RequestBody *>(source->ptr);
    const size_t count = std::min(capacity, body->bytes.size() - body->offset);
    std::memcpy(buffer, body->bytes.data() + body->offset, count);
    body->offset += count;
    if (body->offset == body->bytes.size())
        *flags |= NGHTTP2_DATA_FLAG_EOF;
    return static_cast<ssize_t>(count);
}

int submitPost(nghttp2_session *client, RequestBody &body)
{
    const std::string method = "POST", scheme = "http", authority = "localhost";
    const std::string path = "/echo";
    nghttp2_nv headers[] = {
        nv(":method", method), nv(":scheme", scheme),
        nv(":authority", authority), nv(":path", path)};
    nghttp2_data_provider provider{};
    provider.source.ptr = &body;
    provider.read_callback = readRequestBody;
    return nghttp2_submit_request(client, nullptr, headers, 4, &provider, nullptr);
}

void transfer(nghttp2_session *client, Http2Codec &server)
{
    const uint8_t *bytes = nullptr;
    while (true)
    {
        const auto len = nghttp2_session_mem_send(client, &bytes);
        CHECK(len >= 0);
        if (len == 0)
            break;
        // Deliberately split the connection preface, HEADERS and HPACK bytes.
        for (size_t pos = 0; pos < static_cast<size_t>(len); pos += 3)
            CHECK(server.receive(reinterpret_cast<const char *>(bytes + pos),
                                  std::min<size_t>(3, len - pos)));
    }
}

int main()
{
    CHECK(matchHttp2Preface(kHttp2ClientPreface.data(), 1) ==
          Http2PrefaceMatch::Partial);
    CHECK(matchHttp2Preface(kHttp2ClientPreface.data(), 23) ==
          Http2PrefaceMatch::Partial);
    CHECK(matchHttp2Preface(kHttp2ClientPreface.data(), 24) ==
          Http2PrefaceMatch::Full);
    CHECK(matchHttp2Preface("POST", 4) == Http2PrefaceMatch::None);

    Http2Codec server;
    CHECK(server.valid());
    ClientObserved observed;
    nghttp2_session_callbacks *callbacks = nullptr;
    CHECK(nghttp2_session_callbacks_new(&callbacks) == 0);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, onHeader);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, onData);
    nghttp2_session *client = nullptr;
    CHECK(nghttp2_session_client_new(&client, callbacks, &observed) == 0);
    nghttp2_session_callbacks_del(callbacks);

    CHECK(nghttp2_submit_settings(client, NGHTTP2_FLAG_NONE, nullptr, 0) == 0);
    const auto first = submitGet(client, "/alpha");
    const auto second = submitGet(client, "/beta?x=1");
    CHECK(first == 1 && second == 3);
    transfer(client, server);
    auto ready = server.takeReady();
    CHECK(ready.size() == 2);
    CHECK(ready[0].streamId == first && ready[0].rawPath == "/alpha");
    CHECK(ready[1].streamId == second && ready[1].rawPath == "/beta?x=1");

    // Complete stream 3 before stream 1: one TCP connection, independent replies.
    CHECK(server.submitResponse(second, 200, {{"Content-Type", "text/plain"}}, "second"));
    CHECK(server.submitResponse(first, 201, {{"X-Test", "yes"}}, "first"));
    std::string wire;
    CHECK(server.drainOutput(wire) && !wire.empty());
    for (size_t pos = 0; pos < wire.size(); pos += 5)
    {
        const auto len = std::min<size_t>(5, wire.size() - pos);
        CHECK(nghttp2_session_mem_recv(client,
                   reinterpret_cast<const uint8_t *>(wire.data() + pos), len) ==
               static_cast<ssize_t>(len));
    }
    CHECK(observed.status[first] == "201" && observed.body[first] == "first");
    CHECK(observed.status[second] == "200" && observed.body[second] == "second");

    RequestBody upload{"request-body"};
    const auto posted = submitPost(client, upload);
    transfer(client, server);
    auto postReady = server.takeReady();
    CHECK(postReady.size() == 1);
    CHECK(postReady[0].streamId == posted && postReady[0].method == "POST");
    CHECK(postReady[0].body == "request-body");

    const auto cancelled = submitGet(client, "/cancel");
    transfer(client, server);
    CHECK(server.takeReady().size() == 1);
    CHECK(nghttp2_submit_rst_stream(client, NGHTTP2_FLAG_NONE, cancelled,
                                     NGHTTP2_CANCEL) == 0);
    transfer(client, server);
    auto closed = server.takeClosed();
    CHECK(std::find(closed.begin(), closed.end(), cancelled) != closed.end());
    nghttp2_session_del(client);
    std::cout << "HTTP/2 codec: split preface, HPACK, multiplexing, POST DATA and reset passed\n";
}
