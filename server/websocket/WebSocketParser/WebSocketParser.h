// ==============================================================================
// 文件名：WebSocketParser.h
// 职责比喻：这是"WebSocket 帧分拣传送带"的总调度室（对称 HttpParser.h 的总调度）。
//   一条 TCP 连接上源源不断来字节流（像传送带上的二进制包裹），WebSocketParser 是分拣总调度：
//   它维护一个"当前分到哪一步"的状态机（stage），把字节流依次交给四个专项拆解员——
//   FrameHeaderParser（拆 2 字节基础头）、ExtendedLengthParser（展开扩展长度）、
//   MaskKeyParser（收 4 字节掩码密钥）、PayloadParser（取 payload 并解掩码）。
//   每个拆解员干完一步，总调度就切换到下一个状态，直到整个帧分拣完成。
//
// 关键技术点（初学者重点理解）：
//   1. 【状态机解析】把解析过程拆成 BASE_HEADER→EXT_LENGTH→MASK_KEY→PAYLOAD→COMPLETE 等状态，
//      按字节推进。这样能优雅处理"数据没到齐"的情况，不完整就停在当前状态等下次。
//   2. 【增量解析】只消费已确认完整且合法的字节，未完成数据留在 Buffer。半包/粘包下不会丢数据。
//   3. 【与 HTTP 解析的本质区别】HTTP 是文本协议，按 CRLF/空格切分；WebSocket 是二进制协议，
//      按位字段切分（FIN 1 位/RSV 3 位/opcode 4 位...）。这就是为什么需要完全不同的一套解析器。
//   4. 【payload 上限】kWsMaxPayloadBytes（1 MiB）在分配大对象前就拒绝异常输入，防 DoS。
//   5. 【子解析器栈上分配】四个子解析器是栈上成员对象，无需动态分配，解析器构造无堆开销。
//   6. 【结果只交付一次】COMPLETE 状态用 resultDelivered_ 保证一个完整帧只返回一次 Ok，
//      防止误用导致"一个帧被处理两次"。
// ==============================================================================
#pragma once
#ifndef WEBSOCKET_PARSER_H
#define WEBSOCKET_PARSER_H

#include "ParserUtils.h"
#include "FrameHeaderParser.h"
#include "ExtendedLengthParser.h"
#include "MaskKeyParser.h"
#include "PayloadParser.h"
#include "server/Buffer/Buffer.h"
#include "server/websocket/WebSocketTypes/WebSocketTypes.h"

#include <cstddef>

/**
 * @brief 解析状态机的各个阶段
 *
 * 通俗解释：就像快递分拣的工位顺序——先拆"基础面单"（2 字节头），再看"重量纸条"（扩展长度），
 *   收下"解密钥匙"（mask key），最后取出"货物"（payload）。每个工位干完去下一个，货物取完就到
 *   COMPLETE 完成区。
 */
enum class WsParseStage
{
    BASE_HEADER,   // 解析 2 字节基础帧头：FIN/RSV/opcode/MASK/len7
    EXT_LENGTH,    // 展开 126/127 扩展长度字段（若 len7 < 126 则跳过）
    MASK_KEY,      // 读取 4 字节掩码密钥
    PAYLOAD,       // 读取 payload 并解掩码
    COMPLETE,      // 整帧解析完成
    ERROR          // 解析出错
};

/**
 * @brief WebSocket 帧增量解析器——协调四个子解析器的"总调度"（对称 HttpParser）
 *
 * 通俗解释：WebSocketParser 自己不亲自拆字节，它像车间流水线调度员：盯着 stage 状态机，
 *   当前该拆基础头就把 Buffer 交给 FrameHeaderParser，该展开长度就交给 ExtendedLengthParser……
 *   子解析器干完汇报结果，调度员据此切换 stage，循环推进直到 COMPLETE。
 *
 * 【framing 状态传递 通俗解释】"framing"就是确定帧边界——payload 多长？是否掩码？是否最后一帧？
 *   这些信息在基础头/扩展长度解析时得到，要传给 PayloadParser 决定怎么取 payload。
 *   WebSocketParser 通过 WsParseContext 在子解析器之间搬运这些状态。
 */
class WebSocketParser
{
public:
    // 对外暴露的 payload 上限常量，供上层校验或日志使用
    static constexpr size_t kMaxPayloadBytes = kWsMaxPayloadBytes;

    /**
     * @brief 解析主循环：按 stage 状态机持续推进，直到完成/需更多数据/出错
     * @param buffer 连接读缓冲区
     * @param out 输出帧对象
     * @return WsDecodeResult::Ok（完整）/ NeedMore（等数据）/ Error（出错）/ Closed（Close 帧）
     */
    WsDecodeResult parse(Buffer &buffer, WsFrame &out);

    /**
     * @brief 重置解析器，准备解析下一个帧
     * @note 只重置解析器自身状态，不清空 Buffer——其中可能已有下一帧的数据。
     */
    void reset();

    /** @brief 查询当前所处状态机阶段（供上层调试或状态判断） */
    WsParseStage stage() const { return stage_; }

private:
    WsParseStage stage_ = WsParseStage::BASE_HEADER;  // 当前所处状态机阶段
    bool resultDelivered_ = false;                     // 完整帧是否已交付一次，防止重复交付
    WsParseContext ctx_;                               // 跨阶段流转单

    // 四个子解析器均为栈上成员对象，无需动态分配
    FrameHeaderParser frameHeaderParser_;
    ExtendedLengthParser extendedLengthParser_;
    MaskKeyParser maskKeyParser_;
    PayloadParser payloadParser_;
};

#endif
