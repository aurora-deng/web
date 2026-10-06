# web-test 2.0 · Function1.0

> Function1.0 以 [test9.1](https://github.com/aurora-deng/web/tree/test9.1) 为基线，
> 保留 Phase 10 的 HTTP/gRPC 性能路径，并加入完整的 Phase 11 应用层：账号、PostgreSQL、
> 好友、私聊/群聊、离线消息、事务 Outbox、AI Provider 和响应式网页端。完整变化见
> [Function1.0 与 test9.1 对比](docs/architecture/function1.0-vs-test9.1.md)。
>
> 当前版本已接入 PostgreSQL 18.6、libsodium 1.0.22、libcurl 8.22.0、真实账号 HTTP API、
> 好友/私聊/群聊、WebSocket 实时消息、SSE 通知与事务 Outbox。AI Provider、模型注册以及
> Ollama API 纵向链路已接通。Linux CPU 版 ONNX Runtime 1.28.0、ONNX Runtime GenAI 0.17.0
> 和进程内 Provider 也已接入，有界 Worker、模型/Adapter 缓存、模型树摘要、取消和逐 Token
> 回调均完成验证；独立 `webserver-onnx-worker` 与 `GrpcOnnxModelProvider` 也已用真实模型打通，
> 模型进程退出时普通聊天仍可继续。模型版本现在会先经真实 Runtime 试装再切换，Adapter 不兼容时保留旧 Active，
> 并支持重新校验后的显式回滚。已获准的 Phi-3 Mini 4K CPU INT4 真实模型通过 C++ smoke 和
> WebSocket → PostgreSQL → ONNX → SSE → 持久化整链黑盒；Qwen2.5-0.5B 的 Adapter-ready INT4 图、
> 独立 `.onnx_adapter`、同图 logits 对照、C++ 加载和相同整链黑盒也已通过。按要求不在 Linux
> 安装 Ollama；状态、运行方式与代码索引见
> [Phase 11](docs/architecture/phase11.md)。

这是一个面向 Linux 的 C++20 高级 Web 服务器学习项目。test9.0 以
[test8.0](https://github.com/aurora-deng/web/tree/test8.0) 的 HTTP/2、TLS/ALPN
和 stream 协程为基础，加入 gRPC、跨协议身份与资源保护、SSE 断线重放、运行指标和
协议级优雅停机，并把 gRPC 服务从同步 Service API 升级为 Callback API。test9.1 在此基础上
优化 HTTP/gRPC 热路径、共享池和指标竞争，并补充独立 gRPC 部署与可复现基准。

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

所有消息缓冲都由 Reactor 持有，至少存活到对应完成回调。OnCancel、Alarm 与读写回调可能并发，
所以每个流式 Reactor 都保护终态并保证只执行一次 Finish()；`OnDone()` 完成协议记账，
对象在 gRPC 与 Alarm 两个异步引用都释放后删除。
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

- Release 全量构建与 CTest：84/84 通过，25.98 秒。
- Debug 全量构建与 CTest：84/84 通过，26.74 秒。
- ASan、LeakSanitizer、UBSan 全量 CTest：84/84 通过，29.77 秒。
- HTTP、TLS、WebSocket、SSE、HTTP/2 与优雅停机真实 socket 黑盒。
- gRPC 四种 RPC、认证 metadata、request-id、配额、deadline/取消以及 TLS + ALPN h2。
- Callback 专项：Core 线程预算设为 1，一条 Count 流在 1 秒 Alarm 等待时，并发 Echo 仍在
  500 ms 门限内完成；两条活跃流占满业务配额后，第三个 RPC 得到 RESOURCE_EXHAUSTED。
- Sanitizer 首轮发现并修复 Count 定时器的 1080 字节引用环泄漏：Alarm 回调使用
  weak_ptr，既打破 state/alarm/callback 环，又在回调执行期间保持状态存活。
- Phase 10 同机受控五轮中位数：HTTP `/fast` 从 33,957.99 提升到 49,459.19 QPS
  （+45.65%），gRPC Echo 从 12,389.40 提升到 16,389.00 QPS（+32.28%）；
  两者错误为 0，P99 均低于基线。
- Phase 11 打开 PostgreSQL、libsodium、libcurl、gRPC 与 ONNX GenAI 后，2026-10-06 的
  Linux Release 全量 CTest 为 **93/93**（数据库契约测试使用隔离测试库）；此前 Debug、
  ASan+LSan+UBSan 两套记录均为 **92/92**。Phi-3 Mini 4K CPU INT4 真实模型
  已通过“启用前试装 + Release C++ 逐 Token” smoke；整链黑盒生成 242 个 token、1104 字节，并验证
  WebSocket Ping/Pong、SSE 完成事件与用户/AI 消息落库。Qwen Adapter 整链另生成 512 个 token、
  3315 字节，消息追踪包含模型节点、版本和 Adapter，Outbox 无积压。独立 gRPC Worker 整链另收到
  512 个 token、2424 字节，并验证 Worker 退出时 AI 受控失败而普通聊天存活；真实模型的 gRPC TLS
  流也已通过 CA/服务名校验。完整 gRPC+ONNX
  Release 二进制在相同受控配置下 `/fast` 五轮中位数为 **56,929.50 QPS**，相对 test9.1 提升
  15.10%，错误数为 0。

最终测试数字与发现过的问题记录在
[Phase 9 验证记录](docs/architecture/phase9.md#11-验证记录)。

## 学习入口

1. [Phase 5：SSE](docs/architecture/phase5.md)
2. [Phase 6：HTTP/2、nghttp2 与手写教学版](docs/architecture/phase6.md)
3. [Phase 7：TLS/ALPN 与教学版](docs/architecture/phase7.md)
4. [Phase 8：gRPC 协议语义与消息封装](docs/architecture/phase8.md)
5. [Phase 9：生产化护栏与 Callback Reactor](docs/architecture/phase9.md)
6. [test9.1：相对 test9.0 的版本差异](docs/architecture/test9.1.md)
7. [Phase 10：HTTP/gRPC 性能路径与架构拆分](docs/architecture/phase10.md)
8. [文档总导航](docs/README.md)
9. [Phase 11：账号、社交聊天与 AI 推理平台](docs/architecture/phase11.md)
10. [项目目录分层与文件放置规则](docs/architecture/project-layout.md)
11. [Function1.0：相对 test9.1 的完整差异](docs/architecture/function1.0-vs-test9.1.md)
