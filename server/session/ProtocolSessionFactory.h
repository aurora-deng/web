#pragma once

#include "server/session/SessionFactory.h"

class Executor;
class SseSessionManager;
class WebSocketDispatcher;
class WebSocketSessionManager;

/** Runtime 的统一协议会话装配台，集中创建 WebSocket 与 SSE 会话。 */
class ProtocolSessionFactory : public SessionFactory
{
public:
    ProtocolSessionFactory(WebSocketSessionManager &wsManager,
                           WebSocketDispatcher &wsDispatcher,
                           Executor &wsExecutor,
                           SseSessionManager &sseManager);

    std::shared_ptr<Session> createWebSocketSession(
        ConnectionKey key, SubReactor *reactor, UserId uid) override;
    std::shared_ptr<Session> createSseSession(
        ConnectionKey key,
        SubReactor *reactor,
        std::uint64_t clientId) override;
    std::shared_ptr<Session> createHttp2Session(
        ConnectionKey key, SubReactor *reactor) override;

private:
    WebSocketSessionManager &wsManager_;
    WebSocketDispatcher &wsDispatcher_;
    Executor &wsExecutor_;
    SseSessionManager &sseManager_;
};
