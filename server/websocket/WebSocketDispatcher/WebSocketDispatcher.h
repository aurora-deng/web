// ==============================================================================
// 文件名：WebSocketDispatcher.h
// 职责比喻：总台话务员 —— 按"业务类型"分流 WebSocket 消息的快递分拣员
//
// 【整体比喻】
// 想象一家大公司的总台：外面打来的电话（WebSocket 业务消息）内容各异——有的是
// "转接到销售部"（type="chat"），有的是"转接到客服"（type="support"）。总台话务员
// （WebSocketDispatcher）面前有一本分机簿（handlers_：type → 处理函数），每来一通
// 电话看一眼类型代号，按代号转给对应分机；查不到代号的转给默认值班（defaultHandler_）。
//
// 【与 HTTP Router 的对称设计】
//   - HTTP Router  按 "URL path"        路由（GET /api/users → handler）；
//   - WebSocket Dispatcher 按 "message.type" 路由（type="chat" → handler）。
// 两者结构完全对称——都是"键 → handler 映射表 + 兜底默认处理"。区别仅在于：
//   - HTTP 的 key 是路径字符串，可含 :param 动态参数（Router.h 支持静态/动态两种）；
//   - WebSocket 的 key 是业务 type 字符串，纯字符串等值匹配（无动态参数需求）。
// 因此 Dispatcher 比 Router 简单得多：一张 unordered_map 足矣，不需要 vector + 分片匹配。
//
// 【在系统中的角色】
// 本文件位于 WebSocket 业务层，被 WebSocketSession 持有。当 WebSocketCodec 把一帧
// WsFrame 翻译成业务层的 WebSocketMessage 后，WebSocketSession 调
// dispatcher_.dispatch(ctx) 让 Dispatcher 按 ctx.inbound.type 找到对应 handler 执行。
// handler 处理完后可能生成 ctx.outbound，由 WebSocketSession 组帧发出。
//
// 关键技术点（初学者重点理解）：
//   1. 【与 HTTP Router 的对称性】同样是"键 → handler 映射 + 默认兜底"，只是 key 维度
//      不同（path vs message.type）。理解了 Router 就理解了 Dispatcher。
//   2. 【unordered_map O(1) 查找】type 是纯字符串，无动态参数，一张哈希表即可。
//      不需要 Router 那种"静态 map + 动态 vector"双数据结构。
//   3. 【defaultHandler 兜底】未注册的 type 走默认 handler——比 Router 直接返回 404
//      更灵活，适合 WebSocket 这种"未知消息也要给个回执"的场景。
//   4. 【handler 返回 bool】true 表示已处理（ctx.outbound 应被发送）；
//      false 表示未处理（WebSocketSession 据此决定是否 echo 回显）。
//   5. 【无状态】Dispatcher 本身不存会话状态——所有上下文通过 WsMessageContext
//      传参带入。这让 Dispatcher 可以被多个 WebSocketSession 共享，无需每连接一份。
// ==============================================================================
#pragma once
#ifndef WEBSOCKET_DISPATCHER_H
#define WEBSOCKET_DISPATCHER_H

#include "server/websocket/WebSocketDispatcher/WsMessageContext.h"

#include <functional>
#include <string>
#include <unordered_map>

// 业务处理器类型：接收 WsMessageContext，返回是否已处理（true 表示已生成 outbound 应发送）
// 【WsHandler 通俗解释】话务员转过去的"分机"，每个分机负责一类业务（chat/support/...），
//   接到电话后看情况填好"回执单"（ctx.outbound），返回 true 表示已经处理过。
using WsHandler = std::function<bool(WsMessageContext &)>;

/**
 * @brief WebSocket 业务消息分发器：按 message.type 路由到 handler（对称 HTTP Router）
 *
 * 【WebSocketDispatcher 通俗解释】
 * 总台话务员——每来一条业务消息（WebSocketMessage），看一眼它的 type 字段，
 * 翻 handlers_ 这本分机簿找对应分机（handler）；找不到就转给默认值班（defaultHandler_）。
 *
 * @note 按 message.type 路由（非 HTTP path Router）。无状态，可被多 Session 共享。
 */
class WebSocketDispatcher
{
public:
    /**
     * @brief 注册某 type 的处理器
     * @param type 业务类型字符串（如 "chat"/"support"/"heartbeat"）
     * @param handler 处理回调
     * @note 同一 type 重复注册会覆盖旧 handler（handlers_[type] = ...）
     */
    void on(const std::string &type, WsHandler handler);

    /**
     * @brief 注册默认处理器：未命中任何 type 时走这里
     * @param handler 默认回调
     * @note 类似 HTTP Router 的 404，但更灵活——可返回 true 表示已兜底处理。
     */
    void onDefault(WsHandler handler);

    /**
     * @brief 派发一条业务消息到对应 handler
     * @param ctx 消息上下文（含 inbound 输入消息、outbound 输出消息等）
     * @return true 命中 type 或 default 且 handler 返回 true；false 均未注册或均返回 false
     *
     * 通俗解释：话务员拿到一通电话——先按代号查分机簿（handlers_），找到就转过去；
     *   查不到就转给默认值班（defaultHandler_）；连值班都没设就让电话响着（返回 false）。
     */
    bool dispatch(WsMessageContext &ctx);

private:
    std::unordered_map<std::string, WsHandler> handlers_;  // type → handler 映射表（分机簿）
    WsHandler defaultHandler_;                             // 默认兜底 handler（值班分机）
};

#endif
