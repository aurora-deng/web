// ==============================================================================
// 文件名：HttpCodec.h
// 职责比喻：这是 HTTP 协议的"海关翻译官"。
//   Session 只管"接收字节 → 处理 → 发送字节"的流水线，但字节流和业务对象之间需要一个翻译：
//   - decode（解码）：把客户端来的字节流"翻译"成 HttpRequest 结构对象；
//   - dispatch（分发）：把翻译好的请求交给路由 Router，找到对应 handler 处理并准备好响应。
//   Codec 把"协议细节"和"业务路由"封装在边界内，Reactor 和 Session 都不用懂 HTTP 协议，
//   只管搬运字节即可。这样后续要换协议（如 HTTP/2）或换分发策略，只改 Codec 一处。
//
// 第四阶段为支持 WebSocket 做的改造：
//   - 不引入协议抽象基类——HttpCodec 与 WebSocketCodec 各自独立，由对应 Session 子类
//     直接持有；上层通过 Session 多态识别协议，不依赖 codec 多态。
//   - 构造函数保持 explicit HttpCodec(Router&)，由 HttpSession 在构造时内部自建（每连接独立实例）。
//
// 关键技术点（初学者重点理解）：
//   1. 【无继承的协议适配层】HttpCodec 不继承任何抽象基类，直接由 HttpSession 持有；
//      WebSocketCodec 同理独立。两种 Session 子类各自搭配自己的 Codec，上层通过
//      Session 多态区分协议，不依赖 Codec 多态。
//   2. 【Codec 与 Parser 分工】Codec 只做"驱动+分发"，真正逐字节解析的是 HttpParser。Codec
//      持有 Parser 引用，调用 parse() 并处理结果，保持协议层可独立测试复用。
//   3. 【Router 引用借用】Router 生命周期由 ServerRuntime 保证长于所有 Session，Codec 只借
//      引用不持有所有权，避免重复持有和析构顺序问题。
// ==============================================================================
#pragma once
#ifndef HTTP_CODEC_H
#define HTTP_CODEC_H
#include "server/http/http.h"
#include "server/http/HttpParser/HttpParser.h"
struct Connection;
class HttpResponse;
class Router;
class RequestContext;
// HttpCodec 是 Session 与 HTTP 语义之间的适配层：decode 驱动连接私有解析器，
// dispatch 则准备响应对象并进入路由。集中这条边界后，Reactor 无需了解路由和对象池细节，
// 后续增加协议版本或替换分发策略时也有明确扩展点。

/**
 * @brief HTTP 协议编解码器——"字节流 ↔ HttpRequest/HttpResponse"的双向转换层
 *
 * 通俗解释：就像国际贸易中的海关翻译官，一边对着"原始字节流单据"（Buffer），一边对着
 *   "标准化的请求档案"（HttpRequest）。decode 把单据翻译成档案，dispatch 把档案送去对应
 *   的业务窗口（Router）处理。
 *
 * 【为何 Codec 不持有 Connection 通俗解释】为了让协议层能脱离网络层独立测试，Codec 只依赖
 *   Buffer 和 Parser，不碰 Connection/SubReactor。这样单测时构造一个 Buffer 就能测解码，
 *   不必起真实连接。
 */
class HttpCodec
{
public:
    // 构造时绑定 Router，dispatch 时按 URL 路由到 handler。
    explicit HttpCodec(Router &router) : router(router) {}

    /**
     * @brief 解码：把 Buffer 中的字节流解析成 HttpRequest
     * @param buffer 连接的读缓冲区（可能含半包/粘包数据）
     * @param parser 连接私有的解析器（跨多次 recv 保留状态）
     * @param req 输出参数，解析结果填入
     * @param keepAlive 输出参数，解析得到的 keep-alive 标志
     * @return PARSE_OK / PARSE_NEED_MORE / PARSE_ERROR
     */
    ParseState decode(
        Buffer &buffer,
        HttpParser &parser,
        HttpRequest &req,
        bool &keepAlive);

    /**
     * @brief 分发：准备响应对象并交给路由处理
     * @param ctx 请求上下文（含请求、响应指针、连接信息等）
     * @return true 路由命中并处理；false 未命中
     * @note 若 ctx.response 为空，先从对象池借一个，再交给 router.handle。
     */
    bool dispatch(RequestContext &ctx);

    // 业务对象 → 协议字节：序列化响应头进 HeaderBody_（对称 WebSocketCodec::encode）。
    void encode(HttpResponse &resp);

private:
    // Router 的生命周期由 ServerRuntime 保证长于所有 Session；Codec 仅借用引用。
    Router &router;
};

#endif
