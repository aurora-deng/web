// ==============================================================================
// 文件名：HttpParser.h
// 职责比喻：这是"快递分拣传送带"的总调度室。
//   一条 TCP 连接上源源不断来字节流（像传送带上的包裹），HttpParser 是分拣总调度：
//   它维护一个"当前分到哪一步"的状态机（stage），把字节流依次交给三个专项分拣员——
//   RequestLineParser（拆请求行）、HeaderParser（拆头部）、BodyParser（拆正文）。
//   每个分拣员干完一步，总调度就切换到下一个状态，直到整个请求分拣完成。
//
// 关键技术点（初学者重点理解）：
//   1. 【状态机解析】把解析过程拆成 REQUEST_LINE→HEADERS→BODY→COMPLETE 等状态，按字节推进，
//      遇到分隔符（CRLF）切换状态。这样能优雅处理"数据没到齐"的情况，不完整就停在当前状态等下次。
//   2. 【增量解析】只消费已确认完整且合法的字节，未完成数据留在 Buffer。半包/粘包下不会丢数据。
//   3. 【三类大小上限】MAX_REQUEST_LINE_BYTES / MAX_HEADER_BYTES / MAX_BODY_BYTES 在分配大对象
//      前就拒绝异常输入，防止恶意请求撑爆内存，约束单连接资源占用。
//   4. 【子解析器栈上分配】三个子解析器是栈上成员对象，无需动态分配，解析器构造无堆开销。
// ==============================================================================
#pragma once
#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include "../http.h"
#include "ParserUtils.h"
#include "RequestLineParser.h"
#include "HeaderParser.h"
#include "BodyParser.h"

/**
 * @brief 解析状态机的各个阶段
 *
 * 通俗解释：就像快递分拣的工位顺序——先拆"快递面单"（请求行），再拆"内层包装"（头部），
 *   最后取出"货物"（正文）。每个工位干完去下一个，货物取完就到 COMPLETE 完成区。
 *
 * 【chunked 多工位 通俗解释】分块传输（chunked）的正文不是一次性到齐的，要反复"读长度→读数据"，
 *   所以多了 CHUNK_SIZE / CHUNK_DATA / CHUNK_TRAILERS 三个工位，像拆俄罗斯套娃一层层来。
 */
enum class ParseStage
{
    // 状态保存在连接私有解析器中，PARSE_NEED_MORE 返回后可从当前阶段继续；
    // 分块体额外经历"长度行 -> 数据 -> 尾部字段"，避免把不完整 TCP 数据误当完整请求。
    REQUEST_LINE,   // 解析请求行：GET /path HTTP/1.1
    HEADERS,        // 解析请求头部若干行，直到空行
    BODY,           // 解析固定长度正文（Content-Length）
    CHUNK_SIZE,     // chunked：读取当前块的十六进制长度行
    CHUNK_DATA,     // chunked：读取当前块的数据 + CRLF
    CHUNK_TRAILERS, // chunked：读取尾部字段，直到空行结束
    COMPLETE,       // 整个请求解析完成
    ERROR           // 解析出错
};
// 负责解析
// 增量 HTTP/1.x 请求解析器。它只消费已确认完整且合法的字节，未完成数据留在 Buffer，
// 因而能正确处理半包和粘包；三类大小上限则在分配大对象前拒绝异常输入，约束单连接资源占用。
//
// 解析逻辑被拆分为三个栈上子解析器：RequestLineParser、HeaderParser、BodyParser。
// HttpParser 保留 stage 状态机，负责协调三个子解析器并在它们之间传递 framing 状态
// （keepAlive、contentLength、chunked、headerBytes 等）。
/**
 * @brief HTTP/1.x 增量请求解析器——协调三个子解析器的"总调度"
 *
 * 通俗解释：HttpParser 自己不亲自拆字节，它像车间流水线调度员：盯着 stage 状态机，
 *   当前该拆请求行就把 Buffer 交给 RequestLineParser，该拆头部就交给 HeaderParser……
 *   子解析器干完汇报结果，调度员据此切换 stage，循环推进直到 COMPLETE。
 *
 * 【framing 状态传递 通俗解释】"framing"就是确定请求边界——靠 Content-Length 还是 chunked？
 *   keepAlive 是开是关？这些信息在请求行/头部解析时得到，要传给 BodyParser 决定怎么读正文。
 *   HttpParser 负责在子解析器之间搬运这些状态。
 */
class HttpParser
{
public:
    // 对外暴露的大小上限常量，供上层校验或日志使用
    static constexpr size_t MAX_REQUEST_LINE_BYTES = kMaxRequestLineBytes;
    static constexpr size_t MAX_HEADER_BYTES = kMaxHeaderBytes;
    static constexpr size_t MAX_BODY_BYTES = kMaxBodyBytes;

    /**
     * @brief 解析主循环：按 stage 状态机持续推进，直到完成/需更多数据/出错
     * @param buffer 连接读缓冲区
     * @param req 输出请求对象
     * @return PARSE_OK / PARSE_NEED_MORE / PARSE_ERROR
     */
    ParseState parse(Buffer &buffer, HttpRequest &req);

    /**
     * @brief 重置解析器，准备解析下一个请求
     * @note 只重置解析器自身状态，不清空 Buffer——其中可能已有 keep-alive 连接的下一个请求数据。
     */
    void reset();

    // ---- 以下 getter 转发到对应子解析器，供上层查询 framing 状态 ----
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
    ParseStage stage = ParseStage::REQUEST_LINE; // 当前所处状态机阶段

    bool resultDelivered_ = false; // 完整请求是否已交付一次，防止重复交付

    // 三个子解析器均为栈上成员对象，无需动态分配。
    RequestLineParser requestLineParser_;
    HeaderParser headerParser_;
    BodyParser bodyParser_;
};
#endif
