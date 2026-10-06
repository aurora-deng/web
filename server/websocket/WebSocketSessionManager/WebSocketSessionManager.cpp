// ==============================================================================
// 文件名：WebSocketSessionManager.cpp
// 职责比喻：电话局"分机号登记台"的工作手册 —— 实现"登记 / 注销 / 跨楼层寻呼"
//
// 【整体比喻】
// 接 WebSocketSessionManager.h 的酒店比喻：本文件是前台登记簿的"操作手册"，
// 一步步说明怎么登记新住客、怎么注销退房住客、怎么按房号跨楼层送东西。
//
// 【跨 Reactor 投递链路（初学者重点理解）】
//   业务层 sendEncoded(B_uid, bytes)
//     → 加锁查 sessions_[B_uid] 拿到 Locator（含 reactorIndex/fd/connId）
//     → 检查 weak_ptr 是否过期；过期则惰性 erase 并返回 false
//     → 释放锁后调 ReactorGroup::postOutbound(reactorIndex, fd, connId, OutboundTask)
//     → ReactorGroup 转发给目标 SubReactor::postOutbound
//     → 目标 SubReactor 把任务投到自己的 pendingOutbound_ 队列
//     → eventfd 唤醒目标 Reactor 线程
//     → 目标 Reactor 线程在自己线程内 processPendingOutbound → enqueueOutbound（由 writerLoop 冲刷）
//
// 关键技术点（初学者重点理解）：
//   1. 【锁粒度最小化】查表 + 拷贝 Locator 在锁内完成，真正的跨线程投递（postOutbound）
//      在锁外执行——避免持锁期间被网络 IO 阻塞拖累其他线程的注册/注销。
//   2. 【weak_ptr 过期检测】sendEncoded 发现 session.expired() 时顺手 erase 该条目，
//      实现登记簿的惰性清理，避免死条目堆积。
//   3. 【匿名用户拒绝登记】uid==0 表示匿名，匿名用户没有"房号"无法被寻呼，
//      registerSession 直接返回 false。
//   4. 【connId 的作用】postOutbound 同时传 fd 和 connId——fd 可能被系统重新分配给
//      新连接，目标 Reactor 用 connId 二次校验"这条 fd+connId 还活着且是同一条连接"，
//      避免把字节送进一个已被复用 fd 的新连接。
//   5. 【sendText 复用 sendEncoded】文本发送只是先把 text 编码成 Text 帧字节，
//      再走统一的 sendEncoded 路径——避免重复实现跨 Reactor 投递逻辑。
// ==============================================================================
#include "WebSocketSessionManager.h"

#include "server/Reactor/ReactorGroup.h"
#include "server/websocket/WebSocketCodec/WebSocketCodec.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

// ---- 会话注册：把(uid, session, 位置)三元组写入登记簿 ----
bool WebSocketSessionManager::registerSession(UserId uid,
                                              const std::shared_ptr<WebSocketSession> &session,
                                              size_t reactorIndex,
                                              ConnectionKey key)
{
    // 匿名用户无房号；空 session 或无效连接身份也不允许登记。
    if (uid == 0 || !session || !key)
        return false;
    PresenceObserver observer;
    std::size_t connections = 0;
    {
        // 同一个 uid 保留多个 Locator：手机和电脑都能同时收到持久消息广播。
        std::lock_guard<std::mutex> lock(mtx_);
        auto &bucket = sessions_[uid];
        std::erase_if(bucket, [](const Locator &item)
                      { return item.session.expired(); });
        if (std::any_of(bucket.begin(), bucket.end(), [&](const Locator &item)
                        { return item.key == key; }) ||
            bucket.size() >= maxConnectionsPerUser_)
        {
            if (bucket.empty())
                sessions_.erase(uid);
            return false;
        }
        bucket.push_back(Locator{session, reactorIndex, key});
        connections = bucket.size();
        observer = presenceObserver_;
    }
    // 观察者在锁外执行，避免数据库/业务回调反过来堵住所有会话登记。
    if (observer)
        observer(uid, connections);
    return true;
}

// ---- 会话注销：只从该 uid 的设备列表中擦除指定连接 ----
bool WebSocketSessionManager::unregister(UserId uid, ConnectionKey key)
{
    // 匿名用户从未登记过，无需注销
    if (uid == 0 || !key)
        return false;
    PresenceObserver observer;
    std::size_t connections = 0;
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = sessions_.find(uid);
        if (it == sessions_.end())
            return false;
        auto &bucket = it->second;
        removed = std::any_of(bucket.begin(), bucket.end(), [&](const Locator &item)
                              { return item.key == key; });
        std::erase_if(bucket, [&](const Locator &item)
                      { return item.session.expired() || item.key == key; });
        connections = bucket.size();
        if (bucket.empty())
            sessions_.erase(it);
        observer = presenceObserver_;
    }
    if (removed && observer)
        observer(uid, connections);
    return removed;
}

void WebSocketSessionManager::setPresenceObserver(PresenceObserver observer)
{
    std::lock_guard<std::mutex> lock(mtx_);
    presenceObserver_ = std::move(observer);
}

// ---- 跨 Reactor 投递：核心方法，链路见文件头注释 ----
EnqueueResult WebSocketSessionManager::sendEncodedImpl(
    UserId uid,
    std::string encodedBytes,
    std::shared_ptr<OutboundReceipt> receipt)
{
    if (encodedBytes.empty())
        return EnqueueResult::Invalid;
    if (!reactorGroup_)
        return EnqueueResult::Closed;

    Locator loc;
    {
        // 临界区：只做查表 + 拷贝，尽快释放锁
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = sessions_.find(uid);
        if (it == sessions_.end())
            return EnqueueResult::Closed;  // 用户不在线
        auto &bucket = it->second;
        std::erase_if(bucket, [](const Locator &item)
                      { return item.session.expired(); });
        if (bucket.empty())
        {
            sessions_.erase(it);
            return EnqueueResult::Closed;
        }
        // 可靠投递旧接口保留单一回执语义，选择最近登记的连接；业务广播会发给全部设备。
        loc = bucket.back();
    }  // 锁在此处释放——postOutbound 在锁外执行，避免持锁期间被 IO 阻塞

    // 把字节投递到目标 Reactor 的发送队列（由目标线程自行 flush）
    return reactorGroup_->postOutbound(
        loc.reactorIndex,
        loc.key.fd,
        loc.key.connId,
        OutboundTask::encoded(
            std::move(encodedBytes),
            0,
            OutboundCompletion::None,
            std::move(receipt)));
}

EnqueueResult WebSocketSessionManager::sendEncoded(
    UserId uid,
    std::string encodedBytes)
{
    return sendEncodedImpl(uid, std::move(encodedBytes), {});
}

OutboundSubmission WebSocketSessionManager::sendEncodedTracked(
    UserId uid,
    std::string encodedBytes)
{
    auto receipt = std::make_shared<OutboundReceipt>();
    const auto admission =
        sendEncodedImpl(uid, std::move(encodedBytes), receipt);
    if (admission == EnqueueResult::Backpressure)
        receipt->complete(OutboundOutcome::Backpressured);
    else if (admission == EnqueueResult::Closed)
        receipt->complete(OutboundOutcome::Closed);
    else if (admission == EnqueueResult::Invalid)
        receipt->complete(OutboundOutcome::Invalid);
    return {admission, std::move(receipt)};
}

// ---- 文本发送便捷封装：先编码成 Text 帧，再走 sendEncoded ----
EnqueueResult WebSocketSessionManager::sendText(
    UserId uid,
    const std::string &text)
{
    return sendEncoded(uid, WebSocketCodec::encodeText(text));
}

OutboundSubmission WebSocketSessionManager::sendTextTracked(
    UserId uid,
    const std::string &text)
{
    return sendEncodedTracked(uid, WebSocketCodec::encodeText(text));
}

size_t WebSocketSessionManager::broadcastText(
    const std::vector<UserId> &users,
    const std::string &text)
{
    if (!reactorGroup_ || users.empty())
        return 0;

    auto encoded = WebSocketCodec::encodeText(text);
    if (encoded.empty())
        return 0;

    std::vector<Locator> targets;
    std::unordered_set<UserId> visited;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const UserId uid : users)
        {
            if (!visited.insert(uid).second)
                continue;
            auto it = sessions_.find(uid);
            if (it == sessions_.end())
                continue;
            auto &bucket = it->second;
            std::erase_if(bucket, [](const Locator &item)
                          { return item.session.expired(); });
            if (bucket.empty())
            {
                sessions_.erase(it);
                continue;
            }
            targets.insert(targets.end(), bucket.begin(), bucket.end());
        }
    }

    auto frame = std::make_shared<const std::string>(std::move(encoded));
    size_t accepted = 0;
    for (const auto &target : targets)
    {
        if (reactorGroup_->postOutbound(
                target.reactorIndex,
                target.key.fd,
                target.key.connId,
                OutboundTask::sharedEncoded(frame)) == EnqueueResult::Ok)
        {
            ++accepted;
        }
    }
    return accepted;
}

// ---- 在线计数：统计各账号仍存活的设备连接 ----
size_t WebSocketSessionManager::onlineCount() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::size_t count = 0;
    for (const auto &[_, bucket] : sessions_)
        count += static_cast<std::size_t>(std::count_if(
            bucket.begin(), bucket.end(), [](const Locator &item)
            { return !item.session.expired(); }));
    return count;
}

size_t WebSocketSessionManager::connectionCount(UserId uid) const
{
    std::lock_guard<std::mutex> lock(mtx_);
    const auto found = sessions_.find(uid);
    if (found == sessions_.end())
        return 0;
    return static_cast<std::size_t>(std::count_if(
        found->second.begin(), found->second.end(), [](const Locator &item)
        { return !item.session.expired(); }));
}
