#pragma once

#include <memory>
#include <string>

#include <openssl/ssl.h>

// 进程级 TLS 配置。SSL_CTX 由 shared_ptr 持有，保证所有 Reactor 连接结束前不释放。
class TlsContext {
public:
    static std::shared_ptr<TlsContext> create(const std::string &certificate,
                                               const std::string &privateKey);
    ~TlsContext();
    TlsContext(const TlsContext &) = delete;
    TlsContext &operator=(const TlsContext &) = delete;

    SSL_CTX *get() const noexcept { return context_; }

private:
    explicit TlsContext(SSL_CTX *context) : context_(context) {}
    SSL_CTX *context_ = nullptr;
};
