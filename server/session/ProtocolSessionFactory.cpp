#include "server/session/ProtocolSessionFactory.h"

#include "server/sse/SseSession.h"
#include "server/http2/Http2Session.h"
#include "server/websocket/WebSocketSession/WebSocketSession.h"

ProtocolSessionFactory::ProtocolSessionFactory(
    WebSocketSessionManager &wsManager,
    WebSocketDispatcher &wsDispatcher,
    Executor &wsExecutor,
    SseSessionManager &sseManager)
    : wsManager_(wsManager),
      wsDispatcher_(wsDispatcher),
      wsExecutor_(wsExecutor),
      sseManager_(sseManager)
{
}

std::shared_ptr<Session> ProtocolSessionFactory::createWebSocketSession(
    ConnectionKey key, SubReactor *reactor, UserId uid)
{
    return std::make_shared<WebSocketSession>(
        key, reactor, uid, &wsManager_, wsDispatcher_, wsExecutor_);
}

std::shared_ptr<Session> ProtocolSessionFactory::createSseSession(
    ConnectionKey key,
    SubReactor *reactor,
    std::uint64_t clientId)
{
    return std::make_shared<SseSession>(
        key, reactor, clientId, &sseManager_);
}

std::shared_ptr<Session> ProtocolSessionFactory::createHttp2Session(
    ConnectionKey key, SubReactor *reactor)
{
    return std::make_shared<Http2Session>(key, reactor);
}
