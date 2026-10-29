// 独立教学程序：两端在内存 BIO 中完成真正的 TLS 1.3 握手。
// 不实现密码学；OpenSSL 负责记录层、证书、密钥协商和加密，本程序手动推进
// 握手状态、解析 ALPN 长度前缀列表，并观察加密前后的应用字节。
#include <openssl/ssl.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {
using Context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using Peer = std::unique_ptr<SSL, decltype(&SSL_free)>;

int selectAlpn(SSL *, const unsigned char **out, unsigned char *outLength,
               const unsigned char *wire, unsigned int wireLength, void *) {
    // ALPN 列表是 [长度][协议字节]...，不能按 C 字符串或逗号切割。
    for (std::string_view wanted : {"h2", "http/1.1"}) {
        unsigned int cursor = 0;
        while (cursor < wireLength) {
            const unsigned int n = wire[cursor++];
            if (n == 0 || n > wireLength - cursor)
                return SSL_TLSEXT_ERR_ALERT_FATAL;
            if (n == wanted.size() &&
                std::memcmp(wire + cursor, wanted.data(), n) == 0) {
                *out = wire + cursor;
                *outLength = static_cast<unsigned char>(n);
                return SSL_TLSEXT_ERR_OK;
            }
            cursor += n;
        }
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

bool step(SSL *peer, bool &done) {
    if (done) return true;
    const int result = SSL_do_handshake(peer);
    if (result == 1) {
        done = true;
        return true;
    }
    const int error = SSL_get_error(peer, result);
    return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE;
}

std::string_view selected(SSL *peer) {
    const unsigned char *bytes = nullptr;
    unsigned int length = 0;
    SSL_get0_alpn_selected(peer, &bytes, &length);
    return bytes ? std::string_view(reinterpret_cast<const char *>(bytes), length)
                 : std::string_view{};
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: tls_learn cert.pem key.pem [--h1]\n";
        return 2;
    }
    try {
        Context serverCtx(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
        Context clientCtx(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
        if (!serverCtx || !clientCtx ||
            SSL_CTX_set_min_proto_version(serverCtx.get(), TLS1_3_VERSION) != 1 ||
            SSL_CTX_set_min_proto_version(clientCtx.get(), TLS1_3_VERSION) != 1 ||
            SSL_CTX_use_certificate_chain_file(serverCtx.get(), argv[1]) != 1 ||
            SSL_CTX_use_PrivateKey_file(serverCtx.get(), argv[2], SSL_FILETYPE_PEM) != 1)
            throw std::runtime_error("context or certificate setup failed");
        SSL_CTX_set_alpn_select_cb(serverCtx.get(), selectAlpn, nullptr);
        // 只为自签名证书的本地教学演示；真实 TLS 客户端必须验证证书和主机名。
        SSL_CTX_set_verify(clientCtx.get(), SSL_VERIFY_NONE, nullptr);

        Peer client(SSL_new(clientCtx.get()), SSL_free);
        Peer server(SSL_new(serverCtx.get()), SSL_free);
        if (!client || !server)
            throw std::runtime_error("SSL_new failed");
        const unsigned char both[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        const unsigned char h1[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        const bool onlyH1 = argc == 4 && std::string_view(argv[3]) == "--h1";
        if (SSL_set_alpn_protos(client.get(), onlyH1 ? h1 : both,
                                onlyH1 ? sizeof(h1) : sizeof(both)) != 0)
            throw std::runtime_error("setting client ALPN failed");

        BIO *clientBio = nullptr, *serverBio = nullptr;
        if (BIO_new_bio_pair(&clientBio, 0, &serverBio, 0) != 1)
            throw std::runtime_error("BIO pair failed");
        // 两个 BIO 在内存里相连，代替网络 socket；SSL_set_bio 接管所有权。
        SSL_set_bio(client.get(), clientBio, clientBio);
        SSL_set_bio(server.get(), serverBio, serverBio);
        SSL_set_connect_state(client.get());
        SSL_set_accept_state(server.get());

        bool clientDone = false, serverDone = false;
        int rounds = 0;
        for (; rounds < 1000 && !(clientDone && serverDone); ++rounds) {
            if (!step(client.get(), clientDone) || !step(server.get(), serverDone))
                throw std::runtime_error("TLS handshake failed");
        }
        if (!clientDone || !serverDone || selected(client.get()) != selected(server.get()))
            throw std::runtime_error("TLS/ALPN handshake did not finish");
        std::cout << "TLS=" << SSL_get_version(client.get())
                  << " ALPN=" << selected(client.get())
                  << " handshake_rounds=" << rounds << '\n';

        constexpr std::string_view message = "HTTP application bytes";
        std::size_t written = 0;
        if (SSL_write_ex(client.get(), message.data(), message.size(), &written) != 1 ||
            written != message.size())
            throw std::runtime_error("client TLS write failed");
        char buffer[128]{};
        std::size_t received = 0;
        if (SSL_read_ex(server.get(), buffer, sizeof(buffer), &received) != 1 ||
            std::string_view(buffer, received) != message)
            throw std::runtime_error("server TLS read failed");
        std::cout << "decrypted_payload=" << std::string_view(buffer, received) << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
