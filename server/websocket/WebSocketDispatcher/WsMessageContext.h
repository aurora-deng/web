// ==============================================================================
// 文件名：WsMessageContext.h
// 职责比喻：话务员的"工作单" —— 承载处理一条 WebSocket 业务消息所需的所有上下文
//
// 【整体比喻】
// 话务员（WebSocketDispatcher）转接一通电话时手边要有一张工作单——上面写清：
//   - 来电内容是什么（inbound：收到的业务消息）
//   - 要怎么回复（outbound：要发回去的消息，hasOutbound 标记是否需要回）
//   - 这通电话要不要继续保持着（keepConnection）
//   - 来电的是谁、当前话务员是谁、值班经理是谁（uid/session/manager）
// 这张工作单就是 WsMessageContext。Dispatcher 把它传给分机 handler，handler 看单子
// 干活、填好回执（outbound），话务员再据回执决定怎么发回去。
//
// 【与 RequestContext 的对称设计】
//   - HTTP 侧有 RequestContext（含 HttpRequest/HttpResponse/params/query 等），
//     HTTP Router 的 handler 接收它处理请求并填好响应；
//   - WebSocket 侧有 WsMessageContext（含 inbound/outbound/uid/session/manager 等），
//     WebSocket Dispatcher 的 handler 接收它处理消息并填好回执。
// 两者都是"handler 的工作单"——把所有相关状态打包成一个参数传给 handler，
// 避免 handler 接收一大串零散参数。
//
// 【在系统中的角色】
// 本文件位于 WebSocket 业务层。当 WebSocketSession 收到一条完整应用消息后，会构造一个
// WsMessageContext，把 inbound 设为收到的消息、uid/session/manager 设为当前会话相关引用，
// 然后调 dispatcher_.dispatch(ctx)。handler 在执行过程中可能填 outbound 并把 hasOutbound
// 置 true，WebSocketSession 据此组帧发送。keepConnection=false 时表示 handler 要求关闭连接。
//
// 关键技术点（初学者重点理解）：
//   1. 【inbound/outbound 双消息】一次 dispatch 既有"输入"也有"输出"——handler 处理
//      inbound 后把回复填到 outbound，无需 handler 自己组帧或调用 sender。
//   2. 【hasOutbound 显式标记】即使 handler 填了 outbound 字段，也要把 hasOutbound
//      置 true 才会真正发送——避免"handler 没想回复却误填了 outbound 导致空回复"。
//   3. 【keepConnection 控制生命周期】handler 返回后，WebSocketSession 看
//      keepConnection 决定是否发 Close 帧关闭连接——业务层可借此实现"处理完就断"语义。
//   4. 【裸指针 session/manager】这两个指针不持有所有权——Session 由 Connection 的
//      shared_ptr 持有，Manager 是全局单例。WsMessageContext 只是 dispatch 期间的
//      临时借用，生命周期远短于 Session 和 Manager。
//   5. 【值类型语义】WsMessageContext 设计为值类型 struct，在栈上构造、按引用传给 handler，
//      dispatch 结束后自动析构——无需动态分配，性能开销小。
// ==============================================================================
#pragma once
#ifndef WS_MESSAGE_CONTEXT_H
#define WS_MESSAGE_CONTEXT_H

#include "server/websocket/WebSocketMessage/WebSocketMessage.h"

class WebSocketSession;
class WebSocketSessionManager;

/**
 * @brief WebSocket 业务分发上下文：handler 处理一条消息时的"工作单"（对称 RequestContext）
 *
 * 【WsMessageContext 通俗解释】
 * 话务员的工作单——上面记着"来电内容、回执内容、要不要保持通话、来电者是谁、
 * 当前话务员和值班经理是谁"。Dispatcher 把工作单交给分机 handler，handler 看单干活、
 * 填好回执，话务员再据回执发出回复。
 */
struct WsMessageContext
{
    WebSocketMessage inbound;                  // 输入消息：Codec 翻译出来的业务消息，handler 据此处理
    WebSocketMessage outbound;                 // 输出消息：handler 填好的回执，hasOutbound=true 时由 Session 组帧发出
    bool hasOutbound = false;                  // 是否有回执要发——显式标记避免误发空回复
    bool keepConnection = true;                // 是否保持连接——handler 设 false 时 Session 处理完会发 Close 帧

    UserId uid = 0;                            // 当前会话绑定的用户 id（从 session->userId() 取，方便 handler 直接用）
    WebSocketSession *session = nullptr;       // 源会话指针（不持有所有权）——handler 可借此调 session 的方法
    WebSocketSessionManager *manager = nullptr;// 全局 Manager 指针（不持有所有权）——handler 可借此向其他用户推送
};

#endif
