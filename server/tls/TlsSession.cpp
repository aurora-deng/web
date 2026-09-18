#include "server/tls/TlsSession.h"

#include "server/CoroutineScheduler/AWaiter.h"
#include "server/SubReactor/SubReactor.h"
#include "server/http/HttpSession/HttpSession.h"
#include "server/session/SessionFactory.h"
#include "server/tls/TlsTransport.h"

#include <memory>

Task<void> TlsSession::run() {
    while (true) {
        auto *conn = reactor_->findConnection(key_.fd, key_.connId);
        if (!conn || !conn->tls)
            co_return;
        const auto state = conn->tls->handshake();
        if (state == TlsIo::WantRead) {
            // 上一轮若等过可写，撤掉旧 EPOLLOUT 兴趣，避免只等读时被可写事件干扰。
            conn->state.wantWrite = false;
            co_await ReadAwaiter(reactor_, key_);
            continue;
        }
        if (state == TlsIo::WantWrite) {
            co_await WriteAwaiter(reactor_, key_);
            continue;
        }
        if (state != TlsIo::Done) {
            reactor_->fd_close(key_.fd, "TLS handshake failed", CoroutineRole::Main);
            co_return;
        }

        // TLS h2 必须依据 ALPN，不能再靠明文前言探测。HTTP/1.1 也可在 TLS 内运行。
        const auto protocol = conn->tls->alpn();
        std::shared_ptr<Session> next;
        if (protocol == "h2") {
            if (auto *factory = reactor_->sessionFactory())
                next = factory->createHttp2Session(key_, reactor_);
        } else if (protocol == "http/1.1") {
            next = std::make_shared<HttpSession>(key_, reactor_, reactor_->router());
        }
        if (!next) {
            reactor_->fd_close(key_.fd, "TLS ALPN was not h2 or http/1.1",
                               CoroutineRole::Main);
            co_return;
        }
        conn->state.wantWrite = false;
        conn->session = next;
        auto task = next->run();
        auto handle = task.release();
        conn->slot(CoroutineRole::Main).handle = handle;
        reactor_->scheduler().adopt(key_.fd, key_.connId,
                                     CoroutineRole::Main, handle, next);
        reactor_->updateEvent(key_.fd);
        co_return;
    }
}
