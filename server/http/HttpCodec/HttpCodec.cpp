// ==============================================================================
// 文件名：HttpCodec.cpp
// 职责比喻：这是"海关翻译官"的日常工作记录本。
//   对应 HttpCodec.h 中声明的两个核心动作——decode（翻译字节流）和 dispatch（送去路由窗口）。
//   本文件实现了这两个动作的具体步骤，是协议层与业务层之间的"接驳通道"。
//
// 关键技术点（初学者重点理解）：
//   1. 【Parser 跨次保留状态】parser 属于 HttpSession，非阻塞读取下可能一次只来半个请求，
//      parser 必须把已解析的状态记下来，下次 recv 到新数据接着推进。NEED_MORE 时绝不能 reset。
//   2. 【何时 reset Parser】只有 PARSE_OK（完整请求）才 reset，准备解析下一个请求；
//      ERROR 时由 Session 关连接，无需复用 Parser。
//   3. 【响应对象池】dispatch 时若没有现成 response，从对象池借一个，避免每次请求都 new/delete。
// ==============================================================================
#include "HttpCodec.h"
#include "server/SubReactor/SubReactor.h"

/**
 * @brief 解码：驱动 Parser 解析 Buffer，输出 HttpRequest 和 keepAlive 标志
 * @param buffer 连接读缓冲区
 * @param parser 连接私有解析器
 * @param req 输出请求对象
 * @param keepAlive 输出 keep-alive 标志
 * @return 解析状态
 *
 * 通俗解释：翻译官拿到一沓"原始单据"（Buffer），交给翻译助手（Parser）逐字翻译成"标准档案"
 *   （HttpRequest）。如果单据没到齐，助手会说"等下一批"（NEED_MORE），翻译官就原样退还，等下次。
 *
 * 【为何 NEED_MORE 不 reset 通俗解释】Parser 内部记着"请求行已解析完，正在等头部"这类进度。
 *   如果半路 reset，进度丢失，下次新数据来了又从头开始，前面的数据就丢了。只有完整解析成功
 *   （PARSE_OK）才 reset，准备迎接下一个请求。
 */
ParseState HttpCodec::decode(
    Buffer &buffer,
    HttpParser &parser,
    HttpRequest &req,
    bool &keepAlive)
{
    // parser 属于 HttpSession，可跨多次非阻塞读取保留状态。Codec 不再依赖 Connection/SubReactor，
    // 因而协议层可以独立测试和复用。只有完整请求才同步 keep-alive 并 reset；
    // NEED_MORE 时重置会丢失半包进度，ERROR 时则由 Session 关闭连接，无需尝试复用解析器。
    auto state = parser.parse(buffer, req);

    if (state != PARSE_OK)
        return state;

    // 完整请求到手：同步 keepAlive 标志给 Session，并 reset Parser 准备下一个请求
    keepAlive = parser.keepAlive();
    parser.reset();

    return PARSE_OK;
}

/**
 * @brief 分发：准备响应对象并交给路由处理
 * @param ctx 请求上下文（含 request、response 指针、fd、session 等）
 * @return true 路由命中并处理；false 未命中
 *
 * 通俗解释：翻译官把整理好的"档案袋"（RequestContext）送去对应的业务窗口（Router）。
 *   出发前先确认档案袋里有没有空白的"回执单"（response），没有就从对象池借一张带上。
 */
bool HttpCodec::dispatch(RequestContext &ctx)
{
    // 业务 handler 可能没主动创建 response，这里兜底从对象池借一个，保证后续发送有对象可用
    if(ctx.response==nullptr)
    {
        ctx.response=responsePool.acquire();
    }
    return router.handle(ctx);
}

void HttpCodec::encode(HttpResponse &resp)
{
    resp.buildHeader();
}
