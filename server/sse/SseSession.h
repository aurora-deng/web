#pragma once

#include "server/session/Session/Session.h"
#include "server/sse/SseEvent.h"
#include "server/transport/ConnectionKey.h"
#include "server/transport/OutboundTask.h"

#include <memory>
#include <string>

class Connection;
class SseSessionManager;
class SubReactor;

/** HTTP 握手完成后接管连接的 SSE 长会话。 */
class SseSession : public Session,
                   public std::enable_shared_from_this<SseSession>
{
public:
    SseSession(ConnectionKey key,
               SubReactor *reactor,
               SseClientId clientId,
               SseSessionManager *manager,
               std::string lastEventId = {});

    Task<void> run() override;
    bool onTimeout() override;
    void onClose() override;
    void beginDrain() noexcept override;

    SseClientId clientId() const noexcept { return clientId_; }

private:
    Connection *getConn();
    EnqueueResult enqueuePayload(std::string payload);
    EnqueueResult enqueueEvent(const SseEvent &event);
    void unregisterIfNeeded();

    ConnectionKey key_{};
    SubReactor *reactor_ = nullptr;
    SseClientId clientId_ = 0;
    SseSessionManager *manager_ = nullptr;
    std::string lastEventId_;
    bool registered_ = false;
};
