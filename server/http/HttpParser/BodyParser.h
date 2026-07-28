#pragma once
#ifndef BODY_PARSER_H
#define BODY_PARSER_H

#include "../http.h"
#include "../../Buffer/Buffer.h"
#include "ParserUtils.h"

#include <limits>
#include <string>

// 负责解析 HTTP 正文：固定长度正文和 chunked 分块正文。
// contentLength_ / chunked_ / headerBytes_ 由 HttpParser 在首部解析完成后从 HeaderParser 传入；
// headerBytes_ 用于 trailer 的总额度检查，防止攻击者借 trailer 绕过首部长度限制。
class BodyParser
{
public:
    // 处理固定长度正文
    ParseState parse(Buffer &buf, HttpRequest &req);
    // chunked：读取分块长度行
    ParseState parseChunkSize(Buffer &buf);
    // chunked：读取一块分块数据
    ParseState parseChunkData(Buffer &buf, HttpRequest &req);
    // chunked：读取尾部字段
    ParseState parseChunkTrailers(Buffer &buf);
    void reset();

    // 由 HttpParser 在首部解析完成后调用，把 HeaderParser 的结果传入。
    void setContentLength(size_t v) { contentLength_ = v; }
    void setChunked(bool v) { chunked_ = v; }
    void setHeaderBytes(size_t v) { headerBytes_ = v; }

    size_t contentLength() const { return contentLength_; }
    bool chunked() const { return chunked_; }
    size_t currentChunkSize() const { return currentChunkSize_; }
    size_t bodyBytes() const { return bodyBytes_; }
    size_t headerBytes() const { return headerBytes_; }

private:
    size_t contentLength_ = 0;
    bool chunked_ = false;
    size_t currentChunkSize_ = 0;
    // headerBytes_ 同时累计普通头和 trailer，bodyBytes_ 累计所有 chunk，
    // 防止攻击者通过拆成多行或多个小块绕过单次长度检查。
    size_t headerBytes_ = 0;
    size_t bodyBytes_ = 0;
};

inline ParseState BodyParser::parse(Buffer &buf, HttpRequest &req)
{
    // 固定长度正文必须整体到齐后再写入请求对象；不足时不消费 Buffer，
    // 保证下一次读取可以从同一正文起点继续。
    if (contentLength_ > kMaxBodyBytes)
        return PARSE_ERROR;
    if (buf.readableBytes() < contentLength_)
        return PARSE_NEED_MORE;

    req.bodyData.assign(buf.peek(), contentLength_);
    bodyBytes_ = contentLength_;
    req.bodySize = contentLength_;
    // 清空缓存
    buf.retrieve(contentLength_);

    return PARSE_OK;
}

inline ParseState BodyParser::parseChunkSize(Buffer &buf)
{
    // chunk-size 为十六进制，可带扩展参数；这里只接受扩展前的完整数字。
    // 0 长度块不包含数据，状态直接进入 trailer，非零块则等待“数据 + CRLF”整体到齐。
    const char *lineEnd = buf.findCRLF();
    if (lineEnd == buf.beginWrite())
    {
        if (buf.readableBytes() > kMaxRequestLineBytes)
            return PARSE_ERROR;
        return PARSE_NEED_MORE;
    }
    if (static_cast<size_t>(lineEnd - buf.peek()) > kMaxRequestLineBytes)
        return PARSE_ERROR;
    std::string len(buf.peek(), lineEnd);
    try
    {
        const auto extension = len.find(';');
        const std::string sizeText = trim(len.substr(0, extension));
        if (sizeText.empty())
            return PARSE_ERROR;
        size_t parsed = 0;
        const auto parsedSize = std::stoull(sizeText, &parsed, 16);
        if (parsed != sizeText.size())
            return PARSE_ERROR;
        if (parsedSize > std::numeric_limits<size_t>::max())
            return PARSE_ERROR;
        currentChunkSize_ = static_cast<size_t>(parsedSize);
        if (currentChunkSize_ > SIZE_MAX - 2)
            return PARSE_ERROR;
    }
    catch (...)
    {
        return PARSE_ERROR;
    }
    buf.retrieve((lineEnd - buf.peek()) + 2);
    // stage 转换由 HttpParser 状态机根据 currentChunkSize_ 决定。
    return PARSE_OK;
}

inline ParseState BodyParser::parseChunkData(Buffer &buf, HttpRequest &req)
{
    // 使用“上限 - 已接收量”检查累计大小，避免加法溢出；只有数据及结尾 CRLF 都到齐才消费，
    // 从而在非阻塞半包下不会丢掉 chunk 的开头。
    if (bodyBytes_ > kMaxBodyBytes ||
        currentChunkSize_ > kMaxBodyBytes - bodyBytes_)
    {
        return PARSE_ERROR;
    }
    if (buf.readableBytes() < currentChunkSize_ + 2)
    {
        return PARSE_NEED_MORE;
    }
    const char *chunkEnd = buf.peek() + currentChunkSize_;
    if (chunkEnd[0] != '\r' || chunkEnd[1] != '\n')
        return PARSE_ERROR;
    req.bodyData.append(buf.peek(), currentChunkSize_);
    bodyBytes_ += currentChunkSize_;
    req.bodySize = req.bodyData.size();
    buf.retrieve(currentChunkSize_ + 2);
    // stage 转回 CHUNK_SIZE 由 HttpParser 状态机负责。
    return PARSE_OK;
}

inline ParseState BodyParser::parseChunkTrailers(Buffer &buf)
{
    // trailer 当前只校验基本字段形状而不暴露给业务层，但仍计入头部总额度；
    // 这样未来扩展 trailer 存储时协议边界已正确，且不能借 trailer 绕过头部限制。
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
        if (lineEnd == buf.peek())
        {
            buf.retrieve(2);
            return PARSE_OK;
        }
        std::string trailer(buf.peek(), lineEnd);
        if (trailer.find(':') == std::string::npos)
            return PARSE_ERROR;
        buf.retrieve((lineEnd - buf.peek()) + 2);
    }
}

inline void BodyParser::reset()
{
    contentLength_ = 0;
    chunked_ = false;
    currentChunkSize_ = 0;
    headerBytes_ = 0;
    bodyBytes_ = 0;
}

#endif
