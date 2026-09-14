// ==============================================================================
// 文件名：WebSocketCodec.h
// 职责比喻：WebSocket 编解码器——字节流与帧/业务消息之间的"翻译官"（对称 HttpCodec）
//
// 【整体比喻】
// 如果说 WebSocketParser 是"拆包裹"的拆解员，
// 那 WebSocketCodec 就是夹在中间的"翻译官"：它把拆解员交上来的 WsFrame 翻译成业务
// 能懂的 WebSocketMessage（拆包→翻译→交业务），又把业务要发的 WebSocketMessage 翻译
// 成字节串交给 WebSocketSession 由其入 OutboundTask 队列（业务→翻译→组帧）。它对外提供
// 的 decode/messageFromFrame/dispatch/encode 四件套，让 WebSocketSession 不用直接碰 Parser
// 的细节。出站字节由 WebSocketSession 通过 OutboundTask 体系交给 TransportWriter，不经由 Codec。
//
// 【在系统中的角色】
// 对称 http/ 模块的 HttpCodec——两者都不继承任何抽象基类，由各自 Session 持有，
// 负责把"字节流 ↔ 业务消息"的双向翻译封装成统一接口。区别在于：
//   - HttpCodec 翻译的是 HTTP 文本协议（按 CRLF 切分）；
//   - WebSocketCodec 翻译的是 WebSocket 二进制帧协议（按位字段切分），
//     并额外承担"帧 ↔ 业务消息"的语义转换（如把 JSON 文本帧解析成 type/toUserId）。
//
// 数据流（四条翻译路径）：
//   decode：Buffer + Parser → WsFrame（委托 Parser 拆帧，解析完自动 reset）
//   messageFromFrame：WsFrame → WebSocketMessage（帧→业务对象）
//   dispatch：WebSocketMessage → Dispatcher → handler（交给业务路由）
//   encode：WebSocketMessage / WsFrame → 协议字节（组帧，交给 WebSocketSession 入 OutboundTask 队列）
//
// 关键技术点（初学者重点理解）：
//   1. 【组合模式】Codec 内部不亲自拆帧/组帧，而是组合 WebSocketParser（拆）和
//      encodeRaw 工具函数（组），自身只做"帧 ↔ 业务消息"的语义翻译。这种分层让
//      解析逻辑可独立测试，Codec 专注业务适配。
//   2. 【对称 HttpCodec】两者接口形态相近（decode/dispatch/encode），但都不继承抽象基类；Session
//      层按协议选择对应 Codec 实例，不依赖多态。
//   3. 【服务端发帧不加掩码】所有 encode* 方法都不写 mask 位——RFC 6455 §5.1 规定
//      服务端→客户端帧禁止掩码，这是和客户端发帧最大的不同。
//   4. 【帧类型保留】encode 时不改变业务给的 opcode（包裹类型），分片帧的 FIN 位
//      由调用方决定（encodeText/encodeBinary 的 fin 参数）。
//   5. 【静态 encode 工具】encodeText/encodeBinary/encodeClose/encodePing/encodePong
//      均为静态方法，无需实例即可组帧，方便 Session 在任意位置构造控制帧。
// ==============================================================================
#pragma once
#ifndef WEBSOCKET_CODEC_H
#define WEBSOCKET_CODEC_H

#include "server/Buffer/Buffer.h"
#include "server/websocket/WebSocketDispatcher/WsMessageContext.h"
#include "server/websocket/WebSocketMessage/WebSocketMessage.h"
#include "server/websocket/WebSocketParser/WebSocketParser.h"
#include "server/websocket/WebSocketTypes/WebSocketTypes.h"

#include <cstddef>
#include <string>

class WebSocketDispatcher;

/**
 * @brief WebSocket 编解码器——字节流与业务消息之间的"翻译官"（对称 HttpCodec）
 *
 * 通俗解释：Codec 自己不拆字节也不写 socket，它像翻译官：
 *   - 收到拆解员（Parser）交来的"二进制包裹"（WsFrame），翻译成业务能懂的
 *     WebSocketMessage（messageFromFrame），再交给 Dispatcher 路由到具体 handler；
 *   - 业务要回消息时，它把 WebSocketMessage 翻译回字节串（encode），交给 WebSocketSession
 *     入 OutboundTask 队列。它不继承抽象基类，与 HttpCodec 接口形态对称但彼此独立。
 *
 * @note 服务端发送的所有帧都不加掩码——RFC 6455 §5.1 规定服务端→客户端帧禁止掩码。
 */
class WebSocketCodec
{
public:
    // 对外暴露的 payload（货物）上限常量，直接复用 Parser 的限制，供上层校验或日志使用
    static constexpr size_t kMaxPayloadBytes = WebSocketParser::kMaxPayloadBytes;

    /**
     * @brief 构造时绑定一个 WebSocketDispatcher，用于 dispatch 时路由消息到业务 handler
     * @param dispatcher 业务派发器引用，必须比 Codec 活得长（通常由 SessionManager 持有）
     */
    explicit WebSocketCodec(WebSocketDispatcher &dispatcher) : dispatcher_(dispatcher) {}

    /**
     * @brief 解码：委托 Parser 从 Buffer 拆出一个完整帧
     * @param buffer 连接读缓冲区（可能含半包/粘包）
     * @param parser 该连接专属的帧解析器（持有状态机进度）
     * @param out 输出帧对象
     * @return WsDecodeResult::Ok（完整）/ NeedMore（等数据）/ Error（出错）/ Closed（Close 帧）
     * @note 解析成功（Ok 或 Closed）后自动 reset parser，准备解析下一帧
     */
    WsDecodeResult decode(Buffer &buffer, WebSocketParser &parser, WsFrame &out);

    /**
     * @brief 把帧翻译成业务消息对象（WsFrame → WebSocketMessage）
     * @param frame 已解析的帧（包裹已拆完，payload 已解掩码）
     * @return 业务消息对象，含 opcode/text/type/toUserId 等业务字段
     * @note 文本帧会尝试解析 JSON 或 @user:text 格式提取 type 和 toUserId；
     *       二进制帧直接标记 type="binary"
     */
    WebSocketMessage messageFromFrame(const WsFrame &frame) const;

    /**
     * @brief 派发：把业务消息交给 Dispatcher 路由到具体 handler
     * @param ctx 消息上下文，含 inbound 消息、session 引用、uid 等
     * @return true 表示有 handler 处理并可能产生 outbound；false 表示无 handler
     */
    bool dispatch(WsMessageContext &ctx);

    /**
     * @brief 编码：把业务消息翻译成字节串（WebSocketMessage → 协议字节）
     * @param message 业务消息（文本走 encodeText，二进制走 encodeBinary）
     * @return 已编码的帧字节串，由 WebSocketSession 入 OutboundTask 队列
     * @note 服务端发帧不加掩码（RFC 6455 §5.1）
     */
    std::string encode(const WebSocketMessage &message) const;

    /**
     * @brief 把业务消息序列化成扁平 JSON 应用信封（尚未添加 WebSocket 帧头）
     * @note 与 encode() 分开：encode() 保留发送原始 text 的兼容语义；跨用户可靠消息显式
     *       调本方法，避免把“业务信封”和“RFC 6455 帧”混成一层。
     */
    static std::string serializeApplicationMessage(
        const WebSocketMessage &message);

    /**
     * @brief 编码：把已有帧对象翻译成字节串（WsFrame → 协议字节）
     * @param frame 帧对象（opcode/fin/payload 由调用方填好）
     * @return 已编码的帧字节串
     * @note 服务端发帧不加掩码
     */
    static std::string encode(const WsFrame &frame);

    /**
     * @brief 编码文本帧（opcode=Text）
     * @param text 文本内容（UTF-8）
     * @param fin 是否为消息最后一帧（默认 true，单帧消息）
     * @return 已编码的帧字节串
     */
    static std::string encodeText(const std::string &text, bool fin = true);

    /**
     * @brief 编码二进制帧（opcode=Binary）
     * @param data 二进制数据
     * @param fin 是否为消息最后一帧（默认 true，单帧消息）
     * @return 已编码的帧字节串
     */
    static std::string encodeBinary(const std::string &data, bool fin = true);

    /** 编码不携带状态码和原因的空 Close 帧。 */
    static std::string encodeClose();

    /** 编码任意合法 wire 状态码；非法状态码、原因或超长载荷返回空串。 */
    static std::string encodeClose(uint16_t code, const std::string &reason = {});

    /**
     * @brief 编码关闭帧（opcode=Close）
     * @param code 关闭状态码（如 1000 正常关闭、1002 协议错误）
     * @param reason 可选的人类可读关闭原因
     * @return 已编码的帧字节串，payload 前 2 字节为 code（大端），其后为 reason
     * @note Close 帧的 FIN 固定为 true（控制帧不可分片）
     */
    static std::string encodeClose(WsCloseCode code, const std::string &reason = {});

    /**
     * @brief 编码 Ping 帧（opcode=Ping），用于心跳探活
     * @param payload 可选的探活数据（不超 125 字节）
     * @return 已编码的帧字节串
     * @note Ping 帧的 FIN 固定为 true（控制帧不可分片）
     */
    static std::string encodePing(const std::string &payload = {});

    /**
     * @brief 编码 Pong 帧（opcode=Pong），用于回应 Ping
     * @param payload 可选的回执数据（通常回显 Ping 的 payload）
     * @return 已编码的帧字节串
     * @note Pong 帧的 FIN 固定为 true（控制帧不可分片）
     */
    static std::string encodePong(const std::string &payload = {});

private:
    WebSocketDispatcher &dispatcher_;  // 业务派发器引用，dispatch 时用来路由到 handler
};

#endif
