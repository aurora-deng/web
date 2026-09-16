// =============================================================================
// 文件名：Session.h
// 所属模块：server/session/Session —— 协议会话生命周期所有者基类
//
// 【职责比喻：电话局的"通话接线员"岗位牌】
//   每一通来电（TCP 连接）都会有一位接线员（Session）全程负责，从接通到挂断
//   管理这通通话的完整生命周期。客人说 HTTP 语，接线员就请 HTTP 翻译官
//   （HttpCodec）来翻译；如果客人中途改说 WebSocket 语（协议升级），接线员
//   就把翻译官换成 WebSocket 版（WebSocketCodec），自己继续服务。Session 基类
//   就是所有接线员的"岗位牌"——它规定接线员必须会做的事：接通通话（run()），
//   以及超时/关闭时的生命周期钩子。具体怎么招待由 HttpSession、WebSocketSession、
//   SseSession 实现。
//
// 关键技术点（初学者重点理解）：
//   1. 【会话生命周期所有者】Session 拥有一个连接从建立到关闭的完整生命周期，
//      是协程的载体——run() 是一个协程函数，内部用 co_await 挂起/恢复。
//   2. 【生命周期钩子】onTimeout / onClose 是模板方法钩子：Reactor 控流程，
//      子类可选 override 填细节（如 WS 心跳、SSE 心跳、manager 注销），默认实现保持兼容。
//   3. 【多态派发】子类自持具体的编解码器（HttpCodec/WebSocketCodec/SseCodec）。Reactor 持有
//      基类指针，无需 if/else 分支判断协议类型。出站发送走协议无关的
//      OutboundTask + TransportWriter + writerLoop 体系。
//   4. 【三种会话对称】HttpSession、WebSocketSession 与 SseSession 都继承本类，对上层呈现
//      同一接口；同一套 Reactor / Connection 代码可透明承载三种会话。
//   5. 【协议交接】HTTP 升级 WebSocket 时，HttpSession 把 Connection 的 session
//      指针替换为 WebSocketSession，旧协程 co_return 退出，新协程接管 fd。
// =============================================================================
#pragma once
#ifndef SESSION_H
#define SESSION_H

#include "server/CoroutineScheduler/CoroutineScheduler.h"
#include "server/CoroutineScheduler/Task.h"

/**
 * @brief 协议会话生命周期所有者基类
 *
 * 【通话接线员岗位牌 通俗解释】
 *   Session 是 Connection 上的协议生命周期所有者。HTTP/1.1、WebSocket 与 SSE 共享
 *   协程调度约定；Codec 由具体会话负责（HttpSession 持有 HttpCodec，WebSocketSession
 *   使用 WebSocketCodec，SseSession 使用 SseCodec）。SubReactor 持有 Session 基类指针。
 *   出站发送由协议无关的 OutboundTask + TransportWriter + writerLoop 体系承担，Session
 *   不抽象编解码器与发送器。
 *
 * 【协程作为会话主循环 通俗解释】
 *   run() 返回 Task<void>，是一个 C++20 协程。内部通常是 while(true) 循环：读请求 →
 *   处理 → 发响应 → 继续读下一个（keep-alive）。遇到 I/O 不就绪时 co_await 挂起，Reactor
 *   在事件就绪后恢复，全程异步非阻塞。
 *
 * 【生命周期钩子 通俗解释】
 *   onTimeout / onClose 是模板方法钩子：Reactor 控制超时/关闭的主流程，子类可选 override
 *   填细节（如 WebSocket 发 Ping、SSE 发注释心跳、向 manager 注销自己）。默认实现保持兼容——
 *   onTimeout 返回 true 表示同意关闭，onClose 空操作。
 *
 * @note 设计意图：让 Reactor 以统一接口持有任意协议的会话，新增协议时只需新增一个 Session
 *       子类（搭配对应的 Codec）即可接入；出站发送复用同一套 OutboundTask 体系。
 *       Session 基类只规定 run() 一个纯虚（协程入口）+ onTimeout/onClose 两个可选钩子，
 *       不在基类抽象协议名与发送器，保持接口最小。
 */
class Session
{
public:
    virtual ~Session() = default;  // 多态析构：基类指针销毁子类对象时必须虚析构，否则子类资源泄漏

    /**
     * @brief 会话主循环（协程入口）
     * @return Task<void> 协程任务对象，由调度器驱动
     * 内部用 co_await 挂起/恢复，实现异步非阻塞的请求-响应循环。
     */
    virtual Task<void> run() = 0;

    /**
     * @brief 超时钩子：默认同意关闭；返回 false 表示本轮跳过关闭（如 WS 还要发 Close/Ping）
     *
     * 【通俗解释】时间轮到点时 Reactor 问 Session"该关了吗"。默认返回 true 直接关；
     *   WebSocket 可 override 返回 false，先发 Close/Ping 帧再等下一轮。
     */
    virtual bool onTimeout() { return true; }

    /**
     * @brief 连接即将关闭前的清理钩子；默认空实现
     *
     * 【通俗解释】fd_close 真正动手前给 Session 一个收尾机会（如向 manager 注销、
     *   清理业务状态）。默认空操作，子类按需 override。
     */
    virtual void onClose() {}

    /**
     * @brief 请求正在执行的业务尽快协作退出；不得阻塞，也不会强杀 Worker
     *
     * SubReactor 在连接关闭和事件循环停机时调用。子类通常只需对当前 stop_source
     * 调 request_stop()，实际 handler 必须主动检查 Context::stopRequested()。
     */
    virtual void requestHandlerStop() noexcept {}
};

#endif
