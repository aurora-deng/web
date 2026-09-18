#pragma once

#include "server/tls/TlsContext.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

// OpenSSL 每次 I/O 都可能要求等读或等写，不能把 WANT_* 当成断线。
enum class TlsIo { Done, WantRead, WantWrite, Closed, Error };

// 每条连接一个 SSL 对象，只在所属 SubReactor 线程使用。
class TlsTransport {
public:
    TlsTransport(std::shared_ptr<TlsContext> context, int fd);
    ~TlsTransport();
    TlsTransport(const TlsTransport &) = delete;
    TlsTransport &operator=(const TlsTransport &) = delete;

    TlsIo handshake();
    TlsIo read(char *buffer, std::size_t capacity, std::size_t &received);
    TlsIo write(const char *bytes, std::size_t length, std::size_t &accepted);
    std::string_view alpn() const;
    bool established() const noexcept { return established_; }
    bool readWantsWrite() const noexcept { return readWantsWrite_; }
    bool writeWantsRead() const noexcept { return writeWantsRead_; }
    bool writeWantsWrite() const noexcept { return writeWantsWrite_; }
    void bestEffortShutdown() noexcept;

private:
    TlsIo classify(int result);
    std::shared_ptr<TlsContext> context_;
    SSL *ssl_ = nullptr;
    bool established_ = false;
    bool readWantsWrite_ = false;
    bool writeWantsRead_ = false;
    bool writeWantsWrite_ = false;
    // SSL_write_ex 的 WANT_* 重试必须使用相同字节和长度；保留稳定缓冲直到成功。
    std::string pendingWrite_;
};
