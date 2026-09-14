// ==============================================================================
// 文件名：WebSocketTypes.h
// 职责比喻：WebSocket 协议的"字典与码本"——定义帧类型、状态码等基础数据类型
//
// 【整体比喻】
// 如果把 WebSocket 通信比作两个人用电报交流，那本文件就是"电报码本"：
// 它定义了每种电报（二进制包裹）的类型代号（opcode，即"包裹类型"）、收报状态
// （WsDecodeResult）、电报格式（WsFrame，即"二进制包裹"的结构）以及挂断原因码
// （WsCloseCode）。所有 WebSocket 模块都查这本字典，保证大家对同一个数字有相同的
// 理解——比如 0x1 都表示"文本帧"。
//
// 【在系统中的角色】
// 本文件是整个 websocket/ 目录的类型地基：WebSocketHandshake（对暗号升级通道）、
// WebSocketParser（拆包裹）、WebSocketCodec（翻译官）、WebSocketSession（长管家）
// 都依赖这里定义的枚举与结构体。它本身只有类型声明，无实现逻辑，不依赖项目内其他
// 头文件，便于被任意翻译单元包含。与 http/ 模块的 HttpResponse/HttpCode 等基础类型
// 对称——只是 WebSocket 是二进制协议，类型更贴近 RFC 6455 的位字段语义。
//
// 关键技术点（初学者重点理解）：
//   1. opcode 帧类型：低 4 位标识帧用途（文本/二进制/关闭/Ping/Pong），>=0x8 为控制帧。
//   2. 控制帧约束：Close/Ping/Pong 不可分片（FIN 必须=1），payload（货物）不超 125 字节。
//   3. 掩码（加密钥匙）：客户端→服务端帧必须掩码，服务端→客户端帧禁止掩码（RFC 6455 §5.3）。
//   4. 关闭码语义：1000 正常关闭、1002 协议错误、1009 消息过大等，部分码不可出现在 wire 上。
//
// RFC 参考：RFC 6455 "The WebSocket Protocol"
//   - §5.2  帧格式（opcode / FIN / mask）
//   - §7.4  关闭码定义
// ==============================================================================
#pragma once
#ifndef WEBSOCKET_TYPES_H
#define WEBSOCKET_TYPES_H

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief 帧类型枚举：标识"二进制包裹"的用途（RFC 6455 §5.2 opcode 字段，低 4 位）
 *
 * 通俗解释：每个 WebSocket 帧的最前面 4 个 bit 就是指"包裹类型"——
 *   是普通文本、二进制文件、还是心跳探活、还是挂断请求。收件人看到类型代号
 *   就知道该怎么处理里面的"货物"（payload）。
 *
 * @note 取值范围 0x0~0xB，本枚举仅列出协议实际使用的 6 种。
 *       opcode >= 0x8 为控制帧，控制帧不可分片且 payload 长度不超过 125 字节。
 */
enum class WsOpcode : uint8_t
{
    Continuation = 0x0, // 续帧：前一帧为分片帧时，后续分片用此 opcode
    Text = 0x1,         // 文本帧：payload 为 UTF-8 文本
    Binary = 0x2,       // 二进制帧：payload 为任意字节
    Close = 0x8,        // 关闭帧：对端请求关闭连接（控制帧）
    Ping = 0x9,         // 心跳 Ping：要求对端尽快回 Pong（控制帧）
    Pong = 0xA          // 心跳 Pong：对 Ping 的响应（控制帧）
};

/**
 * @brief 帧解码结果状态：告诉上层这帧拆得怎么样了
 *
 * 通俗解释：拆包裹的拆解员（Parser）每拆完一步要向总管汇报：
 *   Ok（拆好了，可以交货）/ NeedMore（货还没到齐，再等等）/ Error（包裹格式坏掉了，挂断）/
 *   Closed（对方要挂断，回个 Close 完成关闭握手）。WebSocketSession 据此决定下一步动作
 *   （交付业务 / 继续读 / 关闭）。
 */
enum class WsDecodeResult
{
    Ok,        // 得到完整帧：可交付给业务层处理
    NeedMore,  // 缓冲不足：当前字节不足以构成完整帧，需继续读 socket
    Error,     // 协议错误（应关闭连接）：违反 RFC 6455，按 §7.1.7 关闭
    Closed     // 对端 Close 帧（业务侧应回 Close）：完成关闭握手
};

/**
 * @brief 解析后的 WebSocket 帧逻辑表示——一个拆完的"二进制包裹"
 *
 * 通俗解释：这是拆解员把包裹拆完后交到业务手上的"货物清单"：
 *   fin 表示这是不是最后一箱（分片消息的尾帧）、opcode 是包裹类型、
 *   masked 标记来时是否带"加密钥匙"（掩码）、payload 是真正要送的"货物"（已解掩码）。
 *
 * @note 不保留原始 wire 字节，仅保留业务关心字段，掩码已解密到 payload。
 *       设计为值类型，便于在帧队列中拷贝传递。
 */
struct WsFrame
{
    bool fin = true;                          // FIN 位：true 表示消息最后一帧（RFC 6455 §5.2）
    WsOpcode opcode = WsOpcode::Text;         // 帧类型（包裹类型）
    bool masked = false;                      // 客户端→服务端帧必须置掩码（§5.3）
    std::string payload;                      // 应用层数据（已解掩码的"货物"）。用 string 因多数场景为文本
};

/**
 * @brief 关闭状态码：挂断时给对方的一个原因编号（RFC 6455 §7.4.1）
 *
 * 通俗解释：就像挂电话时说一句"我挂了，是因为信号不好/协议不对/消息太大"，
 *   让对方知道发生了什么。仅列举服务端常用的合法码；1004 是保留值，1005/1006
 *   是内部指示，不可出现在 wire 上（只能在程序内部使用，不能写进 Close payload）。
 */
enum class WsCloseCode : uint16_t
{
    Normal = 1000,           // 正常关闭
    GoingAway = 1001,        // 服务器/端点下线
    ProtocolError = 1002,    // 协议错误
    UnsupportedData = 1003,  // 收到不支持的数据类型
    InvalidPayload = 1007,   // 文本消息或 Close 原因不是合法 UTF-8
    PolicyViolation = 1008,  // 消息违反服务端策略
    MessageTooBig = 1009,    // 消息过大
    MandatoryExtension = 1010, // 客户端要求服务端协商某扩展
    InternalError = 1011,    // 服务端内部错误
    ServiceRestart = 1012,   // 服务正在重启
    TryAgainLater = 1013,    // 服务暂时繁忙
    BadGateway = 1014        // 网关收到无效上游响应
};

#endif
