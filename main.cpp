// ============================================================
// 文件名：main.cpp
// ------------------------------------------------------------
// 【职责比喻：工厂大门 + 前台】
//   整个服务器就像一座工厂：本文件既是"大门"也是"前台"——开门投产前，
//   前台要先把各项业务登记到"服务手册"（路由表 Router）上：访客点 "/"
//   就上"hello"这道菜，点 "/user/:id" 就报出访客编号，点 "/ws" 就把访客
//   领进 WebSocket 包间。菜单写好后，前台按下"开工"按钮（server.start()），
//   车间（SubReactor 线程）、工人（协程）、搬运工（Executor 线程池）就开始
//   各司其职运转起来。如果开门失败（比如端口被占），前台要体面地关门并
//   告诉原因，而不是当场崩溃（避免 core dump）。
//
// 关键技术点（初学者重点理解）：
//   1. 中间件链（middleware）：像安检传送带，请求依次经过"日志→鉴权"再到达
//      业务 handler；任意一环调用 next() 放行，不调用则短路返回。
//   2. 动态路由参数："/user/:id" 中的 :id 是占位符，框架自动把 URL 里的实际
//      值填进 ctx.params，handler 用 ctx.params.at("id") 取出。
//   3. Executor 异步执行：/slow 的耗时循环不会卡死整个服务器，因为 handler
//      被丢到 HTTP Worker 池执行；循环还会轮询撤单信号，避免超时后继续白干。
//   4. WebSocket 升级握手：/ws 是 WebSocket 握手入口。客户端发起 HTTP Upgrade
//      请求到 /ws，业务层调用 ctx.acceptWebSocket() 标记此连接要升级为
//      WebSocket；真正的握手应答（101 Switching Protocols + Sec-WebSocket-Accept）
//      和后续帧收发由框架的 WebSocketSession 接管，体现协议边界的清晰划分。
// ============================================================
#include "server/Runtime/ServerRuntime.h"
#include "server/http/RequestContext/RequestContext.h"
#include "server/websocket/WebSocketCodec/WebSocketCodec.h"
#include "server/websocket/WebSocketDelivery/WebSocketDeliveryService.h"
#include "server/websocket/WebSocketDispatcher/WsMessageContext.h"
#include "log/logger/logger.h"
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace
{
std::string serializeDeliveryMetrics(
    const WebSocketDeliveryMetricsSnapshot &metrics)
{
    std::ostringstream out;
    out << "{\"counters\":{"
        << "\"submissions\":" << metrics.submissions << ','
        << "\"newMessages\":" << metrics.newMessages << ','
        << "\"duplicateSubmissions\":" << metrics.duplicateSubmissions << ','
        << "\"rejectedSubmissions\":" << metrics.rejectedSubmissions << ','
        << "\"attemptsDispatched\":" << metrics.attemptsDispatched << ','
        << "\"retriesDispatched\":" << metrics.retriesDispatched << ','
        << "\"writtenAttempts\":" << metrics.writtenAttempts << ','
        << "\"retryableAttemptFailures\":"
        << metrics.retryableAttemptFailures << ','
        << "\"permanentAttemptFailures\":"
        << metrics.permanentAttemptFailures << ','
        << "\"retrySchedules\":" << metrics.retrySchedules << ','
        << "\"transportTimeouts\":" << metrics.transportTimeouts << ','
        << "\"ackTimeouts\":" << metrics.ackTimeouts << ','
        << "\"ackRequests\":" << metrics.ackRequests << ','
        << "\"acknowledgedMessages\":" << metrics.acknowledgedMessages << ','
        << "\"duplicateAcks\":" << metrics.duplicateAcks << ','
        << "\"rejectedAcks\":" << metrics.rejectedAcks << ','
        << "\"failedMessages\":" << metrics.failedMessages << ','
        << "\"ignoredAttemptResults\":" << metrics.ignoredAttemptResults
        << "},\"gauges\":{"
        << "\"pendingMessages\":" << metrics.pendingMessages << ','
        << "\"observedReceipts\":" << metrics.observedReceipts
        << "}}";
    return out.str();
}
}

/**
 * @brief 服务器主入口：装配路由与中间件，启动事件循环
 *
 * 【工厂开门投产】
 *   1. 先创建一个 ServerRuntime（相当于工厂整体运营环境，内含 Reactor、
 *      线程池、定时器等基础设施）。
 *   2. 在 router() 上挂中间件和路由（相当于登记业务菜单和安检流程）。
 *   3. 调用 server.start() 正式开工，主线程进入事件循环阻塞等待。
 *   4. 任何启动异常（如端口占用）用 try/catch 兜底，返回非零退出码。
 *
 * @return 0 正常退出；1 启动失败（如端口被占用）
 */
int main()
{
    try
    {
        ServerRuntime server;
        // 可重复的并发测试需要固定 Reactor 数；生产环境不设置时仍按 CPU 自动选择。
        if (const char *raw = std::getenv("WEB_SERVER_REACTORS"))
        {
            char *end = nullptr;
            errno = 0;
            const unsigned long count = std::strtoul(raw, &end, 10);
            if (errno != 0 || end == raw || *end != '\0' ||
                count == 0 || count > 32)
                throw std::invalid_argument(
                    "WEB_SERVER_REACTORS must be an integer in [1, 32]");
            server.setReactorCount(static_cast<size_t>(count));
        }
        // Runtime 持有可靠投递服务：状态机、最终写回执与可停止的周期重试线程共用同一生命周期。
        auto *deliveryService = &server.wsDelivery();

        // ===== 中间件层：请求到达 handler 前的"安检传送带" =====

        // 请求日志中间件：像工厂门口的签到台，记录每位访客点了什么菜
        // LOG_HTTP 当前为空操作（压测纯净版），需要日志时改回真正调用
        server.router().use([](RequestContext &ctx, auto next)
                            {
        (void)ctx; // 压测模式下 LOG_HTTP 是空宏，仍显式标记参数已使用。
        LOG_HTTP(ctx.request.method + " " + ctx.request.path);
        next(); });

        // 鉴权中间件：像保安检查 VIP 通行证
        // 访问 /admin 必须带 token 头，否则直接 401 拒绝（短路，不调 next）
        server.router().use([](RequestContext &ctx, auto next)
                            {
        if (ctx.request.path == "/admin")
        {
            auto it = ctx.request.headers.find("token");
            if (it == ctx.request.headers.end())
            {
                ctx.response->status = 401;
                ctx.response->text("Unauthorized");
                return;  // 不调用 next()，后续 handler 不执行（中间件短路）
            }
        }
        next(); });

        // ===== 业务路由层：每条路由就是菜单上的一道菜 =====

        // 根路径：返回一段简单 HTML，演示最基本的文本响应
        server.router().GET("/", [](RequestContext &ctx) -> bool
                            {
        ctx.response->html("<h1>hello</h1>");
        return true; });

        // 动态路由：:id 是占位符，框架自动提取 URL 实际值到 ctx.params
        server.router().GET("/user/:id", [](RequestContext &ctx) -> bool
                            {
        ctx.response->text(ctx.params.at("id"));
        return true; });

        // 分块传输（chunked）示例：把响应拆成多块发送，适合动态生成内容
        server.router().GET("/stream1", [](RequestContext &ctx) -> bool
                            {
        ctx.response->beginChunked();
        ctx.response->writeChunk("hello");
        ctx.response->writeChunk(" world");
        ctx.response->endChunked();
        return true; });

        // /stream2 的 sleep(1) 在 Executor Worker 线程执行，不会阻塞 Reactor
        // 这验证了"慢业务丢给线程池、主循环保持敏捷"的异步设计
        server.router().GET("/stream2", [](RequestContext &ctx) -> bool
                            {
        ctx.response->beginChunked();
        for (int i = 0; i < 10; i++)
        {
            if (ctx.stopRequested())
                break;
            ctx.response->writeChunk("hello\n");
            sleep(1);
        }
        ctx.response->endChunked();
        return true; });

        // 静态文件服务：用 sendfile 零拷贝发送 SVG 图片
        // 支持 Range 请求（断点续传），失败时返回 404
        server.router().GET("/logo", [](RequestContext &ctx) -> bool
                            {
        const std::string path = "./static/logo.svg";
        ctx.response->setHeader("Content-Type", "image/svg+xml");
        if (!ctx.response->sendfile(path, ctx.request, ctx.request.range))
        {
            ctx.response->status = 404;
            ctx.response->statusText = "Not Found";
            ctx.response->text("Not Found");
        }
        return true; });

        // /slow: 验证 Executor 异步执行与协作式超时。
        // 像后厨每做一小步就看一次撤单灯：超过 handler deadline 后尽快停工，
        // HttpSession 再把本次结果统一改写成 504；Reactor 仍可同时服务 /fast。
        server.router().GET("/slow", [](RequestContext &ctx) -> bool
                            {
        const auto finishAt = std::chrono::steady_clock::now() +
                              std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < finishAt &&
               !ctx.stopRequested())
        {
            usleep(10 * 1000);
        }
        ctx.response->text("slow done");
        return true; });

        // /fast: 应在 /slow 期间立即返回
        // 两者并行验证了"慢请求不拖累快请求"的并发能力
        server.router().GET("/fast", [](RequestContext &ctx) -> bool
                            {
        ctx.response->text("fast");
        return true; });

        // 可靠投递观测面：计数器只增不减，gauges 表示读取瞬间的当前水位。
        // 生产部署应通过鉴权或仅在管理网络暴露该路由。
        server.router().GET(
            "/delivery-metrics",
            [deliveryService](RequestContext &ctx) -> bool
            {
                ctx.response->setHeader(
                    "Content-Type", "application/json; charset=utf-8");
                ctx.response->text(
                    serializeDeliveryMetrics(deliveryService->metrics()));
                return true;
            });

        // /ws: WebSocket 握手入口（第四阶段新增）。
        // 【握手流程】客户端先发一个 HTTP Upgrade 请求到 /ws（带 Upgrade: websocket、
        //   Connection: Upgrade、Sec-WebSocket-Key、Sec-WebSocket-Version: 13 等头），
        //   框架解析出 HttpRequest 后路由到此 handler。业务层只需调用
        //   ctx.acceptWebSocket() 设置升级标志——框架在 handler 返回后会据此
        //   完成 101 Switching Protocols 应答（含 Sec-WebSocket-Accept 计算），
        //   随后该连接的后续字节流交给 WebSocketSession 接管，不再走 HTTP 解析。
        //   （2.0：WebSocketSession 由 SubReactor 持有的 SessionFactory 创建——依赖倒置，
        //    session/transport 层不直接依赖 websocket 模块；出站走 OutboundQueue。）
        // 【为什么这样写】/ws 只负责"点头同意升级"，把协议切换的细节交给框架，
        //   业务代码保持极简。真正的 WebSocket 消息收发在下方的 wsDispatcher 注册。
        // 【消息格式约定】连接 URL 带 ?uid=1001 注册到 SessionManager；
        //   消息体：JSON {"type":"chat","to":1002,"content":"hello"} 或简写 @1002:hello
        server.router().GET("/ws", [](RequestContext &ctx) -> bool
                            {
        ctx.acceptWebSocket();
        return true; });

        // WebSocket 业务分发层（第四阶段新增）：握手成功后，连接进入 WebSocket 帧模式，
        // 每条文本帧被解析成 WsMessageContext 并按 message.type 分发——这与 HTTP 的
        // path 路由对称，只不过这里路由的是"消息类型"而非"URL 路径"。
        // "chat" 类型：点对点聊天，把消息转发给 toUserId 指定的目标用户。
        server.wsDispatcher().on("chat", [deliveryService](WsMessageContext &ctx) -> bool
                                 {
        if (!ctx.manager || ctx.inbound.toUserId == 0 ||
            ctx.inbound.version != 1)
        {
            ctx.outbound.type = "delivery";
            ctx.outbound.opcode = WsOpcode::Text;
            if (ctx.inbound.messageId.empty())
                ctx.outbound.text = "invalid outbound message";
            else
            {
                WebSocketMessage response;
                response.type = "delivery";
                response.replyTo = ctx.inbound.messageId;
                response.toUserId = ctx.uid;
                response.status = ctx.inbound.version == 1
                                      ? "invalid_request"
                                      : "unsupported_version";
                ctx.outbound.text =
                    WebSocketCodec::serializeApplicationMessage(response);
            }
            ctx.hasOutbound = true;
            return true;
        }

        // 没有客户端 message id 的 @uid:text 走兼容的 best-effort 路径。
        if (ctx.inbound.messageId.empty())
        {
            const auto delivery = ctx.manager->sendText(
                ctx.inbound.toUserId, ctx.inbound.text);
            ctx.outbound.type = "delivery";
            ctx.outbound.opcode = WsOpcode::Text;
            switch (delivery)
            {
            case EnqueueResult::Ok:
                ctx.outbound.text = "accepted";
                break;
            case EnqueueResult::Backpressure:
                ctx.outbound.text = "target busy";
                break;
            case EnqueueResult::Closed:
                ctx.outbound.text = "target unavailable";
                break;
            case EnqueueResult::Invalid:
                ctx.outbound.text = "invalid outbound message";
                break;
            }
            ctx.hasOutbound = true;
            return true;
        }

        const auto submitted = deliveryService->submit(
            ctx.uid,
            ctx.inbound.toUserId,
            ctx.inbound.messageId,
            ctx.inbound.text);

        std::string status;
        switch (submitted.status)
        {
        case DeliverySubmissionStatus::Accepted: status = "accepted"; break;
        case DeliverySubmissionStatus::RetryScheduled: status = "retry_scheduled"; break;
        case DeliverySubmissionStatus::DuplicatePending: status = "duplicate_pending"; break;
        case DeliverySubmissionStatus::Acknowledged: status = "acknowledged"; break;
        case DeliverySubmissionStatus::Failed: status = "failed"; break;
        case DeliverySubmissionStatus::Conflict: status = "id_conflict"; break;
        case DeliverySubmissionStatus::Capacity: status = "delivery_window_full"; break;
        case DeliverySubmissionStatus::Unavailable: status = "service_unavailable"; break;
        case DeliverySubmissionStatus::Invalid: status = "invalid_request"; break;
        }

        WebSocketMessage response;
        response.type = "delivery";
        response.messageId = submitted.serverMessageId;
        response.replyTo = ctx.inbound.messageId;
        response.toUserId = ctx.uid;
        response.status = std::move(status);
        ctx.outbound.type = "delivery";
        ctx.outbound.opcode = WsOpcode::Text;
        ctx.outbound.text =
            WebSocketCodec::serializeApplicationMessage(response);
        ctx.hasOutbound = true;
        return true; });

        // ACK 必须引用服务端生成的 message id；Tracker 校验 ACK 是否来自真实收件人。
        server.wsDispatcher().on("ack", [deliveryService](WsMessageContext &ctx) -> bool
                                 {
        if (ctx.inbound.version != 1)
        {
            WebSocketMessage response;
            response.type = "ack_result";
            response.replyTo = ctx.inbound.replyTo;
            response.toUserId = ctx.uid;
            response.status = "unsupported_version";
            ctx.outbound.type = "ack_result";
            ctx.outbound.opcode = WsOpcode::Text;
            ctx.outbound.text =
                WebSocketCodec::serializeApplicationMessage(response);
            ctx.hasOutbound = true;
            return true;
        }
        const auto ack = deliveryService->acknowledge(
            ctx.uid, ctx.inbound.replyTo);
        std::string status;
        switch (ack.status)
        {
        case DeliveryAckStatus::Acknowledged:
            status = "accepted";
            break;
        case DeliveryAckStatus::Duplicate: status = "duplicate"; break;
        case DeliveryAckStatus::Unknown: status = "unknown_message"; break;
        case DeliveryAckStatus::WrongRecipient: status = "wrong_recipient"; break;
        case DeliveryAckStatus::TooLate: status = "too_late"; break;
        case DeliveryAckStatus::Invalid: status = "invalid_ack"; break;
        }

        WebSocketMessage response;
        response.type = "ack_result";
        response.replyTo = ctx.inbound.replyTo;
        response.toUserId = ctx.uid;
        response.status = std::move(status);
        ctx.outbound.type = "ack_result";
        ctx.outbound.opcode = WsOpcode::Text;
        ctx.outbound.text =
            WebSocketCodec::serializeApplicationMessage(response);
        ctx.hasOutbound = true;
        return true; });

        // 架构探针：配合黑盒测试证明慢 WS handler 在 Worker 执行，而非占住 Reactor。
        server.wsDispatcher().on("slow_probe", [](WsMessageContext &ctx) -> bool
                                 {
        sleep(2);
        ctx.outbound.type = "probe_result";
        ctx.outbound.text = "slow done";
        ctx.hasOutbound = true;
        return true; });
        server.wsDispatcher().on("fast_probe", [](WsMessageContext &ctx) -> bool
                                 {
        ctx.outbound.type = "probe_result";
        ctx.outbound.text = "fast done";
        ctx.hasOutbound = true;
        return true; });
        server.wsDispatcher().on("throw_probe", [](WsMessageContext &) -> bool
                                 {
        throw std::runtime_error("intentional WebSocket handler failure"); });
        server.wsDispatcher().on(
            "cooperative_timeout_probe", [](WsMessageContext &ctx) -> bool
            {
                // C++ 不能安全强杀 Worker；耗时业务必须在可中断边界主动看撤单灯。
                while (!ctx.stopRequested())
                    usleep(10 * 1000);
                return true;
            });

        // onDefault：未匹配任何 type 的消息走这里——原样回显为 "echo" 类型，
        // 保证客户端总能收到响应，不会因消息类型未注册而静默丢弃。
        server.wsDispatcher().onDefault([](WsMessageContext &ctx) -> bool
                                        {
        ctx.outbound = ctx.inbound;
        ctx.outbound.type = "echo";
        ctx.hasOutbound = true;
        return true; });

        // ===== 正式开工：进入事件循环，阻塞直到收到关闭信号 =====
        server.start();
        return 0;
    }
    catch (const std::exception &ex)
    {
        // bind Address already in use 等：正常退出码，避免 uncaught → terminate → 核心转储。
        // 【为什么不让程序崩溃】未捕获异常会导致 std::terminate → abort → 生成 core dump，
        //   既不优雅也难排查；这里捕获后打印友好提示，返回 1 让运维知道是启动失败。
        std::cerr << "webserver failed to start: " << ex.what() << "\n"
                  << "Hint: Ctrl+Z only suspends the process and keeps port 8080; "
                     "use Ctrl+C / kill -TERM, \nor: fg then Ctrl+C, \n"
                     "or kill $(pgrep -n webserver)\n";
        return 1;
    }
}
