// ==============================================================================
// 文件名：HttpParser.cpp
// 职责比喻：这是"快递分拣总调度"的实际工作流程。
//   实现 HttpParser.h 中声明的 parse() 主循环和 reset()。parse() 是状态机的"心脏"：
//   它在一个 while(true) 里根据当前 stage 调用对应子解析器，根据返回结果决定切换到哪个
//   状态、还是返回（等数据 / 出错 / 完成）。整个 HTTP 请求的"拆包"过程就在这里被串起来。
//
// 关键技术点（初学者重点理解）：
//   1. 【状态机循环】while(true) + switch(stage) 是状态机的经典写法。每个 case 处理一个阶段，
//      子解析器返回非 OK 就立即 return（暂停状态机），下次 parse() 从同一 stage 继续。
//   2. 【结果只交付一次】COMPLETE 状态用 resultDelivered_ 保证一个完整请求只返回一次 PARSE_OK，
//      防止误用导致"一个请求被处理两次"。
//   3. 【reset 不清 Buffer】keep-alive 连接复用时，Buffer 里可能已有下一个请求的数据，reset 只
//      重置解析器状态，保留 Buffer 内容，让下一轮直接解析。
// ==============================================================================
#include "HttpParser.h"

// 状态机持续推进，直到请求完成、需要更多字节或发现错误。
// TCP 没有消息边界，因此不能假设一次 recv 对应一次请求；这种增量设计同时覆盖半包与粘包。
// HttpParser 只负责协调 stage 状态机与三个子解析器之间的状态传递，具体解析逻辑由子解析器完成。
/**
 * @brief 解析主循环：状态机持续推进
 * @param buffer 连接读缓冲区
 * @param req 输出请求对象
 * @return PARSE_OK（完整）/ PARSE_NEED_MORE（等数据）/ PARSE_ERROR（出错）
 *
 * 通俗解释：调度员站在传送带前，看当前该干哪步（stage）：
 *   - REQUEST_LINE：让请求行分拣员拆第一行；拆完把 keepAlive 默认值告诉头部分拣员，进入 HEADERS；
 *   - HEADERS：让头部分拣员逐行拆；拆完把 contentLength/chunked 等告诉正文分拣员，进入 BODY 或 CHUNK；
 *   - BODY/CHUNK_*：让正文分拣员取货物；取完进入 COMPLETE；
 *   - COMPLETE：交付一次 PARSE_OK，第二次再进这里就报错（防止重复交付）。
 *   任何子分拣员说"数据不够"或"出错"，调度员立刻 return，等下次新数据再来继续。
 */
ParseState HttpParser::parse(Buffer &buffer, HttpRequest &req)
{
    while (true)
    {
        switch (stage)
        {
        // ---- 阶段 1：解析请求行（GET /path?query HTTP/1.1） ----
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
        // ---- 阶段 2：解析头部若干行，直到空行 ----
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

            // chunked 走分块状态机，否则走固定长度正文
            if (headerParser_.chunked())
                stage = ParseStage::CHUNK_SIZE;
            else
                stage = ParseStage::BODY;

            break;
        }
        // ---- 阶段 3：解析固定长度正文（Content-Length） ----
        case ParseStage::BODY:
        {
            auto s = bodyParser_.parse(buffer, req);

            if (s != PARSE_OK)
                return s;

            stage = ParseStage::COMPLETE;

            break;
        }
        // ---- 阶段 4：完成态，交付一次 PARSE_OK ----
        case ParseStage::COMPLETE:
            // 一个完整请求只能交付一次；调用方必须 reset 后才能解析 Buffer 中的下一请求。
            // 这可避免独立使用 Parser 时重复调用 parse 得到第二个虚假的 PARSE_OK。
            if (resultDelivered_)
                return PARSE_ERROR;
            resultDelivered_ = true;
            return PARSE_OK;
        // ---- chunked 分支：读取当前块的十六进制长度行 ----
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

        // ---- chunked 分支：读取当前块的数据 + CRLF ----
        case ParseStage::CHUNK_DATA:
        {

            auto s = bodyParser_.parseChunkData(buffer, req);

            if (s != PARSE_OK)
                return s;

            // 一块读完，回到 CHUNK_SIZE 读下一块的长度
            stage = ParseStage::CHUNK_SIZE;

            break;
        }
        // ---- chunked 分支：读取尾部字段，直到空行结束 ----
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

/**
 * @brief 重置解析器，准备解析下一个请求
 *
 * 通俗解释：调度员把状态机拨回 REQUEST_LINE 起点，清空三个子分拣员的进度记录，
 *   准备迎接下一个包裹。但传送带（Buffer）上剩下的货不动——那可能是下一个请求的开头。
 */
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
