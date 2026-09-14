// ==============================================================================
// 文件名：WebSocketMessage.h
// 职责比喻：业务信封 —— 业务代码直接使用的 WebSocket 消息抽象（区别于协议层 WsFrame "运输箱"）
//
// 【整体比喻】
// 寄快递时有两个层次的抽象：
//   - "运输箱"（WsFrame）：物流公司关心的格式——面单（FIN/opcode）、是否上锁（mask）、
//     箱子尺寸（payload 长度）等。这是协议层概念，关心"怎么把字节安全送到对端"。
//   - "业务信封"（WebSocketMessage）：寄件人和收件人关心的内容——信件类型（type，
//     如"聊天"/"通知"）、正文（text）、收件人（toUserId）、发件人（fromUserId）等。
//     这是业务层概念，关心"这封信是干什么的、给谁看"。
//
// WebSocketMessage 就是业务层的信封：业务代码不需要懂帧头/掩码/分片这些 RFC 6455
// 细节，只跟 type/text/toUserId 这些业务字段打交道。协议层和业务层之间的翻译
// 由 WebSocketCodec 完成——它把"运输箱"（WsFrame）拆开取出"信封"（WebSocketMessage），
// 或反过来把"信封"装进新的"运输箱"发出。
//
// 【与 WsFrame 的区别（初学者最易混淆，重点理解）】
//   ┌─────────────────┬─────────────────────────────┬──────────────────────────────┐
//   │                 │ WsFrame（协议层）            │ WebSocketMessage（业务层）     │
//   ├─────────────────┼─────────────────────────────┼──────────────────────────────┤
//   │ 关心什么         │ FIN/opcode/mask/payload     │ type/id/replyTo/from/to/text │
//   │ 谁用             │ Parser/Codec                │ Dispatcher/handler/业务代码  │
//   │ RFC 6455 字段    │ 是（FIN/opcode/mask 直接对应）│ 否（业务自定义 type 字段）    │
//   │ payload 形态     │ 原始字节（已解掩码）          │ 业务字符串 text               │
//   │ 是否分片         │ 单帧（分片由 Session 重组）  │ 已是完整消息（无分片概念）    │
//   │ 生命周期         │ 解析时存在，翻译后丢弃        │ 贯穿整个业务处理流程          │
//   └─────────────────┴─────────────────────────────┴──────────────────────────────┘
//
// 【在系统中的角色】
// 本文件是 WebSocket 业务层消息的"信封格式定义"。WebSocketCodec 把收到的 WsFrame
// 翻译成 WebSocketMessage 放进 WsMessageContext.inbound；业务 handler 处理后填
// WsMessageContext.outbound（也是 WebSocketMessage），再由 Codec 翻译回 WsFrame 发出。
// 它把"协议层细节"和"业务层语义"彻底解耦——业务代码不依赖任何 RFC 6455 概念。
//
// 关键技术点（初学者重点理解）：
//   1. 【业务层 vs 协议层抽象】WsFrame 是"线上的字节布局"，WebSocketMessage 是"业务
//      想表达什么"。理解这种分层是理解整个 WebSocket 模块的关键——Parser/Codec
//      处理 WsFrame，Dispatcher/handler 处理 WebSocketMessage。
//   2. 【type 字段是业务路由 key】Dispatcher 按 message.type 路由——type 是业务侧
//      自定义的字符串（如 "chat"/"support"），与 RFC 6455 的 opcode 无关。
//   3. 【text 兼容二进制】虽然字段名叫 text，但本质是 std::string——可装 UTF-8 文本
//      也可装任意二进制。opcode=Binary 时 isBinary() 返回 true，业务据此决定怎么解释。
//   4. 【toUserId/fromUserId 支持私聊语义】业务消息显式携带收发双方 uid，handler
//      可通过 SessionManager::sendEncoded(toUserId, ...) 把消息转发给第三方——
//      这是"用户 A 给用户 B 发消息"链路的核心字段。
//   5. 【值类型 + 简单结构】WebSocketMessage 是普通 struct，inbound/outbound 在
//      WsMessageContext 中直接持有；其中 std::string 仍可能按内容大小申请堆内存。
//   6. 【应用层可靠语义】messageId/replyTo/ackRequested 让业务追踪“哪一封信被确认”；
//      TCP/WebSocket 的可靠传输本身并不代表接收方业务已经处理。
//   7. 【依赖倒置】本文件只依赖 WebSocketTypes 与独立的 UserId 类型定义，
//      不依赖 SessionManager、Parser 或 Codec。
// ==============================================================================
#pragma once
#ifndef WEBSOCKET_MESSAGE_H
#define WEBSOCKET_MESSAGE_H

#include "server/websocket/WebSocketTypes/WebSocketTypes.h"
#include "server/websocket/WebSocketTypes/UserId.h"

#include <string>
#include <cstdint>

/**
 * @brief 业务层 WebSocket 消息：业务代码直接使用的"信封"（区别于协议层 WsFrame）
 *
 * 【WebSocketMessage 通俗解释】
 * 业务信封——上面写"信件类型（type）/ 正文（text）/ 收件人（toUserId）/ 发件人
 * （fromUserId）"。业务代码只跟信封打交道，不必管运输箱（WsFrame）怎么打包、
 * 是否分片、是否掩码——那些由 Codec 在协议层处理。
 *
 * @note 区别于协议层 WsFrame（含 FIN/opcode/mask/payload 等线格式字段）。
 *       本结构体是业务层抽象，不含任何 RFC 6455 线格式细节。
 */
struct WebSocketMessage
{
    std::uint32_t version = 1;                // 应用信封版本；与 RFC 6455 帧版本无关
    std::string type;                        // 业务类型字符串（如 "chat"/"support"）——Dispatcher 按此字段路由
    std::string messageId;                   // 本消息 id：用于幂等与 ACK 关联，按字符串处理避免 JS 精度问题
    std::string replyTo;                     // 回复的是哪条消息；ACK 必须填写服务端消息 id
    std::string status;                      // delivery/ack_result 等回执的业务状态
    std::string text;                        // 消息正文：可装 UTF-8 文本或任意二进制（opcode=Binary 时）
    WsOpcode opcode = WsOpcode::Text;        // payload 类型标记：Text/Binary，业务据此决定怎么解释 text
    UserId toUserId = 0;                     // 收件人 uid（0 表示广播或不指定）——支持私聊/定向推送
    UserId fromUserId = 0;                   // 发件人 uid——便于接收方知道消息来自谁
    bool ackRequested = false;               // true 表示接收方应用处理后应回 type=ack/replyTo=id

    /**
     * @brief 返回 payload 引用（text 的别名）
     * @return text 的 const 引用
     * @note 提供这个名字是为了让业务代码用"payload"语义访问正文，更贴近协议术语。
     */
    const std::string &payload() const { return text; }

    /**
     * @brief 是否为二进制消息
     * @return true 表示 opcode==Binary，text 装的是二进制数据而非 UTF-8 文本
     */
    bool isBinary() const { return opcode == WsOpcode::Binary; }
};

#endif
