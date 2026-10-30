#include "backend/application/transport/learning/LearningRoutes/LearningRoutes.h"
#include "backend/server/Runtime/ServerRuntime.h"
#include "backend/server/http/RequestContext/RequestContext.h"
#include "backend/server/sse/SseEvent.h"
#include "backend/server/websocket/WebSocketCodec/WebSocketCodec.h"
#include "backend/server/websocket/WebSocketDelivery/WebSocketDeliveryService.h"
#include "backend/server/websocket/WebSocketDispatcher/WsMessageContext.h"
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
namespace webserver::learning {
namespace {
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
void registerLearningRoutes(ServerRuntime &server) {
    auto *deliveryService=&server.wsDelivery();
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
        return true; }, RouteOptions{ExecutionPolicy::ReactorSafe});

        // 可靠投递观测面：计数器只增不减，gauges 表示读取瞬间的当前水位。
        // 生产部署应通过鉴权或仅在管理网络暴露该路由。
        server.router().GET(
            "/delivery-metrics",
            [deliveryService](RequestContext &ctx) -> bool
            {
                // text() 会先设置默认的 text/plain；随后覆盖成 JSON，确保最终首部
                // 与实际响应体一致，避免调用顺序把显式 Content-Type 冲掉。
                ctx.response->text(
                    serializeDeliveryMetrics(deliveryService->metrics()));
                ctx.response->setHeader(
                    "Content-Type", "application/json; charset=utf-8");
                return true;
            });

        // SSE 订阅入口：HTTP handler 只表达“接受流”，首部写完后由 SseSession 接管连接。
        // 浏览器可使用 new EventSource('/events?uid=1001') 建立订阅。
        server.router().GET("/events", [](RequestContext &ctx) -> bool
                            {
        ctx.acceptSse();
        return true; });

        // SSE 发布入口：示例使用查询参数，便于直接用 curl 观察完整跨 Reactor 推送链路。
        // POST /events/1001?event=notice&id=42&data=hello
        auto *sseManager = &server.sseManager();
        server.router().POST(
            "/events/:uid",
            [sseManager](RequestContext &ctx) -> bool
            {
                SseClientId clientId = 0;
                try
                {
                    std::size_t parsed = 0;
                    const auto &raw = ctx.params.at("uid");
                    clientId = std::stoull(raw, &parsed);
                    if (parsed != raw.size())
                        clientId = 0;
                }
                catch (...)
                {
                    clientId = 0;
                }

                if (clientId == 0)
                {
                    ctx.response->status = 400;
                    ctx.response->statusText = "Bad Request";
                    ctx.response->json("{\"error\":\"uid must be a positive integer\"}");
                    return true;
                }

                SseEvent event;
                event.eventName = ctx.querry("event");
                if (event.eventName.empty())
                    event.eventName = "message";
                event.id = ctx.querry("id");
                event.data = ctx.querry("data");
                if (event.data.empty())
                    event.data = "hello from the SSE publisher";

                const auto accepted = sseManager->publish(clientId, event);
                if (accepted == 0)
                {
                    ctx.response->status = 404;
                    ctx.response->statusText = "Not Found";
                }
                ctx.response->json(
                    "{\"acceptedConnections\":" +
                    std::to_string(accepted) + "}");
                return true;
            });

        server.router().GET(
            "/events-status",
            [sseManager](RequestContext &ctx) -> bool
            {
                const auto metrics = sseManager->metrics();
                ctx.response->json(
                    "{\"onlineConnections\":" +
                    std::to_string(sseManager->onlineCount()) +
                    ",\"historyEvents\":" +
                    std::to_string(sseManager->historyEventCount()) +
                    ",\"registrations\":" +
                    std::to_string(metrics.registrations) +
                    ",\"rejectedRegistrations\":" +
                    std::to_string(metrics.rejectedRegistrations) +
                    ",\"publishedEvents\":" +
                    std::to_string(metrics.publishedEvents) +
                    ",\"acceptedDeliveries\":" +
                    std::to_string(metrics.acceptedDeliveries) +
                    ",\"replayedEvents\":" +
                    std::to_string(metrics.replayedEvents) +
                    ",\"replayGaps\":" +
                    std::to_string(metrics.replayGaps) + "}");
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


}
}
