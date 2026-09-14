// ==============================================================================
// 文件名：PayloadParser.h
// 职责比喻：这是"货物取出员"——取出 WebSocket 帧的最终"货物"（payload）并用掩码钥匙解开。
//   前面三个拆解员已经把基础头、扩展长度、掩码密钥都处理完了，本解析员负责最后一步：
//   从 Buffer 中读出 payloadLen 字节的 payload，用 mask key 异或解密成明文，填进 WsFrame。
//
// 关键技术点（初学者重点理解）：
//   1. 【整体到齐才消费】payload 必须完整到齐（Buffer 可读 >= payloadLen）才一次性取出，
//      不足则返回 NeedMore，不消费任何数据，保证下次从同一 payload 起点继续，不丢开头。
//   2. 【先取后解】先把密文 payload 复制进 out.payload，再用 applyWsMask 原地异或解密。
//      这样只需一次内存分配，避免"先解到临时 buffer 再拷贝"的双倍开销。
//   3. 【异或自反】applyWsMask 用同一把 mask 既可加密也可解密，因为异或运算自反
//      （a ^ b ^ b = a）。客户端用 mask 加密，服务器用同一把 mask 解密。
//   4. 【opcode 透传】本解析器只管取 payload，不做 opcode 语义解释（是文本还是二进制由
//      上层 WebSocketCodec 根据 opcode 决定如何处理 payload）。
// ==============================================================================
#pragma once
#ifndef WS_PAYLOAD_PARSER_H
#define WS_PAYLOAD_PARSER_H

#include "ParserUtils.h"
#include "server/Buffer/Buffer.h"

/**
 * @brief payload 解析器——读取 payload 并解掩码，填充 WsFrame（对称 BodyParser）
 *
 * 通俗解释：前面三个拆解员已经把面单、重量、钥匙都处理完了，最后轮到取货物的人。
 *   本解析员按重量（payloadLen）从传送带上取下货物，用钥匙（mask）解开锁，装进档案袋（WsFrame）。
 */
class PayloadParser
{
public:
    /**
     * @brief 读取 payload 并解掩码
     * @param buffer 连接读缓冲区
     * @param ctx 跨阶段上下文（读 payloadLen/mask/fin/opcode/masked）
     * @param out 输出帧对象（填 fin/opcode/masked/payload）
     * @return WsDecodeResult::Ok / NeedMore / Error
     */
    WsDecodeResult parse(Buffer &buffer, WsParseContext &ctx, WsFrame &out)
    {
        const size_t payloadLen = static_cast<size_t>(ctx.payloadLen);
        // payload 必须整体到齐才消费，不足则等下次，不丢开头
        if (buffer.readableBytes() < payloadLen)
            return WsDecodeResult::NeedMore;

        // 把 framing 状态填进输出帧
        out.fin = ctx.fin;
        out.opcode = ctx.opcode;
        out.masked = ctx.masked;
        // 先把密文 payload 复制进 out.payload
        out.payload.assign(reinterpret_cast<const char *>(buffer.peek()), payloadLen);
        // 再用 mask key 原地异或解密成明文（异或自反，加解密同一个动作）
        applyWsMask(out.payload, ctx.mask);

        // 消费掉这 payloadLen 字节
        buffer.retrieve(payloadLen);
        return WsDecodeResult::Ok;
    }

    /** @brief 重置解析器状态（本解析器无状态，空实现） */
    void reset() {}
};

#endif
