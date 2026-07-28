#pragma once
#ifndef HEADER_PARSER_H
#define HEADER_PARSER_H

#include "../http.h"
#include "../../Buffer/Buffer.h"
#include "ParserUtils.h"

#include <limits>
#include <string>

// 负责解析 HTTP 首部，维护 framing 头（Content-Length / Transfer-Encoding / Connection / Range）。
// 这些头决定请求边界，由 Parser 自己维护，不能只作为普通键值交给业务层。
// keepAlive_ 初始值由 RequestLineParser 根据 HTTP 版本设置，Connection 头可覆盖。
class HeaderParser
{
public:
    ParseState parse(Buffer &buf, HttpRequest &req);
    void reset();

    // 由 HttpParser 在请求行解析完成后调用，传入版本决定的默认值。
    void setKeepAlive(bool v) { keepAlive_ = v; }

    size_t contentLength() const { return contentLength_; }
    bool keepAlive() const { return keepAlive_; }
    bool chunked() const { return chunked_; }
    bool hasContentLength() const { return hasContentLength_; }
    bool hasRange() const { return hasRange_; }
    const RangeInfo &range() const { return range_; }
    // 累计的首部字节数（不含请求行），BodyParser 解析 trailer 时在此基础上继续累计。
    size_t headerBytes() const { return headerBytes_; }

private:
    // 仅接受单个 bytes 范围，并区分闭区间、开放结尾和后缀长度三种形式。
    // 具体范围是否超出文件由响应层结合文件大小裁剪或拒绝，解析层只保证语法和数值安全。
    bool parseRange(const std::string &s, RangeInfo &range);

    size_t contentLength_ = 0;
    bool keepAlive_ = true;
    bool hasRange_ = false;
    RangeInfo range_;
    // chunked 与 Content-Length 互斥，避免请求边界歧义带来的请求走私风险。
    bool chunked_ = false;
    bool hasContentLength_ = false;
    bool hasHost_ = false;
    size_t headerBytes_ = 0;
};

inline ParseState HeaderParser::parse(Buffer &buf, HttpRequest &req)
{
    // 每确认一行就消费一行并累计字节；若当前行尚未完整则原样保留，等待下一次 recv。
    // 先做减法形式的剩余额度检查，可避免 size_t 加法溢出后绕过 kMaxHeaderBytes。
    while (true)
    {
        const char *lineEnd = buf.findCRLF();

        if (lineEnd == buf.beginWrite())
        {
            if (buf.readableBytes() > kMaxHeaderBytes - headerBytes_)
                return PARSE_ERROR;
            return PARSE_NEED_MORE;
        }

        const size_t lineBytes =
            static_cast<size_t>(lineEnd - buf.peek()) + 2;
        if (lineBytes > kMaxHeaderBytes - headerBytes_)
            return PARSE_ERROR;
        headerBytes_ += lineBytes;

        // 空行
        if (lineEnd == buf.peek())
        {
            // HTTP/1.1 要求且只允许一个 Host；缺失 Host 会让虚拟主机和代理产生路由歧义。
            if (req.version == "HTTP/1.1" && !hasHost_)
                return PARSE_ERROR;
            buf.retrieve(2);

            return PARSE_OK;
        }

        std::string header(buf.peek(), lineEnd);

        auto pos = header.find(':');

        if (pos == std::string::npos)
            return PARSE_ERROR;

        std::string rawKey = header.substr(0, pos);
        if (!isHttpToken(rawKey))
            return PARSE_ERROR;
        std::string key = toLower(rawKey);

        std::string value = trim(header.substr(pos + 1));
        for (unsigned char c : value)
        {
            if ((c < 0x20 && c != '\t') || c == 0x7f)
                return PARSE_ERROR;
        }

        req.headers[key] = value;
        if (key == "host")
        {
            if (hasHost_ || value.empty())
                return PARSE_ERROR;
            hasHost_ = true;
        }
        else if (key == "content-length")
        {
            // 重复 Content-Length 或与 chunked 并存会产生不同解析边界，直接拒绝以消除歧义。
            if (chunked_ || hasContentLength_)
                return PARSE_ERROR;
            if (value.empty() ||
                value.find_first_not_of("0123456789") != std::string::npos)
            {
                return PARSE_ERROR;
            }

            try
            {
                const auto parsedLength = std::stoull(value);
                if (parsedLength > std::numeric_limits<size_t>::max())
                    return PARSE_ERROR;
                contentLength_ = static_cast<size_t>(parsedLength);
                if (contentLength_ > kMaxBodyBytes)
                    return PARSE_ERROR;
                hasContentLength_ = true;
            }
            catch (...)
            {
                return PARSE_ERROR;
            }
        }

        else if (key == "connection")
        {
            const auto v = toLower(value);
            bool sawKeepAlive = false;
            bool sawClose = false;
            size_t begin = 0;
            while (begin <= v.size())
            {
                const size_t comma = v.find(',', begin);
                const std::string token = trim(v.substr(
                    begin,
                    comma == std::string::npos
                        ? std::string::npos
                        : comma - begin));
                if (token == "close")
                    sawClose = true;
                else if (token == "keep-alive")
                    sawKeepAlive = true;
                if (comma == std::string::npos)
                    break;
                begin = comma + 1;
            }
            // close 具有最高优先级，避免组合字段被误判为可复用连接。
            if (sawClose)
                keepAlive_ = false;
            else if (sawKeepAlive)
                keepAlive_ = true;
        }
        else if (key == "transfer-encoding")
        {
            auto v = toLower(value);

            if (v != "chunked" || hasContentLength_ || chunked_)
                return PARSE_ERROR;
            chunked_ = true;
        }
        else if (key == "range")
        {

            if (!parseRange(value, range_))
            {
                return PARSE_ERROR;
            }

            hasRange_ = true;
            req.range = range_;
        }
        buf.retrieve((lineEnd - buf.peek()) + 2);
    }
}

inline void HeaderParser::reset()
{
    contentLength_ = 0;
    keepAlive_ = true;
    hasRange_ = false;
    range_ = {};
    chunked_ = false;
    hasContentLength_ = false;
    hasHost_ = false;
    headerBytes_ = 0;
}

inline bool HeaderParser::parseRange(const std::string &s, RangeInfo &range)
{
    if (!s.starts_with("bytes="))
        return false;

    auto pos = s.find('-', 6);

    if (pos == std::string::npos)
    {
        return false;
    }
    // stuoll类型转换
    std::string left = s.substr(6, pos - 6);
    std::string right = s.substr(pos + 1);
    auto parseNumber = [](const std::string &text, size_t &value)
    {
        if (text.empty() ||
            text.find_first_not_of("0123456789") != std::string::npos)
        {
            return false;
        }
        try
        {
            const auto parsed = std::stoull(text);
            if (parsed > std::numeric_limits<size_t>::max())
                return false;
            value = static_cast<size_t>(parsed);
            return true;
        }
        catch (...)
        {
            return false;
        }
    };

    range = {};
    range.enable = true;

    if (!left.empty() && !right.empty())
    {
        if (!parseNumber(left, range.begin) ||
            !parseNumber(right, range.end) ||
            range.begin > range.end)
            return false;
    }
    else if (!left.empty())
    {
        if (!parseNumber(left, range.begin))
            return false;
        range.end = SIZE_MAX;
    }
    else if (!right.empty())
    {
        range.suffix = true;
        if (!parseNumber(right, range.end) || range.end == 0)
            return false;
    }
    else
    {
        return false;
    }
    return true;
}

#endif
