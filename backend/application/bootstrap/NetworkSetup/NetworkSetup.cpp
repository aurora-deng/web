#include "NetworkSetup.h"
#include "backend/application/bootstrap/BootstrapSettings/BootstrapSettings.h"
#include "backend/server/Runtime/ServerRuntime.h"
#include <cerrno>
#include <iostream>
namespace webserver::bootstrap {
void configureNetwork(ServerRuntime&server,bool productionMode){
        std::cout << "worker pools: http=" << server.httpWorkerCount()
                  << ", websocket=" << server.webSocketWorkerCount() << '\n';
        server.setMaxConnections(
            size("WEB_MAX_CONNECTIONS", 10'000, 1, 1'000'000));
        server.setShutdownDrain(std::chrono::milliseconds(
            size("WEB_SHUTDOWN_DRAIN_MS", 500, 0, 30'000)));
        if (const char *rawPort = std::getenv("WEB_SERVER_PORT")) {
            char *end = nullptr;
            errno = 0;
            const long value = std::strtol(rawPort, &end, 10);
            if (errno || end == rawPort || *end != '\0' || value < 1 || value > 65535)
                throw std::invalid_argument("WEB_SERVER_PORT must be in [1, 65535]");
            server.setPort(static_cast<int>(value));
        }
        // 同时设置证书和私钥才开启独立 HTTPS 端口。ALPN 在此端口选 h2/http/1.1；
        // 8080 明文入口仍保留 HTTP/1.1 与 prior-knowledge h2c。
        if (const char *cert = std::getenv("WEB_TLS_CERT")) {
            const char *key = std::getenv("WEB_TLS_KEY");
            if (!key)
                throw std::invalid_argument("WEB_TLS_KEY is required with WEB_TLS_CERT");
            int tlsPort = 8443;
            if (const char *rawPort = std::getenv("WEB_TLS_PORT")) {
                char *end = nullptr;
                errno = 0;
                const long value = std::strtol(rawPort, &end, 10);
                if (errno || end == rawPort || *end != '\0' || value < 1 || value > 65535)
                    throw std::invalid_argument("WEB_TLS_PORT must be in [1, 65535]");
                tlsPort = static_cast<int>(value);
            }
            server.setTls(cert, key, tlsPort);
        } else if (std::getenv("WEB_TLS_KEY")) {
            throw std::invalid_argument("WEB_TLS_CERT is required with WEB_TLS_KEY");
        }
        if (productionMode && !std::getenv("WEB_TLS_CERT"))
            throw std::invalid_argument(
                "WEB_PRODUCTION_MODE requires WEB_TLS_CERT and WEB_TLS_KEY");
        if (const char *raw = std::getenv("WEB_SERVER_REACTORS"))
        {
            char *end = nullptr;
            errno = 0;
            const unsigned long count = std::strtoul(raw, &end, 10);
            if (errno != 0 || end == raw || *end != '\0' ||
                count == 0 || count > 32)
                throw std::invalid_argument(
                    "WEB_SERVER_REACTORS must be an integer in [1, 32]");
            server.setReactorCount(static_cast<size_t>(count));
        }

}
}
