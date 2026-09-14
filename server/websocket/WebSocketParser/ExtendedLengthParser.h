// ==============================================================================
// 文件名：ExtendedLengthParser.h
// 职责比喻：这是"扩展长度展开员"——当基础头标注的"包裹重量"不够表示时，展开真实长度。
//   基础头的 len7 只有 7 位，最大 125。如果 payload 更长，RFC 6455 规定：
//   - len7=126：后跟 2 字节真实长度（可表示 0~65535）；
//   - len7=127：后跟 8 字节真实长度（可表示 0~2^63-1）。
//   本解析员负责读出这 2 或 8 字节，检查最短编码并限制单帧 payload 上限。
//
// 关键技术点（初学者重点理解）：
//   1. 【网络字节序】WebSocket 规定多字节长度用大端序（网络字节序），所以要用移位拼出真实值，
//      不能直接 memcpy（小端机器上会得到反的）。
//   2. 【63 位上限】len7=127 时最高位必须为 0（RFC 6455 约定），否则判错。真实有效长度是 63 位。
//   3. 【最短编码】126 只能承载 126..65535；127 只能承载 65536 以上。一个长度不能有多种写法。
//   4. 【payload 上限】展开后的 payloadLen 不能超过 kWsMaxPayloadBytes（1 MiB），超长直接判错，
//      防止攻击者声称 2^63 字节撑爆内存。
//   控制帧的 FIN 和短长度约束在 FrameHeaderParser 中已经提前拒绝。
// ==============================================================================
#pragma once
#ifndef WS_EXTENDED_LENGTH_PARSER_H
#define WS_EXTENDED_LENGTH_PARSER_H

#include "ParserUtils.h"
#include "server/Buffer/Buffer.h"

/**
 * @brief 扩展长度解析器——展开 126/127 扩展长度字段（对称 HeaderParser 中的 framing 解析）
 *
 * 通俗解释：基础面单上"重量"栏只有 7 位，最大写 125。如果包裹更重，就标"126"或"127"，
 *   表示"真实重量写在后面的小纸条上"——126 后面跟 2 字节小纸条，127 后面跟 8 字节大纸条。
 *   本解析员负责把小纸条上的数字读出来，填进流转单。
 */
class ExtendedLengthParser
{
public:
    /**
     * @brief 解析扩展长度字段
     * @param buffer 连接读缓冲区
     * @param ctx 跨阶段上下文（读 lenField/opcode，写 payloadLen/headerLen）
     * @return WsDecodeResult::Ok / NeedMore / Error
     */
    WsDecodeResult parse(Buffer &buffer, WsParseContext &ctx)
    {
        // ---- 短形式（lenField < 126）：无需扩展 ----
        if (ctx.lenField < 126)
            return WsDecodeResult::Ok;

        // ---- 中等长度（lenField == 126）：读 2 字节真实长度 ----
        if (ctx.lenField == 126)
        {
            if (buffer.readableBytes() < 2)
                return WsDecodeResult::NeedMore;

            const auto *p = reinterpret_cast<const uint8_t *>(buffer.peek());
            // 大端序拼 2 字节：高位字节在前，左移 8 位再拼低位字节
            const uint64_t len = (static_cast<uint64_t>(p[0]) << 8) | p[1];
            // 126 扩展形式只能表示 126..65535；更小的值必须用短形式。
            if (len < 126 || len > kWsMaxPayloadBytes)
                return WsDecodeResult::Error;
            ctx.payloadLen = len;
            ctx.headerLen = 4;   // 基础头 2 字节 + 扩展 2 字节
            buffer.retrieve(2);
        }
        else // lenField == 127：读 8 字节真实长度
        {
            if (buffer.readableBytes() < 8)
                return WsDecodeResult::NeedMore;

            const auto *p = reinterpret_cast<const uint8_t *>(buffer.peek());
            // RFC 6455：8 字节长度的最高位必须为 0（有效位 63 位），否则判错
            if (p[0] & 0x80)
                return WsDecodeResult::Error;

            // 大端序拼 8 字节：逐字节左移 8 位再拼当前字节
            uint64_t len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | p[i];
            // 127 扩展形式只能表示 65536 以上；更小的值必须用 126 形式。
            if (len <= 0xFFFF || len > kWsMaxPayloadBytes)
                return WsDecodeResult::Error;
            ctx.payloadLen = len;
            ctx.headerLen = 10;  // 基础头 2 字节 + 扩展 8 字节
            buffer.retrieve(8);
        }

        return WsDecodeResult::Ok;
    }

    /** @brief 重置解析器状态（本解析器无状态，空实现） */
    void reset() {}
};

#endif
