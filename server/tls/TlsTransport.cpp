#include "server/tls/TlsTransport.h"

#include <stdexcept>
#include <utility>

TlsTransport::TlsTransport(std::shared_ptr<TlsContext> context, int fd)
    : context_(std::move(context)), ssl_(SSL_new(context_->get())) {
    // fd 仍归 Connection/fd_close 独占。BIO_NOCLOSE 避免 SSL_free 在 fd 复用后
    // 二次 close 新连接；SSL 对象只拥有这个 BIO，不拥有底层 socket。
    BIO *socketBio = ssl_ ? BIO_new_socket(fd, BIO_NOCLOSE) : nullptr;
    if (!ssl_ || !socketBio) {
        if (socketBio) BIO_free(socketBio);
        if (ssl_) SSL_free(ssl_);
        ssl_ = nullptr;
        throw std::runtime_error("could not create TLS connection");
    }
    SSL_set_bio(ssl_, socketBio, socketBio);
    SSL_set_accept_state(ssl_);
}

TlsTransport::~TlsTransport() { SSL_free(ssl_); }

TlsIo TlsTransport::classify(int result) {
    switch (SSL_get_error(ssl_, result)) {
    case SSL_ERROR_WANT_READ: return TlsIo::WantRead;
    case SSL_ERROR_WANT_WRITE: return TlsIo::WantWrite;
    case SSL_ERROR_ZERO_RETURN: return TlsIo::Closed;
    default: return TlsIo::Error;
    }
}

TlsIo TlsTransport::handshake() {
    const int result = SSL_accept(ssl_);
    if (result == 1) {
        established_ = true;
        return TlsIo::Done;
    }
    return classify(result);
}

TlsIo TlsTransport::read(char *buffer, std::size_t capacity,
                         std::size_t &received) {
    received = 0;
    const int result = SSL_read_ex(ssl_, buffer, capacity, &received);
    if (result == 1) {
        readWantsWrite_ = false;
        return TlsIo::Done;
    }
    const auto state = classify(result);
    readWantsWrite_ = state == TlsIo::WantWrite;
    return state;
}

TlsIo TlsTransport::write(const char *bytes, std::size_t length,
                          std::size_t &accepted) {
    accepted = 0;
    if (pendingWrite_.empty())
        pendingWrite_.assign(bytes, length);
    // 不启用 PARTIAL_WRITE。成功才推进上层队列游标；失败时保留原字节重试。
    std::size_t written = 0;
    const int result = SSL_write_ex(ssl_, pendingWrite_.data(),
                                    pendingWrite_.size(), &written);
    if (result == 1) {
        accepted = written;
        pendingWrite_.clear();
        writeWantsRead_ = writeWantsWrite_ = false;
        return TlsIo::Done;
    }
    const auto state = classify(result);
    writeWantsRead_ = state == TlsIo::WantRead;
    writeWantsWrite_ = state == TlsIo::WantWrite;
    return state;
}

std::string_view TlsTransport::alpn() const {
    const unsigned char *bytes = nullptr;
    unsigned int length = 0;
    SSL_get0_alpn_selected(ssl_, &bytes, &length);
    if (!bytes)
        return {};
    return {reinterpret_cast<const char *>(bytes), length};
}

void TlsTransport::bestEffortShutdown() noexcept {
    if (ssl_ && established_ && pendingWrite_.empty())
        (void)SSL_shutdown(ssl_);
}
