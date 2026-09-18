// ==============================================================================
// 文件名：HttpSession.h
// 所属模块：server/http/HttpSession —— HTTP 请求-响应轮次的"车间主任"
//
// 【职责比喻：流水线车间主任】
//   一个 HttpSession 对应一条 TCP 连接上的 HTTP 处理循环：读请求 → 解析 → 分发执行 →
//   发送响应 → （Keep-Alive 则继续下一轮，否则关连接）。它自持 Parser（解析器）+ Codec
//   （编解码）两件套，把整个 HTTP 处理流程串成一条协程。出站发送不在 Session 层抽象
//   sender，而是走协议无关的 OutboundTask + OutboundQueue + TransportWriter + writerLoop
//   体系（由 SubReactor 持有），Session 只负责把响应对象 enqueue 进出站队列。若业务接受
//   WebSocket 升级，发完 101 响应后把连接交接给 WebSocketSession。
//
// 关键技术点（初学者重点理解）：
//   1. 【协程驱动】run() 返回 Task<void>，是 C++20 协程。遇到"等数据/等可写"会 co_await 挂起，
//      让出线程给其他连接；事件就绪后被调度器恢复，从挂起点继续。这是高并发的关键。
//   2. 【Keep-Alive 循环】run() 内层 while(true) 处理一个请求；keepAlive_ 为真就继续下一轮，
//      一个 TCP 连接可处理多个请求，省去反复建连的开销。
//   3. 【状态机 SessionState】READING/PARSING/EXECUTING/WRITING/CLOSED 标记当前所处阶段，
//      便于调试和上层查询（如时间轮超时处理）。
//   4. 【协议栈下沉】Parser/Codec 从 SubReactor 共享下沉到每个 Session 内部——不同 Session 可能
//      跑不同协议（HTTP/WebSocket），编解码器必须隔离，不能共享同一套。出站发送统一走
//      OutboundTask + OutboundQueue 体系，不在 Session 抽象 sender。
//   5. 【Session 基类多态】继承 Session 后（2.0 的 Session 基类只有 run() 纯虚 +
//      onTimeout/onClose 两个默认钩子，没有 protocol()），Connection::session 可在 HTTP 和
//      WebSocket 间多态切换，升级时直接替换 session 指针即可交接。
//   6. 【唯一根协程】run() 是唯一拥有 co_await 的主流程；readRequest/startHandler/
//      queueResponse 都是同步推进函数。这样每条连接只有一棵可追踪的协程状态机，
//      避免嵌套 Task 的 continuation 与生命周期互相缠绕。
//   7. 【出站发送零 sender】queueResponse 把响应包成 OutboundTask 入 OutboundQueue，由
//      SubReactor 的 writerLoop + TransportWriter 实际搬运字节，协程通过
//      SendCompletionAwaiter 挂起等待发送完成通知。
// ==============================================================================
#pragma once
#ifndef HTTP_SESSION_H
#define HTTP_SESSION_H

#include "server/CoroutineScheduler/Task.h"
#include "server/http/HttpCodec/HttpCodec.h"
#include "server/http/HttpParser/HttpParser.h"
#include "server/http/RequestContext/RequestContext.h"
#include "server/session/Session/Session.h"
#include "server/transport/ConnectionKey.h"

#include <stop_token>

class SubReactor;
class Router;

/**
 * @brief 会话处理状态机阶段
 *
 * 通俗解释：像车间主任看板上的"当前工序"标签——正在读、正在解析、正在执行、正在写、已关闭。
 *   主要供调试和上层（如超时检测）查询会话进度。
 */
enum class SessionState
{
    READING,    // 正在读取/等待请求数据
    PARSING,    // 正在解析请求
    EXECUTING,  // 正在执行业务 handler
    WRITING,    // 正在发送响应
    CLOSED      // 会话已关闭
};

/**
 * @brief readRequest() 的返回结果
 *
 * 通俗解释：车间主任问"这次读请求结果如何"的五种可能：
 *   COMPLETE（齐了，去执行）、NEED_MORE（没读够，继续等）、CLOSED（连接没了）、
 *   ERROR（格式坏了）、TOO_LARGE（请求太大，超缓冲区上限）。
 */
enum class RequestReadResult
{
    COMPLETE,
    NEED_MORE,
    CLOSED,
    ERROR,
    TOO_LARGE,
    HTTP2
};

enum class HandlerStartResult
{
    SUBMITTED, // 已交给 Executor，根协程接下来等待 EXECUTE
    READY,     // Executor 拒绝，已经准备好 503 响应，可直接发送
    CLOSED     // 原连接已经不存在
};

struct Connection;

// HttpSession：HTTP/1.1 请求-响应轮次。协议栈自持 Parser + Codec；出站走 OutboundTask 体系。
// 若业务接受 WebSocket 升级，发送 101 后将 Connection::session 交接给 WebSocketSession。
/**
 * @brief HTTP 会话——一条连接上 HTTP 请求-响应循环的"车间主任"
 *
 * 通俗解释：车间主任手里攥着两件套（Parser 拆包、Codec 翻译分发）；发货走
 *   OutboundTask + OutboundQueue + TransportWriter，按"读→解析→执行→写"的工序循环跑。
 *   Keep-Alive 就继续下一轮，否则收工关连接。
 *
 * 【为何 codec_ 下沉到 Session 通俗解释】不同 Session 可能跑不同协议（HTTP 或 WebSocket），
 *   编解码器必须各自隔离——HTTP 连接用 HttpCodec，WebSocket 连接用 WebSocketCodec，不能混用。
 *   所以把 codec 下沉为每个 Session 的私有成员，实现多协议隔离。出站发送不在 Session 抽象 sender，
 *   统一走协议无关的 OutboundTask + OutboundQueue + TransportWriter + writerLoop 体系。
 *   协程挂起恢复时状态都在成员里，不会丢；连接私有也意味着无需加锁。
 *
 * @note 2.0 的 Session 基类规定 run() 协程入口及生命周期钩子；本类使用默认
 *       onTimeout/onClose，并 override requestHandlerStop() 点亮当前工作单的撤单灯。
 */
class HttpSession : public Session
{
private:
    SubReactor *reactor = nullptr;  // 所属 SubReactor（事件循环+调度器+出站队列）
    ConnectionKey key_{};           // fd + connId；跨挂起恢复时用于拒绝旧连接
    HttpParser parser_;             // 连接私有解析器（跨次 recv 保留状态）
    HttpCodec codec_;               // 编解码+分发
    RequestContext context_;        // 本次请求的"档案袋"（请求/响应/连接信息）
    bool keepAlive_ = true;         // 是否保持连接
    uint64_t sseClientId_ = 0;      // SSE 握手校验后的客户端标识
    SessionState state = SessionState::READING; // 当前会话状态
    std::stop_source handlerStopSource_; // 当前在途 handler 的协作式撤单源

public:
    /**
     * @brief 构造：绑定连接身份、所属 reactor 和路由表
     * @param key 连接身份（fd + connId）
     * @param r 所属 SubReactor
     * @param router 路由表（codec_ 初始化需要）
     */
    HttpSession(ConnectionKey key, SubReactor *r, Router &router);
    ~HttpSession() override;

    /**
     * @brief 通过 ConnectionKey 安全查找当前连接对象
     * @return 存在返回 Connection 指针；连接已销毁返回 nullptr
     * @note 协程挂起期间 fd 可能被关闭并复用，每次恢复都要用 fd+connId 重新查表。
     */
    Connection *getConn();

    /** @brief 发送完成后的收尾；true 继续 Keep-Alive，false 已关闭 */
    bool afterSend();

    /**
     * @brief 读取并尝试解析一个请求（两段式：先解析已有缓冲，不够再 recv）
     * @return 读取结果（COMPLETE/NEED_MORE/CLOSED/ERROR/TOO_LARGE）
     * @note 先尝试解析已有缓冲，不够再 recv，再解析。两段式提高半包命中效率。
     */
    RequestReadResult readRequest();

    /** @brief 返回当前会话状态，供调试和超时管理使用 */
    SessionState sessionState() const { return state; }

    /**
     * @brief 会话主协程：请求-响应循环
     * @return Task<void>，C++20 协程
     * @note 内层 while(true) 处理请求；Keep-Alive 则循环，否则 co_return 结束。
     *       override 自 Session 基类的纯虚 run()。
     */
    Task<void> run() override;

    /** 连接关闭或 Runtime 停机时只发撤单信号，不等待 Worker。 */
    void requestHandlerStop() noexcept override;

private:
    /** @brief 重置请求上下文与 keepAlive_，为下一轮请求清场 */
    void resetRequestContext();

    /** @brief 同步提交 handler；是否等待由唯一根协程 run() 决定 */
    HandlerStartResult startHandler();

    /** @brief 同步编码并入队响应；返回 ticket，0 表示失败 */
    uint64_t queueResponse();

    /** @brief 归还仍由 Session 持有、尚未移交给 OutboundTask 的响应 */
    void releasePendingResponse() noexcept;

    /**
     * @brief 检查业务是否请求了 WebSocket 升级，是则准备 101 响应
     * @return true 业务已接受升级且 101 响应就绪；false 未升级
     */
    bool handleWebSocketUpgradeIfRequested();

    /**
     * @brief 构造 WebSocket 升级握手响应（101）
     * @return true 101 响应已装好；false 不是升级请求或握手非法（已装 400 响应）
     * @note 校验 Upgrade 头与 Sec-WebSocket-Accept，失败回 400。
     */
    bool prepareWebSocketUpgrade();

    /**
     * @brief 创建 WebSocketSession 并交接 Connection::session
     * @return true 交接成功，本 HTTP 协程随后 co_return；false 交接失败
     * @note 通过 SessionFactory 创建 WebSocketSession，adopt 进调度器，旧 HTTP 协程退出。
     */
    bool handoffWebSocket();

    /** 准备 200 text/event-stream 首部；失败时准备普通 400 响应。 */
    bool prepareSseStream();
    /** 检查业务是否调用 acceptSse()。 */
    bool handleSseIfRequested();
    /** 首部写完后创建 SseSession 并替换 Connection::session。 */
    bool handoffSse();
    bool handoffHttp2();
};

#endif
