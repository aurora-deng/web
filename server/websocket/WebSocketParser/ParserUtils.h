// ==============================================================================
// 文件名：ParserUtils.h
// 职责比喻：这是 WebSocket 帧解析器的"工具箱"——四个拆解员共用的"剪刀、尺子、标签纸"。
//   WebSocket 帧解析的四个子解析器（FrameHeaderParser/ExtendedLengthParser/MaskKeyParser/
//   PayloadParser）都会用到这里的通用定义：
//   - kWsMaxPayloadBytes：一把"尺子"，限定单帧 payload 最大 1 MiB，超长直接拒收；
//   - WsParseContext：一张"流转单"，子解析器之间传递的 framing 状态（FIN/opcode/掩码/长度等）；
//   - applyWsMask：一把"解密钥匙"，把客户端掩码过的 payload 还原成明文；
//   - isWsControlOpcode：一个"分类器"，判断 opcode 是否属于控制帧（Close/Ping/Pong）。
//
// 关键技术点（初学者重点理解）：
//   1. 【payload 上限的意义】没有上限的话，攻击者发一个声称 2^63 字节的帧就能撑爆内存。
//      kWsMaxPayloadBytes 在分配大对象前就拒绝，是防 DoS 的第一道闸门。
//   2. 【掩码异或解密】RFC 6455 要求客户端发来的每一帧都必须掩码（mask），掩码是 4 字节密钥，
//      payload 第 i 字节与 mask[i%4] 异或即得明文。applyWsMask 同时用于加解密（异或自反）。
//   3. 【控制帧识别】opcode >= 8（即高位为 1）的是控制帧（Close=8/Ping=9/Pong=10），
//      控制帧不能分片、payload 不能超过 125 字节，ExtendedLengthParser 要据此校验。
//   4. 【跨阶段上下文】WsParseContext 像"流转单"在四个子解析器之间传递，避免每个子解析器
//      各自维护一份状态，保证 framing 信息一致。
// ==============================================================================
#pragma once
#ifndef WS_PARSER_UTILS_H
#define WS_PARSER_UTILS_H

#include "server/websocket/WebSocketTypes/WebSocketTypes.h"
#include "server/websocket/WebSocketTypes/WebSocketLimits.h"

#include <cstddef>
#include <cstdint>
#include <string>

// 单帧 payload 上限：1 MiB。WebSocketParser::kMaxPayloadBytes 对外别名与此一致。
// 在分配大对象前就拒绝异常输入，约束单连接资源占用，防 DoS。
inline constexpr size_t kWsMaxPayloadBytes = kWsMaxFramePayloadBytes;

/**
 * @brief 子解析器之间传递的 framing 状态（对称 HttpParser 在子解析器间传递 keepAlive/contentLength 等）
 *
 * 通俗解释：就像快递分拣的"流转单"——前一个拆解员填好"是否最后一包（fin）、货物类型（opcode）、
 *   有没有上锁（masked）、这包多长（payloadLen）、解密钥匙（mask）"，传给下一个拆解员继续处理。
 *   所有子解析器共享同一张流转单，保证 framing 信息一致。
 */
struct WsParseContext
{
    bool fin = false;                          // 是否为最后一帧（FIN=1）；0 表示分片帧，后续还有
    WsOpcode opcode = WsOpcode::Text;          // 帧类型（Text/Binary/Close/Ping/Pong 等）
    bool masked = false;                       // 是否掩码；RFC 6455 强制客户端帧必须掩码
    // 字节1 低 7 位长度字段；126/127 时由 ExtendedLengthParser 展开为 payloadLen。
    // 0~125 直接当长度；126 表示后跟 2 字节真实长度；127 表示后跟 8 字节真实长度。
    uint8_t lenField = 0;
    uint64_t payloadLen = 0;                   // 真实 payload 字节数（由 lenField 展开得到）
    // 已消费的头部长度（基础头 + 扩展长度 + mask key），不含 payload。
    // 用于统计和调试，也可供上层做协议合规性检查。
    size_t headerLen = 0;
    uint8_t mask[4] = {};                      // 4 字节掩码密钥，PayloadParser 用它解密 payload
};

/**
 * @brief 对 payload 执行掩码异或（同时用于加解密，因为异或自反）
 * @param payload 待处理的数据（原地修改）
 * @param mask 4 字节掩码密钥
 *
 * 通俗解释：像"异或锁"——加密和解密是同一个动作。客户端用 mask 把明文搅成密文发出来，
 *   服务器用同一把 mask 再搅一次就还原成明文。第 i 字节和 mask[i%4] 配对，4 把钥匙轮流用。
 *
 * 【为什么必须掩码】RFC 6455 强制客户端→服务器的帧必须掩码，目的是防止中间代理缓存污染
 *   （混淆攻击）。服务器→客户端的帧不掩码。这是 WebSocket 与 HTTP 一个重要区别。
 */
inline void applyWsMask(std::string &payload, const uint8_t mask[4])
{
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
}

/**
 * @brief 判断 opcode 是否属于控制帧（Close/Ping/Pong）
 * @param opcode 帧类型
 * @return true 是控制帧；false 是数据帧（Text/Binary/continuation）
 *
 * 通俗解释：opcode 高位为 1（即 >= 8）的是控制帧。控制帧是"带外信号"——
 *   Close 表示要关门、Ping/Pong 是心跳探测。它们不能分片、payload 不超过 125 字节，
 *   ExtendedLengthParser 会据此做严格校验。
 */
inline bool isWsControlOpcode(WsOpcode opcode)
{
    return (static_cast<uint8_t>(opcode) & 0x08) != 0;
}

/** @brief 只接受 RFC 6455 当前定义的六种 opcode；其余值均为保留值。 */
inline bool isWsKnownOpcode(WsOpcode opcode)
{
    switch (opcode)
    {
    case WsOpcode::Continuation:
    case WsOpcode::Text:
    case WsOpcode::Binary:
    case WsOpcode::Close:
    case WsOpcode::Ping:
    case WsOpcode::Pong:
        return true;
    default:
        return false;
    }
}

#endif
