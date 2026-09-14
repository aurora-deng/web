// ==============================================================================
// 文件名：WebSocketSessionManager.h
// 职责比喻：电话局的"分机号登记台" —— WebSocket 会话的全局客房登记簿
//
// 【整体比喻】
// 想象一家大酒店：每个住客（UserId）住进一间房（WebSocketSession），客房登记簿
// （WebSocketSessionManager）记下"房号 → 房间指针 + 房间所在楼层位置"。前台要给
// 某位住客送东西时，先查登记簿找到房间在哪个楼层（Reactor 索引）、几号房（fd），
// 然后让楼层的客房服务员（SubReactor）送去——而不是自己冲上楼。
//
// 这里登记簿只记"弱引用"（weak_ptr）——房间被退掉时登记簿里的指针自动失效，
// 不会因为登记簿还指着房间而无法拆除房间。这避免了登记簿和"客房部"（Connection）
// 互相推诿"这间房归谁管"的死循环（循环引用）。
//
// 【在系统中的角色】
// 本文件是 WebSocket 业务层"主动推送"链路的起点。当业务代码想给某个 UserId 发消息时
// （比如用户 A 给用户 B 发私聊），并不需要知道 B 在哪条 Reactor 线程上：
//   1. 调 SessionManager::sendEncoded(B_uid, bytes)；
//   2. Manager 查表拿到 B 的 Locator（reactorIndex + fd + connId）；
//   3. 调 ReactorGroup::postOutbound 把 OutboundTask 投递到目标 SubReactor 的发送队列；
//   4. 目标 SubReactor 线程被 eventfd 唤醒后，把字节挂到对应 Connection 的发送缓冲。
// 整个链路对调用方屏蔽了"目标在哪个线程"的细节，是跨 Reactor 通信的关键中介。
//
// 与协议层 WebSocketParser/Codec 不同——那些是"按连接"的、生命周期绑定到
// WebSocketSession；SessionManager 是"全局"的、被所有 WebSocketSession 共享。
//
// 关键技术点（初学者重点理解）：
//   1. 【weak_ptr 而非 shared_ptr】Session 的真实所有权在 Connection::session
//      （shared_ptr）。Manager 只持有 weak_ptr 观察——Session 销毁时 weak_ptr 自动
//      失效（expired）。若用 shared_ptr 会形成"Connection 持 Session，Session 通过
//      Manager 间接持 Connection"的循环引用，导致内存泄漏。
//   2. 【Locator 的存在理由】光有 weak_ptr 不够——weak_ptr 只能告诉你"还活着"，
//      却不能告诉你"在哪条线程"。跨 Reactor 投递必须知道目标 reactorIndex 才能调
//      postOutbound；fd / connId 用于在目标 Reactor 的连接表中精确定位 Connection。
//      因此每个会话注册时一并记录 reactorIndex/fd/connId 三元组。
//   3. 【mutex 全局锁】多个 SubReactor 线程会并发注册/注销/查询会话——A 在 Reactor 0
//      注册自己，B 在 Reactor 1 同时查询 A 的位置。unordered_map 不是线程安全的，
//      必须用 mutex 串行化所有读写。锁粒度小、临界区短，竞争不激烈。
//   4. 【过期条目惰性清理】weak_ptr::expired 检测到 Session 已死时，顺手把表项 erase 掉，
//      避免登记簿越积越多死条目。这是无 GC 语言里"弱引用表"的常用清理策略。
//   5. 【零业务逻辑】Manager 不解析消息、不路由消息、不调用 handler——它只做"查表 +
//      投递字节"两件事，是纯粹的基础设施。业务路由由 WebSocketDispatcher 负责。
// ==============================================================================
#pragma once
#ifndef WEBSOCKET_SESSION_MANAGER_H
#define WEBSOCKET_SESSION_MANAGER_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "server/transport/ConnectionKey.h"
#include "server/transport/OutboundTask.h"
#include "server/websocket/WebSocketTypes/UserId.h"

class ReactorGroup;
class WebSocketSession;

/**
 * @brief 全局 WebSocket 会话目录：UserId -> 弱引用 Session + 所在 Reactor 定位信息
 *
 * 【WebSocketSessionManager 通俗解释】
 * 这就是大酒店前台的"住客登记簿"——每个住客（UserId）入住时记下房间位置（Locator），
 * 有人找他就翻登记簿查位置；住客退房（Session 销毁）时登记簿里那条自动失效。
 * 不持有房间所有权（weak_ptr），不会卡住房间不让拆。
 *
 * @note 不持有 Session 所有权（weak_ptr），避免与 Connection::session 形成循环引用。
 */
class WebSocketSessionManager
{
public:
    /**
     * @brief 绑定 ReactorGroup 引用，供后续跨 Reactor 投递使用
     * @param group 全局 ReactorGroup 指针（不持有所有权）
     * @note 必须在 sendEncoded 之前调用，否则 sendEncoded 会因 reactorGroup_ 为空而失败。
     */
    void bind(ReactorGroup *group) { reactorGroup_ = group; }

    /**
     * @brief 注册一个会话到全局目录（住客登记入住）
     * @param uid 用户 id（0 表示匿名，匿名不登记——没有房号的住客没法被寻呼）
     * @param session 会话的 shared_ptr（Manager 只存 weak_ptr，不增加引用计数）
     * @param reactorIndex 该会话所在 SubReactor 的下标（用于跨 Reactor 投递定位）
     * @param key 连接身份；fd 定位槽位，connId 防止命中复用后的新连接
     * @return true 注册成功；false uid==0、session 为空或 key 无效
     *
     * 通俗解释：住客办入住——告诉前台"我是 uid=123，住 2 楼 55 号房，本次入住单号是 9001"。
     *   房号可能复用，入住单号不会混淆这两位先后住客。
     */
    bool registerSession(UserId uid,
                         const std::shared_ptr<WebSocketSession> &session,
                         size_t reactorIndex,
                         ConnectionKey key);

    /**
     * @brief 注销会话（住客退房）
     * @param uid 要注销的用户 id
     * @param key 发起注销的连接身份
     * @return 只有目录仍指向该 key 时才删除并返回 true
     * @note 同 uid 重连后，旧 Session 的退场通知不会删除新 Session 的目录项。
     */
    bool unregister(UserId uid, ConnectionKey key);

    /**
     * @brief 向指定用户投递已编码的 WebSocket 帧字节（可跨 Reactor，线程安全）
     * @param uid 目标用户 id
     * @param encodedBytes 已编码的 WS 帧字节串（由 WebSocketCodec::encode* 生成）
     * @return Ok 表示目标 Reactor 已接收；Backpressure/Closed/Invalid 保留具体拒绝原因
     *
     * 通俗解释：前台要给某住客送东西——查登记簿找到他在哪个楼层哪个房间，
     *   然后叫楼层的客房服务员（SubReactor）送去。如果发现登记簿上指针失效
     *   （房间已退），顺手擦掉那条死记录。
     *
     * @note 这是 WebSocket 主动推送链路的核心入口。调用方无需关心目标在哪个线程，
     *       Manager + ReactorGroup::postOutbound 会自动把 OutboundTask 送到目标 Reactor 的发送队列。
     */
    EnqueueResult sendEncoded(UserId uid, std::string encodedBytes);
    /** 返回同步准入结果，并用 receipt 保留任务离开邮箱后的异步终态。 */
    OutboundSubmission sendEncodedTracked(UserId uid,
                                           std::string encodedBytes);

    /**
     * @brief 向指定用户发送一段文本（自动编码成 Text 帧）
     * @param uid 目标用户 id
     * @param text 文本内容
     * @return 目标 Reactor 的准入结果；Ok 不等于对端已经收到字节
     * @note 便捷封装：等价于 sendEncoded(uid, WebSocketCodec::encodeText(text))。
     */
    EnqueueResult sendText(UserId uid, const std::string &text);
    /** sendText 的可跟踪版本；Written 仅表示字节进入本机内核，不是对端业务 ACK。 */
    OutboundSubmission sendTextTracked(UserId uid, const std::string &text);
    size_t broadcastText(const std::vector<UserId> &users,
                         const std::string &text);

    /**
     * @brief 查询当前在线会话数（住客人数）
     * @return sessions_ 表的大小（含可能尚未惰性清理的过期条目，仅作粗略统计）
     * @note 加锁读 size，是 O(1) 操作。
     */
    size_t onlineCount() const;

private:
    EnqueueResult sendEncodedImpl(
        UserId uid,
        std::string encodedBytes,
        std::shared_ptr<OutboundReceipt> receipt);

    /**
     * @brief 会话定位信息：除 weak_ptr 外，记录"会话在哪个 Reactor 哪个 fd"
     *
     * 【Locator 通俗解释】
     * 光有 weak_ptr 知道"会话还活着"不够——跨 Reactor 投递字节必须知道目标在哪条
     * 线程。Locator 就是"住客的位置标签"：reactorIndex 是楼层号、fd 是房间号、
     * connId 是防错编号（防止 fd 被复用后送错房间）。
     */
    struct Locator
    {
        std::weak_ptr<WebSocketSession> session;  // 弱引用：Session 销毁时自动失效，不形成循环引用
        size_t reactorIndex = 0;                  // 所在 SubReactor 下标（postOutbound 据此定位目标线程）
        ConnectionKey key{};                      // fd 定位连接；connId 校验连接代际
    };

    mutable std::mutex mtx_;                                   // 全局读写锁：多 Reactor 线程并发访问 sessions_ 必须串行化
    std::unordered_map<UserId, Locator> sessions_;             // 登记簿主体：UserId -> Locator
    ReactorGroup *reactorGroup_ = nullptr;                     // 全局 ReactorGroup 引用（不持有所有权），用于跨 Reactor 投递
};

#endif
