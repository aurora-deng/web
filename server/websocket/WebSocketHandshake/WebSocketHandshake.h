// ==============================================================================
// 文件名：WebSocketHandshake.h
// 职责比喻：WebSocket 握手工具类——从 HTTP 到 WebSocket 的"对暗号升级通道"窗口
//
// 【整体比喻】
// WebSocket 连接不是凭空建立的，它"寄生"在 HTTP 之上。客户端先发一个特殊的 HTTP
// 请求（带 Upgrade: websocket 头）来"敲门对暗号"，服务端校验通过后回 101 Switching
// Protocols，此后这条 TCP 连接就不再是 HTTP 了，而切换成 WebSocket 二进制帧协议。
// WebSocketHandshake 就是这个"对暗号升级通道"的窗口：检查旅客（请求）的证件是否齐全，
// 盖章放行（回 101），然后移交给 WebSocketSession（长期管家）接管后续帧收发。
//
// 【握手核心：Sec-WebSocket-Accept 计算】
// 客户端发来随机字符串 Sec-WebSocket-Key，服务端拼上固定 GUID 后做 SHA1 再 Base64，
// 得到 Sec-WebSocket-Accept 返回。这不是密码学鉴权，只是验证双方都懂 WebSocket 协议，
// 避免老旧 HTTP 代理误把长连接当普通 HTTP 处理。
//
// 【在系统中的角色】
// 对称 http/ 模块的"请求解析→响应生成"流程，只是握手成功后不再走 HttpCodec/HttpSession，
// 而是把 fd 交给 WebSocketSession 接管。本头文件只声明接口，实现（含内置 SHA-1+Base64）
// 在 WebSocketHandshake.cpp 的匿名命名空间内，避免引入 OpenSSL 等外部依赖。
//
// 关键技术点（初学者重点理解）：
//   1. HTTP Upgrade 机制：用 HTTP/1.1 的 101 状态码完成协议切换，复用已有 TCP 连接。
//   2. Accept Key 公式：Base64(SHA1(Key + GUID))，双方核对同一公式，非密码学鉴权。
//      GUID 是协议固定的魔法字符串 "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"（RFC 6455 §1.3）。
//   3. 无第三方依赖：内置 SHA-1 + Base64 实现，适合轻量部署和教学。
//   4. 全静态方法：握手是无状态的一次性校验，不需要实例化。
//
// RFC 6455 §4.1 描述客户端请求字段要求，§4.2 描述服务端响应要求。
// ==============================================================================
#pragma once
#ifndef WEBSOCKET_HANDSHAKE_H
#define WEBSOCKET_HANDSHAKE_H

#include "server/http/http.h"

#include <string>

/**
 * @brief WebSocket HTTP 升级握手工具类（RFC 6455 §4）
 *
 * 通俗解释：这就是"对暗号升级通道"的窗口办事员。全静态方法，无实例状态——
 *   握手是一次性校验，办完就走人，不需要保留任何状态。
 *
 * 典型调用顺序：
 *   isUpgradeRequest（路由快速分流） -> validate（严格校验+算密钥） -> fillResponse（构造 101）。
 */
class WebSocketHandshake
{
public:
    /**
     * @brief 握手校验结果，由 validate 填充，供 fillResponse 消费
     *
     * 通俗解释：办事员检查完证件后给你一张回执——ok 表示通过，
     *   acceptKey 是算好的"通关印章"（写入 101 响应头），
     *   failReason 在失败时说明哪里不对（可用于日志或 HTTP 400 响应）。
     */
    struct Result
    {
        bool ok = false;          // 是否校验通过
        std::string acceptKey;    // Sec-WebSocket-Accept 头值（仅 ok=true 时有效）
        std::string failReason;   // 失败原因（用于日志或 HTTP 400 响应）
    };

    /**
     * @brief 快速判断请求是否为 WebSocket 升级意图（仅做轻量级特征匹配，不校验完整性）
     *
     * 通俗解释：办事员远远看一眼来人是不是冲着"对暗号升级通道"来的——
     *   GET 方法 + Upgrade:websocket + Connection:upgrade + 带了 Sec-WebSocket-Key，
     *   四个特征都有就放行到下一步。不查证件完整性，避免在普通 HTTP 路由层做重活。
     *
     * @param req 已解析的 HTTP 请求
     * @return 满足四项特征返回 true；否则为普通 HTTP 请求返回 false
     * @note 用于 HTTP 路由层在尚未完整解析时区分普通 HTTP 与 WS 升级请求
     */
    static bool isUpgradeRequest(const HttpRequest &req);

    /**
     * @brief 严格校验握手必填字段并计算 Accept Key（RFC 6455 §4.2.2）
     *
     * 通俗解释：办事员逐项严格检查六项证件（method/version/Upgrade/Connection/Key/Version），
     *   全部合格才用公式 Base64(SHA1(Key+GUID)) 算出"通关印章"（Accept Key）。
     *
     * @param req 待校验的请求
     * @return Result.ok=true 表示通过，acceptKey 可用；否则 failReason 说明失败原因
     * @note 校验失败时，上层代码需要向客户端返回 HTTP 400 Bad Request
     */
    static Result validate(const HttpRequest &req);

    /**
     * @brief 将校验通过的握手写入 HttpResponse，构造 101 Switching Protocols 响应
     *
     * 通俗解释：盖完章，写一张"放行条"——状态码 101，带上 Upgrade/Connection/Accept 三个头。
     *   这条响应一发出去，TCP 通道就正式从 HTTP 切换成 WebSocket 二进制帧流。
     *
     * @param resp 待填充的响应对象（会被 reset 清空）
     * @param result validate 返回的结果（必须 ok=true）
     * @warning 协议强制规则：101 状态码的响应不能携带消息体（body）
     */
    static void fillResponse(HttpResponse &resp, const Result &result);

    /**
     * @brief 计算 Sec-WebSocket-Accept：Base64(SHA1(Sec-WebSocket-Key + 固定 GUID))
     *
     * 通俗解释：把客户端的随机 Key 和协议固定的 GUID 拼起来，做 SHA1 指纹再 Base64 编码，
     *   得到 28 字符的"通关印章"字符串。客户端用同一公式校验，对上了才认这条升级通道。
     *
     * @param secWebSocketKey 客户端请求中的 Sec-WebSocket-Key 值
     * @return 28 字符 Base64 串，可直接写入 101 响应头
     * @note 暴露为公开静态方法以便单元测试直接验证算法正确性
     */
    static std::string computeAcceptKey(const std::string &secWebSocketKey);
};

#endif
