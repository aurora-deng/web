// ==============================================================================
// 文件名：MaskKeyParser.h
// 职责比喻：这是"掩码密钥收取员"——收下客户端随帧附带的 4 字节"解密钥匙"。
//   RFC 6455 要求客户端→服务器的每一帧都必须掩码，掩码密钥（mask key）是 4 字节随机数，
//   紧跟在帧头之后。本解析员把这 4 字节收下来存进流转单，供 PayloadParser 解掩码用。
//
// 关键技术点（初学者重点理解）：
//   1. 【掩码密钥的位置】只在 MASK=1 时才出现，紧跟基础头（和扩展长度）之后，固定 4 字节。
//      FrameHeaderParser 已强制校验 MASK=1，所以本解析器无脑读 4 字节即可。
//   2. 【为什么是 4 字节】RFC 6455 规定掩码密钥固定 4 字节，payload 第 i 字节与 mask[i%4]
//      异或解密。4 字节是兼顾混淆效果和开销的折中。
//   3. 【每帧独立掩码】每个帧的 mask key 都是独立随机的，不能复用上一帧的。所以本解析器
//      每帧都要重新读 4 字节。
//   4. 【headerLen 累计】ctx.headerLen 由 FrameHeaderParser 设为 2（或 ExtendedLengthParser
//      设为 4/10），本解析器在此基础上 += 4，供上层统计完整头部长度。
// ==============================================================================
#pragma once
#ifndef WS_MASK_KEY_PARSER_H
#define WS_MASK_KEY_PARSER_H

#include "ParserUtils.h"
#include "server/Buffer/Buffer.h"

/**
 * @brief 掩码密钥解析器——读取 4 字节 mask key（对称 BodyParser 前的 framing 准备阶段）
 *
 * 通俗解释：快递包裹上挂了一把"小锁"，锁钥匙（4 字节 mask key）就贴在包裹外面。
 *   本解析员把钥匙取下来收好，等取货物的人（PayloadParser）用它解开。
 */
class MaskKeyParser
{
public:
    /**
     * @brief 读取 4 字节掩码密钥
     * @param buffer 连接读缓冲区
     * @param ctx 跨阶段上下文（输出 mask，更新 headerLen）
     * @return WsDecodeResult::Ok / NeedMore / Error
     */
    WsDecodeResult parse(Buffer &buffer, WsParseContext &ctx)
    {
        // 4 字节必须整体到齐，不足则等下次
        if (buffer.readableBytes() < 4)
            return WsDecodeResult::NeedMore;

        const auto *p = reinterpret_cast<const uint8_t *>(buffer.peek());
        // 收下 4 字节掩码密钥，存进流转单
        ctx.mask[0] = p[0];
        ctx.mask[1] = p[1];
        ctx.mask[2] = p[2];
        ctx.mask[3] = p[3];
        // 累计头部长度：之前的基础头(+扩展长度) + 这 4 字节 mask key
        ctx.headerLen += 4;

        // 消费掉这 4 字节
        buffer.retrieve(4);
        return WsDecodeResult::Ok;
    }

    /** @brief 重置解析器状态（本解析器无状态，空实现） */
    void reset() {}
};

#endif
