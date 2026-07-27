#pragma once
#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include "../http.h"

enum class ParseStage
{
    // 状态保存在连接私有解析器中，PARSE_NEED_MORE 返回后可从当前阶段继续；
    // 分块体额外经历“长度行 -> 数据 -> 尾部字段”，避免把不完整 TCP 数据误当完整请求。
    REQUEST_LINE,
    HEADERS,
    BODY,
    CHUNK_SIZE,
    CHUNK_DATA,
    CHUNK_TRAILERS,
    COMPLETE,
    ERROR
};
// 负责解析
// 增量 HTTP/1.x 请求解析器。它只消费已确认完整且合法的字节，未完成数据留在 Buffer，
// 因而能正确处理半包和粘包；三类大小上限则在分配大对象前拒绝异常输入，约束单连接资源占用。
class HttpParser
{
public:
    static constexpr size_t MAX_REQUEST_LINE_BYTES = 8 * 1024;
    static constexpr size_t MAX_HEADER_BYTES = 64 * 1024;
    static constexpr size_t MAX_BODY_BYTES = 1024 * 1024;
    ParseState parse(Buffer &buffer, HttpRequest &req);
    void reset();
    size_t getContentLength() const
    {
        return contentLength;
    }

    bool keepAlive() const
    {
        return keepAlive_;
    }

private:
    ParseStage stage = ParseStage::REQUEST_LINE;

    size_t contentLength = 0;

    bool keepAlive_ = true;

    bool hasRange = false;

    RangeInfo range;
    bool headerFinished = false;

    // chunked 与 Content-Length 互斥，避免请求边界歧义带来的请求走私风险。
    bool chunked = false;
    bool hasContentLength = false;
    size_t currentChunkSize = 0;
    // headerBytes 同时累计普通头和 trailer，bodyBytes 累计所有 chunk，
    // 防止攻击者通过拆成多行或多个小块绕过单次长度检查。
    size_t headerBytes = 0;
    size_t bodyBytes = 0;
    ParseState parseRequestLine(Buffer &, HttpRequest &);
    ParseState parseHeaders(Buffer &, HttpRequest &);
    ParseState parseChunkSize(Buffer &);
    ParseState parseChunkData(Buffer &buf, HttpRequest &req);
    ParseState parseChunkTrailers(Buffer& buf);
    ParseState parseBody(Buffer &, HttpRequest &);
};
#endif