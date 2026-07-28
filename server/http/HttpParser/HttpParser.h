#pragma once
#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include "../http.h"
#include "ParserUtils.h"
#include "RequestLineParser.h"
#include "HeaderParser.h"
#include "BodyParser.h"

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
//
// 解析逻辑被拆分为三个栈上子解析器：RequestLineParser、HeaderParser、BodyParser。
// HttpParser 保留 stage 状态机，负责协调三个子解析器并在它们之间传递 framing 状态
// （keepAlive、contentLength、chunked、headerBytes 等）。
class HttpParser
{
public:
    static constexpr size_t MAX_REQUEST_LINE_BYTES = kMaxRequestLineBytes;
    static constexpr size_t MAX_HEADER_BYTES = kMaxHeaderBytes;
    static constexpr size_t MAX_BODY_BYTES = kMaxBodyBytes;
    ParseState parse(Buffer &buffer, HttpRequest &req);
    void reset();
    size_t getContentLength() const
    {
        return headerParser_.contentLength();
    }

    bool keepAlive() const
    {
        return headerParser_.keepAlive();
    }

    bool chunked() const
    {
        return headerParser_.chunked();
    }

    const RangeInfo &range() const
    {
        return headerParser_.range();
    }

private:
    ParseStage stage = ParseStage::REQUEST_LINE;

    bool resultDelivered_ = false;

    // 三个子解析器均为栈上成员对象，无需动态分配。
    RequestLineParser requestLineParser_;
    HeaderParser headerParser_;
    BodyParser bodyParser_;
};
#endif