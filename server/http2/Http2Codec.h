#pragma once

#include <nghttp2/nghttp2.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

// One instance per TCP connection. Only its owning Reactor thread may call it.
// nghttp2 handles framing, HPACK, stream and flow-control state; this class
// translates completed streams to the existing HttpRequest/HttpResponse model.
class Http2Codec
{
public:
    struct ReadyRequest
    {
        int32_t streamId = 0;
        std::string method;
        std::string rawPath;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
    };

    Http2Codec();
    ~Http2Codec();
    Http2Codec(const Http2Codec &) = delete;
    Http2Codec &operator=(const Http2Codec &) = delete;

    bool valid() const { return session_ != nullptr; }
    bool receive(const char *data, size_t length);
    std::vector<ReadyRequest> takeReady();
    std::vector<int32_t> takeClosed();
    bool submitResponse(int32_t streamId, int status,
                        const std::unordered_map<std::string, std::string> &headers,
                        std::string body);
    // Copies nghttp2's transient output into an owned string before the next call.
    bool drainOutput(std::string &output);

private:
    struct Incoming
    {
        ReadyRequest request;
        size_t headerBytes = 0;
        bool dispatched = false;
    };
    struct Outgoing
    {
        std::string body;
        size_t offset = 0;
    };

    static int onBeginHeaders(nghttp2_session *, const nghttp2_frame *, void *);
    static int onHeader(nghttp2_session *, const nghttp2_frame *, const uint8_t *,
                        size_t, const uint8_t *, size_t, uint8_t, void *);
    static int onData(nghttp2_session *, uint8_t, int32_t, const uint8_t *, size_t, void *);
    static int onFrame(nghttp2_session *, const nghttp2_frame *, void *);
    static int onClose(nghttp2_session *, int32_t, uint32_t, void *);
    static ssize_t readBody(nghttp2_session *, int32_t, uint8_t *, size_t,
                            uint32_t *, nghttp2_data_source *, void *);

    void finishRequest(int32_t streamId);
    nghttp2_session *session_ = nullptr;
    std::unordered_map<int32_t, Incoming> incoming_;
    std::unordered_map<int32_t, Outgoing> outgoing_;
    std::vector<ReadyRequest> ready_;
    std::vector<int32_t> closed_;
};
