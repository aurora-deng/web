#include "server/http2/Http2Codec.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace
{
constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr size_t kMaxRequestBody = 1024 * 1024;
constexpr size_t kMaxResponseBody = 1024 * 1024;
constexpr size_t kMaxConcurrentStreams = 32;

nghttp2_nv header(const std::string &name, const std::string &value)
{
    return {reinterpret_cast<uint8_t *>(const_cast<char *>(name.data())),
            reinterpret_cast<uint8_t *>(const_cast<char *>(value.data())),
            name.size(), value.size(), NGHTTP2_NV_FLAG_NONE};
}
} // namespace

Http2Codec::Http2Codec()
{
    nghttp2_session_callbacks *callbacks = nullptr;
    if (nghttp2_session_callbacks_new(&callbacks) != 0)
        return;
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, onBeginHeaders);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, onHeader);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, onData);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, onFrame);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, onClose);
    const int result = nghttp2_session_server_new(&session_, callbacks, this);
    nghttp2_session_callbacks_del(callbacks);
    if (result != 0)
        return;
    nghttp2_settings_entry settings[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 32},
        {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, static_cast<uint32_t>(kMaxHeaderBytes)}};
    if (nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, settings, 2) != 0)
    {
        nghttp2_session_del(session_);
        session_ = nullptr;
    }
}

Http2Codec::~Http2Codec()
{
    if (session_)
        nghttp2_session_del(session_);
}

bool Http2Codec::receive(const char *data, size_t length)
{
    if (!session_)
        return false;
    const auto consumed = nghttp2_session_mem_recv(
        session_, reinterpret_cast<const uint8_t *>(data), length);
    return consumed >= 0 && static_cast<size_t>(consumed) == length;
}

std::vector<Http2Codec::ReadyRequest> Http2Codec::takeReady()
{
    auto result = std::move(ready_);
    ready_.clear();
    return result;
}

std::vector<int32_t> Http2Codec::takeClosed()
{
    auto result = std::move(closed_);
    closed_.clear();
    return result;
}

int Http2Codec::onBeginHeaders(nghttp2_session *, const nghttp2_frame *frame, void *user)
{
    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST)
    {
        auto *self = static_cast<Http2Codec *>(user);
        if (self->incoming_.size() >= kMaxConcurrentStreams)
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        auto [it, inserted] = self->incoming_.try_emplace(frame->hd.stream_id);
        if (inserted)
            it->second.request.streamId = frame->hd.stream_id;
    }
    return 0;
}

int Http2Codec::onHeader(nghttp2_session *, const nghttp2_frame *frame,
                         const uint8_t *name, size_t nameLen,
                         const uint8_t *value, size_t valueLen, uint8_t, void *user)
{
    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST)
        return 0;
    auto *self = static_cast<Http2Codec *>(user);
    auto it = self->incoming_.find(frame->hd.stream_id);
    if (it == self->incoming_.end())
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    auto &stream = it->second;
    if (nameLen > kMaxHeaderBytes || valueLen > kMaxHeaderBytes - nameLen ||
        stream.headerBytes > kMaxHeaderBytes - nameLen - valueLen)
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    stream.headerBytes += nameLen + valueLen;
    const std::string key(reinterpret_cast<const char *>(name), nameLen);
    const std::string val(reinterpret_cast<const char *>(value), valueLen);
    if (key == ":method")
        stream.request.method = val;
    else if (key == ":path")
        stream.request.rawPath = val;
    else if (key == ":authority")
        stream.request.headers["host"] = val;
    else if (!key.empty() && key.front() != ':')
        stream.request.headers[key] = val;
    return 0;
}

int Http2Codec::onData(nghttp2_session *, uint8_t, int32_t streamId,
                       const uint8_t *data, size_t length, void *user)
{
    auto *self = static_cast<Http2Codec *>(user);
    auto it = self->incoming_.find(streamId);
    if (it == self->incoming_.end() ||
        length > kMaxRequestBody - std::min(it->second.request.body.size(), kMaxRequestBody))
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    it->second.request.body.append(reinterpret_cast<const char *>(data), length);
    return 0;
}

int Http2Codec::onFrame(nghttp2_session *, const nghttp2_frame *frame, void *user)
{
    if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
        static_cast<Http2Codec *>(user)->finishRequest(frame->hd.stream_id);
    return 0;
}

void Http2Codec::finishRequest(int32_t streamId)
{
    auto it = incoming_.find(streamId);
    if (it == incoming_.end() || it->second.dispatched)
        return;
    auto &request = it->second.request;
    if (request.method.empty() || request.rawPath.empty() ||
        request.rawPath.front() != '/')
    {
        nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, streamId,
                                  NGHTTP2_PROTOCOL_ERROR);
        return;
    }
    it->second.dispatched = true;
    ready_.push_back(std::move(request));
}

int Http2Codec::onClose(nghttp2_session *, int32_t streamId, uint32_t, void *user)
{
    auto *self = static_cast<Http2Codec *>(user);
    self->incoming_.erase(streamId);
    self->outgoing_.erase(streamId);
    std::erase_if(self->ready_, [streamId](const ReadyRequest &request)
                  { return request.streamId == streamId; });
    self->closed_.push_back(streamId);
    return 0;
}

ssize_t Http2Codec::readBody(nghttp2_session *, int32_t, uint8_t *buffer,
                             size_t length, uint32_t *flags,
                             nghttp2_data_source *source, void *)
{
    auto *outgoing = static_cast<Outgoing *>(source->ptr);
    const auto count = std::min(length, outgoing->body.size() - outgoing->offset);
    if (count)
        std::memcpy(buffer, outgoing->body.data() + outgoing->offset, count);
    outgoing->offset += count;
    if (outgoing->offset == outgoing->body.size())
        *flags |= NGHTTP2_DATA_FLAG_EOF;
    return static_cast<ssize_t>(count);
}

bool Http2Codec::submitResponse(
    int32_t streamId, int status,
    const std::unordered_map<std::string, std::string> &headers,
    std::string body)
{
    if (!session_ || body.size() > kMaxResponseBody ||
        incoming_.find(streamId) == incoming_.end())
        return false;
    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back(":status", std::to_string(status));
    for (const auto &[name, value] : headers)
    {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.empty() || lower.front() == ':' || lower == "connection" ||
            lower == "keep-alive" || lower == "proxy-connection" ||
            lower == "transfer-encoding" || lower == "upgrade" ||
            lower == "content-length")
            continue;
        fields.emplace_back(std::move(lower), value);
    }
    fields.emplace_back("content-length", std::to_string(body.size()));
    std::vector<nghttp2_nv> nvs;
    nvs.reserve(fields.size());
    for (auto &field : fields)
        nvs.push_back(header(field.first, field.second));
    nghttp2_data_provider provider{};
    nghttp2_data_provider *providerPtr = nullptr;
    if (!body.empty())
    {
        auto [it, inserted] = outgoing_.try_emplace(streamId, Outgoing{std::move(body), 0});
        if (!inserted)
            return false;
        provider.source.ptr = &it->second;
        provider.read_callback = readBody;
        providerPtr = &provider;
    }
    const int result = nghttp2_submit_response(session_, streamId, nvs.data(),
                                                nvs.size(), providerPtr);
    if (result != 0)
        outgoing_.erase(streamId);
    return result == 0;
}

bool Http2Codec::drainOutput(std::string &output)
{
    output.clear();
    if (!session_)
        return false;
    const uint8_t *data = nullptr;
    while (true)
    {
        const auto length = nghttp2_session_mem_send(session_, &data);
        if (length < 0)
            return false;
        if (length == 0)
            return true;
        output.append(reinterpret_cast<const char *>(data), static_cast<size_t>(length));
        if (output.size() >= 64 * 1024)
            return true;
    }
}
