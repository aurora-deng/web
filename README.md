# web-test 2.0 · test9.0

这是一个面向 Linux 的 C++20 高级 Web 服务器学习项目。test9.0 以
[test8.0](https://github.com/aurora-deng/web/tree/test8.0) 的 HTTP/2、TLS/ALPN
和 stream 协程为基础，加入 gRPC、跨协议身份与资源保护、SSE 断线重放、运行指标和
协议级优雅停机，并把 gRPC 服务从同步 Service API 升级为 Callback API。

当前代码已经过 Linux 全量功能测试和 Sanitizer 检查，但仍是学习与预生产基线，不能据此
直接宣称适合公网、多实例关键业务。具体缺口见
[Phase 9 的生产适用性说明](docs/architecture/phase9.md#10-生产适用性与仍缺少的条件)。

## 相对 test8.0 的主要更新

| 维度 | test8.0 | test9.0 |
|---|---|---|
| gRPC | 无 | 官方 gRPC C++ 独立监听器，.proto 生成代码，覆盖 unary、server streaming、client streaming、bidirectional streaming |
| gRPC 执行模型 | 无 | CallbackService + 每 RPC Reactor；流等待时归还执行线程 |
| 身份 | 各协议入口主要依赖原始参数 | HTTP、WebSocket、SSE、gRPC 共用带过期时间和租户的 HMAC 身份 |
| 浏览器边界 | 缺少统一 Origin 策略 | WebSocket、SSE 和 Cookie 写请求执行精确 Origin 白名单检查 |
| SSE 恢复 | 连接断开后只重新订阅 | 有界事件历史、Last-Event-ID 重放和历史 gap 通知 |
| 资源保护 | 连接及出站队列等基础边界 | 增加身份限流、SSE 订阅/历史、gRPC 并发/消息/总量预算和 SegmentPool 缓存上限 |
| 停机 | 停止监听和运行线程 | HTTP/2 GOAWAY、WebSocket 1001、SSE shutdown 事件以及有界 drain |
| 观测 | 分散指标 | liveness、readiness、Prometheus 指标和跨协议 request-id |
| 验证 | HTTP/2、TLS 和既有协议测试 | Linux Debug 与 ASan/LSan/UBSan 全量测试，另含 gRPC 明文/TLS 真实 socket 集成 |

## 当前架构

```text
TCP accept
  ├─ 明文入口
  │    ├─ HTTP/2 client preface → Http2Session → nghttp2 → stream coroutine
  │    └─ HTTP/1.1 → Router
  │                    ├─ 普通 HTTP
  │                    ├─ WebSocket Session
  │                    └─ SSE Session + ReplayBuffer
  └─ TLS 入口 → OpenSSL handshake → ALPN
                     ├─ h2       → Http2Session
                     └─ http/1.1 → HttpSession

独立 gRPC listener
  → gRPC Core 管理 HTTP/2 / HPACK / flow control
  → CallbackService
  → EchoReactor / CountReactor / UploadReactor / ChatReactor
  → CallbackCall 统一认证、配额、request-id、取消和指标
```

Web 入口仍采用“连接归所属 Reactor、业务进入 Executor、出站由单写者发送”的所有权模型。
gRPC 不复用项目手写的 Http2Session，因为官方 gRPC Core 需要完整拥有自己的 HTTP/2、
metadata、trailers、deadline 和流控状态；二者在业务与身份层汇合，而不共同修改协议状态。

## Callback API 如何工作

同步流式 handler 像一名服务员一直站在桌边等待；Callback Reactor 记录桌号，只在读完成、
写完成、定时器到期或取消时回来处理一步。因此，一条等待中的长流仍占用有限状态，但不会
永久绑住一条业务线程。

| RPC | Reactor 流程 |
|---|---|
| Echo | 填充 reply → Finish() → OnDone() |
| Count | StartWrite() → OnWriteDone() → grpc::Alarm → 下一次写 |
| Upload | StartRead() → OnReadDone() 累计 → 客户端半关闭后返回 summary |
| Chat | StartRead() → OnReadDone() → StartWrite() → OnWriteDone() → 下一次读 |

所有消息缓冲都由 Reactor 持有，至少存活到对应完成回调。OnCancel 与读写回调可能并发，
所以每个流式 Reactor 都保护终态并保证只执行一次 Finish()；对象只在 OnDone() 删除。
详细说明与代码索引见 [Phase 9](docs/architecture/phase9.md#6-grpc-四种调用形态)。

## 构建

目标环境需要 Linux、C++20、CMake、OpenSSL、nghttp2、Protobuf 和 gRPC C++。Web TLS 与
gRPC 必须链接同一套 OpenSSL；本项目验证环境中的 gRPC 使用
gRPC_SSL_PROVIDER=package 构建，并安装在独立前缀。

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DWEBSERVER_ENABLE_GRPC=ON \
  -DCMAKE_PREFIX_PATH="$HOME/.local-grpc-systemssl" \
  -DOPENSSL_ROOT_DIR=/usr \
  -DOPENSSL_INCLUDE_DIR=/usr/include \
  -DOPENSSL_SSL_LIBRARY=/usr/lib64/libssl.so \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib64/libcrypto.so \
  -DOPENSSL_USE_STATIC_LIBS=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

上面的 OpenSSL 路径是本项目 CentOS Stream 9 验证环境的路径。Debian/Ubuntu 的库通常位于
`/usr/lib/x86_64-linux-gnu`，应改成该系统中 `libssl.so` 与 `libcrypto.so` 的实际位置。
显式指定四个路径的目的是防止 `$HOME/.local` 中遗留的旧 OpenSSL/BoringSSL 与系统头文件
混用。配置后可执行 `cmake -LA -N build | grep OPENSSL` 检查：include、ssl、crypto 必须来自
同一套安装。CMake 还会在配置阶段检查 `SSL_read_ex` 和 `SSL_write_ex`；检查失败时不要继续
执行构建或 CTest，先修正 OpenSSL 路径后重新运行配置命令即可，无须删除整个源码目录。

默认 Web 明文端口是 8080。设置 WEB_TLS_CERT 与 WEB_TLS_KEY 后启用 TLS，默认端口
是 8443。gRPC 默认监听 127.0.0.1:50051，可用 WEB_GRPC_ADDRESS=off 关闭。

生产模式至少要求：

```bash
export WEB_PRODUCTION_MODE=1
export WEB_AUTH_SECRET='由 secret manager 注入的至少 32 字节随机密钥'
export WEB_ALLOWED_ORIGINS='https://app.example.com'
export WEB_TLS_CERT=/run/secrets/web-cert.pem
export WEB_TLS_KEY=/run/secrets/web-key.pem
export WEB_GRPC_TLS_CERT=/run/secrets/grpc-cert.pem
export WEB_GRPC_TLS_KEY=/run/secrets/grpc-key.pem
./build/webserver
```

缺少安全配置时，生产模式会在开始监听前失败关闭。密钥、证书和私钥都不应提交到仓库。

## 验证状态

CentOS Stream 9 虚拟机使用 GCC 11.5、OpenSSL 3.5.5、libnghttp2 1.43.0 和
gRPC C++ 1.82.0 完成了以下检查：

- Debug 全量构建与 CTest：80/80 通过，25.41 秒。
- ASan、LeakSanitizer、UBSan 全量 CTest：80/80 通过，30.43 秒。
- HTTP、TLS、WebSocket、SSE、HTTP/2 与优雅停机真实 socket 黑盒。
- gRPC 四种 RPC、认证 metadata、request-id、配额、deadline/取消以及 TLS + ALPN h2。
- Callback 专项：Core 线程预算设为 1，一条 Count 流在 1 秒 Alarm 等待时，并发 Echo 仍在
  500 ms 门限内完成；两条活跃流占满业务配额后，第三个 RPC 得到 RESOURCE_EXHAUSTED。
- Sanitizer 首轮发现并修复 Count 定时器的 1080 字节引用环泄漏：Alarm 回调使用
  weak_ptr，既打破 state/alarm/callback 环，又在回调执行期间保持状态存活。

最终测试数字与发现过的问题记录在
[Phase 9 验证记录](docs/architecture/phase9.md#11-验证记录)。

## 学习入口

1. [Phase 5：SSE](docs/architecture/phase5.md)
2. [Phase 6：HTTP/2、nghttp2 与手写教学版](docs/architecture/phase6.md)
3. [Phase 7：TLS/ALPN 与教学版](docs/architecture/phase7.md)
4. [Phase 8：gRPC 协议语义与消息封装](docs/architecture/phase8.md)
5. [Phase 9：生产化护栏与 Callback Reactor](docs/architecture/phase9.md)
6. [文档总导航](docs/README.md)
