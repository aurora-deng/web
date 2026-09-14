// ==============================================================================
// 文件名：FrameHeaderParser.h
// 职责比喻：这是"基础帧头拆解员"——专拆 WebSocket 包裹最外层的"2 字节面单"（RFC 6455 基础帧头）。
//   WebSocket 帧最前面固定 2 字节，本拆解员把它拆成五个字段：
//   - FIN（1 位）：是不是最后一帧，1 表示完整消息，0 表示分片还有后续；
//   - RSV（3 位）：保留位，必须为 0，非 0 判错（防止扩展协议歧义）；
//   - opcode（4 位）：帧类型（Text=1/Binary=2/Close=8/Ping=9/Pong=10）；
//   - MASK（1 位）：是否掩码，客户端帧必须为 1，否则判错；
//   - len7（7 位）：payload 长度的"短形式"，0~125 直接当长度，126/127 表示要读扩展长度。
//   这是状态机的第一个工位，拆完才能进入扩展长度解析。
//
// 关键技术点（初学者重点理解）：
//   1. 【2 字节整体到齐】Buffer 里不足 2 字节就返回 NeedMore，不消费任何数据，等下次继续。
//   2. 【RSV 必须为 0】三位保留位若非 0，说明客户端用了未协商的扩展（如压缩），服务器不识别
//      就直接拒绝，避免误解析。
//   3. 【强制掩码校验】RFC 6455 规定客户端→服务器的帧必须掩码（MASK=1），不掩码的帧直接判错。
//      这是为了防止中间代理缓存污染攻击。
//   4. 【opcode 与控制帧】拒绝保留 opcode；控制帧必须 FIN=1 且只能使用 0~125 的短长度。
//   5. 【len7 三种情况】0~125 直接当长度；126 表示后跟 2 字节真实长度；127 表示后跟 8 字节
//      真实长度。后两种由 ExtendedLengthParser 展开。
//   6. 【位运算拆字段】WebSocket 是二进制协议，字段按位划分，用 & 和 >> 提取——和 HTTP
//      的文本协议（找空格、找冒号）完全不同，这是 WebSocket 与 HTTP 解析层的本质区别之一。
// ==============================================================================
#pragma once
#ifndef WS_FRAME_HEADER_PARSER_H
#define WS_FRAME_HEADER_PARSER_H

#include "ParserUtils.h"
#include "server/Buffer/Buffer.h"

/**
 * @brief 基础帧头解析器——拆解 RFC 6455 帧 2 字节基础头（对称 RequestLineParser）
 *
 * 通俗解释：快递包裹最外面那张"面单"只有 2 字节，但信息量很大：是不是最后一包（FIN）、
 *   货物类型（opcode）、有没有上锁（MASK）、这包大概多重（len7）。本拆解员按位把这五个
 *   字段拆出来填进流转单（WsParseContext），再把这 2 字节从传送带上消费掉。
 */
class FrameHeaderParser
{
public:
    /**
     * @brief 解析基础帧头
     * @param buffer 连接读缓冲区
     * @param ctx 跨阶段上下文（输出 fin/opcode/masked/lenField/payloadLen/headerLen）
     * @return WsDecodeResult::Ok（成功）/ NeedMore（数据不足）/ Error（格式错误）
     */
    WsDecodeResult parse(Buffer &buffer, WsParseContext &ctx)
    {
        // ---- 步骤 1：2 字节整体到齐才能拆，不足则等下次 ----
        if (buffer.readableBytes() < 2)
            return WsDecodeResult::NeedMore;

        const auto *p = reinterpret_cast<const uint8_t *>(buffer.peek());
        // ---- 步骤 2：按位拆出五个字段 ----
        const bool fin = (p[0] & 0x80) != 0;              // 最高位 FIN
        const uint8_t rsv = static_cast<uint8_t>((p[0] >> 4) & 0x07); // 接下来 3 位 RSV
        const auto opcode = static_cast<WsOpcode>(p[0] & 0x0F);       // 低 4 位 opcode
        const bool masked = (p[1] & 0x80) != 0;            // 第二字节最高位 MASK
        const uint8_t lenField = static_cast<uint8_t>(p[1] & 0x7F);   // 第二字节低 7 位长度

        // ---- 步骤 3：严格校验，畸形输入直接判错 ----
        // RSV 必须为 0：非 0 说明用了未协商的扩展（如 permessage-deflate），服务器不识别就拒绝
        if (rsv != 0)
            return WsDecodeResult::Error;
        // 保留 opcode 只有协商了扩展后才能使用；当前服务器没有协商任何扩展。
        if (!isWsKnownOpcode(opcode))
            return WsDecodeResult::Error;
        // 客户端帧必须掩码：RFC 6455 强制要求，防中间代理缓存污染攻击
        if (!masked)
            return WsDecodeResult::Error;
        // 控制帧不能分片，而且 payload 必须使用 0..125 的短长度形式。
        if (isWsControlOpcode(opcode) && (!fin || lenField >= 126))
            return WsDecodeResult::Error;

        // ---- 步骤 4：填进流转单，交给下一个拆解员 ----
        ctx.fin = fin;
        ctx.opcode = opcode;
        ctx.masked = true;
        ctx.lenField = lenField;
        ctx.payloadLen = lenField;   // 短形式直接当长度；126/127 时 ExtendedLengthParser 会覆盖
        ctx.headerLen = 2;

        // 消费掉这 2 字节，传送带往前走
        buffer.retrieve(2);
        return WsDecodeResult::Ok;
    }

    /** @brief 重置解析器状态（本解析器无状态，空实现） */
    void reset() {}
};

#endif
