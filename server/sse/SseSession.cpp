#include "server/sse/SseSession.h"

#include "server/CoroutineScheduler/AWaiter.h"
#include "server/SubReactor/SubReactor.h"
#include "server/sse/SseCodec.h"
#include "server/sse/SseSessionManager.h"

SseSession::SseSession(ConnectionKey key,
                       SubReactor *reactor,
                       SseClientId clientId,
                       SseSessionManager *manager)
    : key_(key), reactor_(reactor), clientId_(clientId), manager_(manager)
{
}

Connection *SseSession::getConn()
{
    return reactor_ ? reactor_->findConnection(key_.fd, key_.connId) : nullptr;
}

EnqueueResult SseSession::enqueuePayload(std::string payload)
{
    if (payload.empty() || !getConn())
        return EnqueueResult::Closed;
    return reactor_->enqueueOutbound(
        key_.fd,
        OutboundTask::encoded(SseCodec::encodeChunk(payload)));
}

EnqueueResult SseSession::enqueueEvent(const SseEvent &event)
{
    return enqueuePayload(SseCodec::encodeEvent(event));
}

void SseSession::unregisterIfNeeded()
{
    if (!registered_ || !manager_)
        return;
    manager_->unregister(clientId_, key_);
    registered_ = false;
}

void SseSession::onClose()
{
    unregisterIfNeeded();
}

bool SseSession::onTimeout()
{
    if (!getConn())
        return true;
    if (enqueuePayload(SseCodec::encodeComment("heartbeat")) != EnqueueResult::Ok)
        return true;
    reactor_->refreshIdleTimer(key_.fd);
    return false;
}

Task<void> SseSession::run()
{
    auto *conn = getConn();
    if (!conn)
        co_return;

    if (manager_)
    {
        registered_ = manager_->registerSession(
            clientId_, shared_from_this(), reactor_->reactorIndex(), key_);
    }
    if (!registered_)
    {
        reactor_->fd_close(key_.fd, "sse registration failed", CoroutineRole::Main);
        co_return;
    }

    SseEvent ready;
    ready.eventName = "ready";
    ready.data = "connected";
    ready.retryMilliseconds = 3000;
    if (enqueueEvent(ready) != EnqueueResult::Ok)
    {
        reactor_->fd_close(key_.fd, "sse ready event rejected", CoroutineRole::Main);
        co_return;
    }

    while (true)
    {
        conn = getConn();
        if (!conn || conn->state.closed)
            co_return;

        const auto readState = reactor_->recvSocket(key_.fd);
        conn = getConn();
        if (!conn || conn->state.closed)
            co_return;
        if (readState == RecvState::CLOSED || conn->state.peerClosed)
        {
            reactor_->fd_close(key_.fd, "sse peer closed", CoroutineRole::Main);
            co_return;
        }

        // SSE 是单向流；客户端意外写入的数据不参与下一轮 HTTP 解析，及时丢弃防止积压。
        if (conn->readBuffer.readableBytes() != 0)
        {
            conn->readBuffer.clear();
            conn->pendingBytes = 0;
            conn->state.pauseByMemory = false;
        }
        co_await ReadAwaiter(reactor_, key_);
    }
}
