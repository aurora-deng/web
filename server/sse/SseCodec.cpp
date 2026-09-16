#include "server/sse/SseCodec.h"

#include <sstream>

namespace
{
std::string singleLine(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const char ch : value)
    {
        if (ch == '\0')
            continue;
        if (ch == '\r' || ch == '\n')
            result.push_back(' ');
        else
            result.push_back(ch);
    }
    return result;
}

std::string normalizeNewlines(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index)
    {
        if (value[index] != '\r')
        {
            result.push_back(value[index]);
            continue;
        }
        if (index + 1 < value.size() && value[index + 1] == '\n')
            ++index;
        result.push_back('\n');
    }
    return result;
}

void appendLines(std::string &out,
                 std::string_view prefix,
                 std::string_view value)
{
    const std::string normalized = normalizeNewlines(value);
    std::size_t begin = 0;
    do
    {
        const auto end = normalized.find('\n', begin);
        out.append(prefix);
        if (end == std::string::npos)
        {
            out.append(normalized, begin, std::string::npos);
            out.push_back('\n');
            break;
        }
        out.append(normalized, begin, end - begin);
        out.push_back('\n');
        begin = end + 1;
    } while (begin <= normalized.size());
}
} // namespace

std::string SseCodec::encodeEvent(const SseEvent &event)
{
    std::string encoded;
    if (!event.id.empty())
        encoded += "id: " + singleLine(event.id) + "\n";
    if (!event.eventName.empty())
        encoded += "event: " + singleLine(event.eventName) + "\n";
    if (event.retryMilliseconds)
        encoded += "retry: " + std::to_string(*event.retryMilliseconds) + "\n";

    appendLines(encoded, "data: ", event.data);
    encoded.push_back('\n');
    return encoded;
}

std::string SseCodec::encodeComment(std::string_view comment)
{
    std::string encoded;
    appendLines(encoded, ": ", comment);
    encoded.push_back('\n');
    return encoded;
}

std::string SseCodec::encodeChunk(std::string_view payload)
{
    std::ostringstream prefix;
    prefix << std::hex << payload.size();
    return prefix.str() + "\r\n" + std::string(payload) + "\r\n";
}
