# web-test 2.0 · test8.0

这是一个面向 Linux 的 C++20 Web 服务器学习项目。本分支从 [`test7.0`](https://github.com/aurora-deng/web/tree/test7.0)（提交 `067d34ce91333ca0ba995bad10882cbac9c41d4e`）继续：保留 HTTP/1.1、WebSocket、SSE 和原有 Reactor/协程/统一出站架构，新增进程内 HTTP/2 与可选的 TLS/ALPN 入口。HTTP/2 的协议复杂部分交给 nghttp2；另有一套独立的手写教学程序供逐步学习。

## 本分支相对 test7.0 的主要更新

| 维度 | test7.0 | test8.0 |
|---|---|---|
| 明文入口 | HTTP/1.1、WebSocket、SSE | 同一端口增添 HTTP/2 prior knowledge：首包匹配客户端前言后交给 `Http2Session`，否则走原 HTTP/1.1 |
| HTTP/2 协议 | 无 | `Http2Codec` 封装 nghttp2，处理帧、HPACK、SETTINGS、stream 与流量控制；`Http2Session` 把完整请求送进现有 Router/Executor |
| HTTPS 入口 | 无 | 配置证书和私钥后开启独立 TLS 监听端口；OpenSSL 完成 TLS 1.3 握手，ALPN 选 `h2` 或 `http/1.1` |
| 连接读写 | 明文 `recv/writev/sendfile` | TLS 连接先解密再交协议 Session；出站经 `SSL_write_ex` 加密，文件响应经 `pread` 分块读取 |
| 学习代码 | SSE 与 WebSocket 分层示例 | 增加独立的手写 HTTP/2 完整交换示例，以及 TLS 记录拆包和 OpenSSL 内存双端示例 |
| 验证 | 原 HTTP/WS/SSE 测试 | 增加 HTTP/2 codec 往返测试、TLS/ALPN Linux 黑盒脚本；本次 Windows 验证结果见下文 |

这次升级没有另建一套业务服务器。新协议最终仍汇入同一个 Router、HTTP Executor、OutboundQueue 与单写者 `TransportWriter`；`fd + connId` 仍用来防止旧连接任务误投到复用的 fd。

## 请求如何经过服务器

```text
TCP accept
  ├─ 明文端口 → HttpSession 检查 h2 客户端前言
  │               ├─ 匹配 → Http2Session
  │               └─ 其他 → HTTP/1.1；需要时交接 WebSocket/SSE Session
  └─ TLS 端口 → TlsSession 握手 → ALPN
                  ├─ h2       → Http2Session
                  └─ http/1.1 → HttpSession

Http2Session → Http2Codec/nghttp2 → HttpRequest → Router → HTTP Executor
             → HttpResponse → nghttp2 HEADERS/DATA → OutboundQueue
             → 唯一 writerLoop → 明文 socket 或 TlsTransport → TCP
```

可以把 ALPN 理解为进门时选定“说哪种 HTTP 语言”的牌子；nghttp2 才是真正拆解 HTTP/2 帧和 HPACK 首部的人。TLS 不替代 HTTP 解析，HTTP/2 也不替代 TLS 加密。明文端口的前言探测是本项目的协议选择设计；HTTPS 端口依据 ALPN，不能靠密文字节猜 HTTP 版本。

HTTP/2 当前支持普通、有界的请求与响应，同一连接的多个 stream 可以独立完成。它暂不把 SSE 长流或 WebSocket 升级映射到 HTTP/2 stream；命中这类路由时返回 501，使用这些功能请走 HTTP/1.1。大文件/流式响应、完整的 stream 公平调度也尚未接入 HTTP/2。TLS 仅配置 TLS 1.3、单证书和可选独立端口；仍需在目标 Linux 上做真实端到端验收。

## 学习入口与代码索引

1. [Phase 5：SSE](docs/architecture/phase5.md) 是 `test7.0` 的基线，先回顾 HTTP 长响应如何交接给 Session。
2. [Phase 6：HTTP/2 接入、理论与手写教学版](docs/architecture/phase6.md) 对照 [生产版 codec](server/http2/Http2Codec.cpp)、[生产版 session](server/http2/Http2Session.cpp) 和 [独立教学版](server/http2/learn/README.md)。教学版从 24 字节前言、SETTINGS、9 字节帧头、HPACK、stream、窗口一直走到 GOAWAY，不依赖 nghttp2 或网络 socket。
3. [Phase 7：TLS/ALPN 接入](docs/architecture/phase7.md) 对照 [TLS 上下文](server/tls/TlsContext.cpp)、[传输包装](server/tls/TlsTransport.cpp)、[握手会话](server/tls/TlsSession.cpp) 和 [TLS 教学版](server/tls/learn/README.md)。
4. [文档导航](docs/README.md) 收录 WebSocket 旧课程、测试、性能和运维说明。

核心运行路径仍在 `server/Runtime`、`server/Reactor`、`server/SubReactor`、`server/http`、`server/websocket`、`server/sse`、`server/transport`；新增内容主要在 `server/http2` 和 `server/tls`。测试入口见 [tests/README.md](tests/README.md)。

## 构建与运行

目标环境为 Linux，需 C++20、CMake 3.16+、OpenSSL 开发库及 nghttp2 开发库；运行全部单元测试还需 GoogleTest 与 Python 3。例如 Debian/Ubuntu：

```bash
sudo apt install build-essential cmake libssl-dev libnghttp2-dev libgtest-dev python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/webserver
```

默认明文端口为 8080；`WEB_SERVER_PORT` 可覆盖。仅在同时设置 `WEB_TLS_CERT` 和 `WEB_TLS_KEY` 时开启 HTTPS，默认 8443，`WEB_TLS_PORT` 可覆盖。证书与私钥由运行环境提供，不提交进仓库。以下命令只用于本地演示：

```bash
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -keyout /tmp/web-key.pem -out /tmp/web-cert.pem -subj '/CN=localhost'
WEB_TLS_CERT=/tmp/web-cert.pem WEB_TLS_KEY=/tmp/web-key.pem ./build/webserver
curl --http2-prior-knowledge http://127.0.0.1:8080/
curl -k --http2 https://127.0.0.1:8443/
curl -k --http1.1 https://127.0.0.1:8443/
```

`-k` 仅供上述自签名证书的本地演示。SSE 的 `/events`、WebSocket 的 `/ws` 应使用 HTTP/1.1。详细测试方法见 Phase 6/7。

## 当前验证状态

在本次 Windows 环境中实际通过：

- Python WebSocket 可靠接收端：5 个用例。
- 手写 HTTP/2 教学程序：前言拆分、帧、HPACK 静态/动态表、两个并发 stream、窗口、PING、RST_STREAM、GOAWAY。
- 生产版 `Http2Codec` 与本地编译的 nghttp2 静态库的往返测试：前言拆包、HPACK、多路请求、POST DATA、RST_STREAM。
- SSE 编码独立检查：多行数据、HTTP chunk、注释心跳和字段注入防护。
- TLS 外层记录增量解析教学程序；项目 Python 文件语法检查。
- 全部 `server/` 源码及 HTTP/2 codec 测试源码的 Cppcheck warning 扫描；其中发现并修复了预留路由 ID 未初始化的问题。

完整服务器依赖 Linux epoll，本机也没有 OpenSSL 开发头文件或 GoogleTest，因此 **Linux 全量构建、GTest 套件、TLS/ALPN 真正握手与 HTTP/WS/SSE/TLS 网络黑盒尚未在此环境运行**。这些验证仍需在虚拟机中执行上面的 CTest 命令；不能把独立组件通过解释为完整服务已经通过。
