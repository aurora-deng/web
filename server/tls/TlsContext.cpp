#include "server/tls/TlsContext.h"

#include <stdexcept>
#include <cstring>
#include <string_view>

namespace {
// ALPN 是长度前缀字节串，不是逗号分隔文本。服务端优先选 h2。
int chooseAlpn(SSL *, const unsigned char **selected, unsigned char *length,
               const unsigned char *offered, unsigned int offeredLength, void *) {
    for (std::string_view candidate : {"h2", "http/1.1"}) {
        unsigned int i = 0;
        while (i < offeredLength) {
            const auto n = offered[i++];
            if (n == 0 || n > offeredLength - i)
                return SSL_TLSEXT_ERR_ALERT_FATAL;
            if (n == candidate.size() &&
                std::memcmp(offered + i, candidate.data(), n) == 0) {
                *selected = offered + i;
                *length = n;
                return SSL_TLSEXT_ERR_OK;
            }
            i += n;
        }
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}
} // namespace

std::shared_ptr<TlsContext> TlsContext::create(const std::string &certificate,
                                                const std::string &privateKey) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx)
        throw std::runtime_error("SSL_CTX_new failed");
    // 教学阶段只提供 TLS 1.3：省去 HTTP/2 over TLS 1.2 的禁用密码套件配置。
    const bool valid = SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) == 1 &&
        SSL_CTX_use_certificate_chain_file(ctx, certificate.c_str()) == 1 &&
        SSL_CTX_use_PrivateKey_file(ctx, privateKey.c_str(), SSL_FILETYPE_PEM) == 1 &&
        SSL_CTX_check_private_key(ctx) == 1;
    if (!valid) {
        SSL_CTX_free(ctx);
        throw std::runtime_error("TLS certificate or private key configuration failed");
    }
    SSL_CTX_set_alpn_select_cb(ctx, chooseAlpn, nullptr);
    return std::shared_ptr<TlsContext>(new TlsContext(ctx));
}

TlsContext::~TlsContext() { SSL_CTX_free(context_); }
