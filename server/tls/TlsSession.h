#pragma once

#include "server/session/Session/Session.h"
#include "server/transport/ConnectionKey.h"

class SubReactor;

// TLS 握手只在所属 Reactor 协程中推进；ALPN 决定握手后的 HTTP Session 类型。
class TlsSession final : public Session {
public:
    TlsSession(ConnectionKey key, SubReactor *reactor)
        : key_(key), reactor_(reactor) {}
    Task<void> run() override;

private:
    ConnectionKey key_;
    SubReactor *reactor_;
};
