#include "HttpParser.h"

// 状态机持续推进，直到请求完成、需要更多字节或发现错误。
// TCP 没有消息边界，因此不能假设一次 recv 对应一次请求；这种增量设计同时覆盖半包与粘包。
// HttpParser 只负责协调 stage 状态机与三个子解析器之间的状态传递，具体解析逻辑由子解析器完成。
ParseState HttpParser::parse(Buffer &buffer, HttpRequest &req)
{
    while (true)
    {
        switch (stage)
        {
        case ParseStage::REQUEST_LINE:
        {
            auto s = requestLineParser_.parse(buffer, req);

            if (s != PARSE_OK)
                return s;

            // 把请求行解析得到的 keep-alive 默认值传给 HeaderParser，Connection 头可覆盖。
            headerParser_.setKeepAlive(requestLineParser_.keepAlive());

            stage = ParseStage::HEADERS;

            break;
        }
        case ParseStage::HEADERS:
        {
            auto s = headerParser_.parse(buffer, req);

            if (s != PARSE_OK)
                return s;

            // 把 framing 状态传给 BodyParser，使其能区分固定长度正文与 chunked 正文，
            // 并在 trailer 解析时继承首部已累计的字节额度。
            bodyParser_.setContentLength(headerParser_.contentLength());
            bodyParser_.setChunked(headerParser_.chunked());
            bodyParser_.setHeaderBytes(headerParser_.headerBytes());

            if (headerParser_.chunked())
                stage = ParseStage::CHUNK_SIZE;
            else
                stage = ParseStage::BODY;

            break;
        }
        case ParseStage::BODY:
        {
            auto s = bodyParser_.parse(buffer, req);

            if (s != PARSE_OK)
                return s;

            stage = ParseStage::COMPLETE;

            break;
        }
        case ParseStage::COMPLETE:
            // 一个完整请求只能交付一次；调用方必须 reset 后才能解析 Buffer 中的下一请求。
            // 这可避免独立使用 Parser 时重复调用 parse 得到第二个虚假的 PARSE_OK。
            if (resultDelivered_)
                return PARSE_ERROR;
            resultDelivered_ = true;
            return PARSE_OK;
        case ParseStage::CHUNK_SIZE:
        {

            auto s = bodyParser_.parseChunkSize(buffer);

            if (s != PARSE_OK)
                return s;

            // 0 长度块表示 chunked 体结束，进入 trailer；非零块则等待“数据 + CRLF”整体到齐。
            if (bodyParser_.currentChunkSize() == 0)
                stage = ParseStage::CHUNK_TRAILERS;
            else
                stage = ParseStage::CHUNK_DATA;

            break;
        }

        case ParseStage::CHUNK_DATA:
        {

            auto s = bodyParser_.parseChunkData(buffer, req);

            if (s != PARSE_OK)
                return s;

            stage = ParseStage::CHUNK_SIZE;

            break;
        }
        case ParseStage::CHUNK_TRAILERS:
        {
            auto s = bodyParser_.parseChunkTrailers(buffer);
            if (s != PARSE_OK)
                return s;
            stage = ParseStage::COMPLETE;
            break;
        }
        default:

            return PARSE_ERROR;
        }
    }
}

void HttpParser::reset()
{
    // 只重置解析器自身，不清空 Buffer：其中可能已包含 keep-alive 连接的下一个请求，
    // 保留未消费字节可让 Session 下一轮直接解析，实现安全的连接复用。
    stage = ParseStage::REQUEST_LINE;
    resultDelivered_ = false;
    requestLineParser_.reset();
    headerParser_.reset();
    bodyParser_.reset();
}
