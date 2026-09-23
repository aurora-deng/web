# Phase 9：从“协议能跑”到“可控、可观测、可恢复”

> 基线是 [Phase 8：gRPC 接入](phase8.md)。本阶段代码进入 `test9.0` 分支；本文只把实际完成项写成“已实现”，尚未完成的生产条件列在第 10 节，避免把学习项目误称为可直接公网生产部署。

## 1. 这一阶段解决什么问题

Phase 8 已能跑 HTTP/1.1、WebSocket、SSE、HTTP/2、TLS/ALPN 和 gRPC，但“能说这些协议”不等于“遇到攻击、断线、慢客户端和停机仍能守住边界”。Phase 9 增加四层护栏：

1. **可信身份**：HTTP、WebSocket 握手、SSE 握手和 gRPC 共用 HMAC-SHA256 令牌，业务身份不再直接相信 `?uid=`。
2. **有界资源**：限制连接、身份请求频率、SSE 订阅/历史、gRPC 并发、单消息、单次上传和 RPC 时长。
3. **断线恢复与优雅下线**：SSE 支持 `Last-Event-ID`；HTTP/2、WebSocket、SSE 在停机时分别发送 GOAWAY、1001 和 shutdown 事件。
4. **运行观测**：增加存活/就绪探针、Prometheus 文本指标和 gRPC request-id 回传。

可以把 Phase 8 想成一辆已经能开的车，Phase 9 是补上安全带、仪表盘、限速器和停车前的转向灯。

## 2. 相对 Phase 8 的变化

| 维度 | Phase 8 | Phase 9 |
|---|---|---|
| 身份 | Web 路径依赖查询参数；gRPC 无认证 | 同一签名令牌覆盖 HTTP/WS/SSE/gRPC，`uid` 必须与令牌一致 |
| 浏览器跨站边界 | 未校验 Origin | WebSocket/SSE 与 Cookie 写请求都做精确 Origin 白名单校验 |
| gRPC 形态 | unary `Echo`、server streaming `Count` | 增加 client streaming `Upload`、bidirectional `Chat`，四种形态齐全 |
| gRPC 生命周期 | 同步 Service，检查基础取消 | CallbackService + 每 RPC Reactor、统一 admission、并发配额、request-id、deadline/取消、大小与总量预算 |
| SSE 重连 | 断线后重新订阅，从新事件开始 | 有界历史 + `Last-Event-ID` 补发；游标过旧发送 `replay-reset` |
| 停机 | 关闭监听器并停止线程 | 先关闭监听器，再发协议下线信号并短暂排空，最后硬停 |
| 观测 | 分散的业务指标 | `/health/live`、`/health/ready`、`/metrics` 与 SSE 详细状态 |
| 启动配置 | 可在未配置安全项时对外监听 | `WEB_PRODUCTION_MODE=1` 时缺密钥、Origin 白名单或 TLS 会直接拒绝启动 |

## 3. 当前总架构

```text
TCP accept
  → 全局连接上限
  → 明文前言探测 / TLS handshake + ALPN
  → HttpSession / Http2Session
  → Router middleware
       ├─ 请求计数
       ├─ HMAC token 校验
       ├─ userId + tenant 写入 RequestContext
       ├─ Origin 白名单
       └─ 按身份固定窗口限流
  → HTTP Handler
       ├─ 普通 HttpResponse
       ├─ WebSocket Session（使用可信 userId）
       └─ SSE Session（使用可信 userId + Last-Event-ID）
  → OutboundQueue → TransportWriter → TCP/TLS

独立 gRPC listener
  → gRPC C++ HTTP/2/HPACK/flow control
  → CallbackService
  → CallbackCall：token → 并发配额 → request-id → 取消/时长
  → EchoReactor / CountReactor / UploadReactor / ChatReactor
  → grpc-status trailers
```

关键边界仍是“谁拥有状态，谁修改状态”：Web Reactor 线程拥有连接和协议 Session；Worker 只做普通 Web 业务；nghttp2 拥有普通 HTTP/2 状态；gRPC Core 拥有 gRPC 端口的 HTTP/2 状态，每个 gRPC Callback Reactor 持有自己 RPC 的消息与进度。`OnCancel` 可能和读写完成回调并发，因此流式 Reactor 用互斥状态门保证只调用一次 `Finish()`。

## 4. 统一身份与生产模式

### 4.1 令牌结构

[`AuthToken`](../../server/security/AuthToken.h) 使用：

```text
v1.userId.expiryUnixSeconds.tenant.HMAC-SHA256-hex
```

- 签名覆盖前四段，校验使用常量时间比较。
- `userId` 必须大于 0；`tenant` 只允许字母、数字、`_`、`-`，最长 64 字节。
- 当前 WebSocket/SSE 目录要求 `userId` 在所有 tenant 中全局唯一；`tenant` 用于授权。若数据库按租户重复编号，必须把会话键升级为 `(tenant,userId)` 后才能部署。
- HTTP 从 `Authorization: Bearer` 或 `Cookie: web_session=` 读取。EventSource 不能方便地自定义 Authorization，因此 Cookie 是浏览器 SSE 的实用入口。
- gRPC 从 metadata 的 `authorization` 读取同一 Bearer token。
- WebSocket/SSE 交接只使用 `RequestContext::authenticatedUserId`；兼容查询参数只保留在未启用鉴权的学习模式。

这像盖章车票：`uid` 是车票上印的座位号，签名是不可伪造的章。客户端可以展示座位号，却不能自己改号后仍通过验票。

### 4.2 启动时失败关闭

设置 `WEB_PRODUCTION_MODE=1` 后，程序要求：

- `WEB_AUTH_SECRET` 至少 32 字节；
- `WEB_ALLOWED_ORIGINS` 非空；
- Web listener 配置 `WEB_TLS_CERT` 与 `WEB_TLS_KEY`；
- 若 gRPC listener 未关闭，还必须配置 `WEB_GRPC_TLS_CERT` 与 `WEB_GRPC_TLS_KEY`。

缺任意一项会在监听端口前抛出配置错误。这里的原则是“保险柜没锁好就不开门”。开发模式仍允许无认证运行旧测试，避免把学习入口一次性全部封死。

### 4.3 授权规则

- `/ws`、`/events`、`/events/*`、`/events-status`、`/delivery-metrics`、`/metrics`、`/admin` 在启用密钥后都需要登录。
- 运维指标和 SSE 发布要求 `tenant=ops`。
- WebSocket/SSE 若仍带 `?uid=`，它必须等于签名身份；不带时直接使用签名身份。
- 生产模式下 WebSocket/SSE 必须携带白名单中的 `Origin`；非生产模式配置了白名单后，也会拒绝明确携带的不可信 Origin。
- 只要写请求使用 Cookie 身份，就必须携带白名单 `Origin`，这条规则不因开发模式而放松。原因是浏览器会自动附带 Cookie，缺少 Origin 校验会留下 CSRF 入口。

## 5. 容量、背压和观测

| 层 | 当前边界 | 作用 |
|---|---:|---|
| TCP | `WEB_MAX_CONNECTIONS`，默认 10000 | 防止 fd/Connection 无界增长 |
| 认证身份 | `WEB_REQUESTS_PER_MINUTE`，默认 120 | 防止一个身份高速建立请求/长连接 |
| 限流表 | `WEB_RATE_LIMIT_IDENTITIES`，默认 65536 | 防止随机 uid 反过来撑爆限流器 |
| SSE | 总订阅 5000；每用户 4；每用户历史 256；最多 4096 个历史用户 | 限制长连接和重放内存 |
| HTTP/2 | 最大并发 stream 32；请求/响应体各 1 MiB | 限制单连接多路复用资源 |
| gRPC | 活跃 RPC 默认 256；Core 运行时线程预算默认 64 | CallbackCall 限业务状态，ResourceQuota 限 Core 线程；等待中的流不独占业务线程 |
| gRPC 消息 | Server 1 MiB；业务消息/块 64 KiB | 传输上限与业务上限分层 |
| Upload | 最多 10000 块、总计 4 MiB、序号连续 | 防止 client stream 无限上传 |
| Count/Chat | 最多 10000 条；RPC 最长 30 秒 | 防止永久业务循环 |
| 出站 | 每连接 256 个任务、4 MiB，Writer 有单轮发送预算 | 慢客户端把压力挡在有界队列内 |
| SegmentPool | 每池最多缓存 256 个 Block；析构释放全部缓存 | 流量尖峰退去后归还多余内存，并避免池对象结束时泄漏 |

新增接口：

- `GET /health/live`：进程存活。
- `GET /health/ready`：运行图已经启动并可接流量。
- `GET /metrics`：Prometheus 文本，含 HTTP 拒绝、gRPC、WebSocket 在线数、SSE 在线/历史/重放等指标。
- `GET /events-status`：SSE 的 JSON 细节。

gRPC 会把 `x-request-id` 和已认证身份放入 initial metadata。客户端提供合法的 64 字节内 request-id 就原样关联；没有时由服务端生成。

## 6. gRPC 四种调用形态

```text
Echo    : request  ───────────────→ reply
Count   : request  ───────────────→ reply, reply, reply...
Upload  : chunk, chunk, chunk... ─→ summary
Chat    : message ⇄ message ⇄ message（同一个 HTTP/2 stream）
```

它们分别对应 unary、server streaming、client streaming、bidirectional streaming。Phase 9 当前实现继承生成代码的 `CallbackService`，每次调用返回一个独立 Reactor。四个 Reactor 都先创建 `CallbackCall`：

```text
metadata Bearer token
  → HMAC + expiry 校验
  → 原子并发配额 CAS
  → grpcStarted++
  → 添加 x-request-id / authenticated metadata
  → StartRead / StartWrite / Alarm
  → 完成回调检查 IsCancelled + 服务端最长时长
  → OnDone 删除 Reactor，释放配额并记录 completed/cancelled
```

边界错误返回 `INVALID_ARGUMENT`，容量耗尽返回 `RESOURCE_EXHAUSTED`，未登录返回 `UNAUTHENTICATED`，取消/超时返回 `CANCELLED` 或 `DEADLINE_EXCEEDED`。错误只终止当前 RPC，不关闭共享 HTTP/2 connection。

四种 Reactor 的推进方式如下：

| RPC | Callback 推进方式 | 关键所有权 |
|---|---|---|
| Echo | 填充 reply 后立即 `Finish()` | 自定义 Unary Reactor 活到 `OnDone()`，业务配额不会提前归还 |
| Count | `StartWrite()` → `OnWriteDone()`；间隔由 `grpc::Alarm` 唤醒 | reply 是 Reactor 成员；定时等待不 `sleep`、不占住 Worker |
| Upload | 一次只挂一个 `StartRead()`；`OnReadDone()` 累计后继续读 | chunk 是 Reactor 成员，半关闭后生成 summary |
| Chat | `StartRead()` → `OnReadDone()` → `StartWrite()` → `OnWriteDone()` | 同时只保留一个业务写，形成逐消息背压 |

可以把同步 API 想成“服务员站在桌边一直等客人吃完”，Callback Reactor 则像“留下桌号，出菜或客人呼叫时再回来”。连接和 RPC 仍然存在，但等待期间没有一条业务线程被永久绑在这张桌子旁。

`OnDone()` 是 Reactor 唯一的销毁点，因为只有此时 gRPC 才保证该 RPC 的所有读写 reaction 已结束。Count 的 `Alarm` 不在 gRPC RPC 完成计数内，所以取消时先取消 Alarm、等待它的回调落地，再执行 `Finish()`；否则可能在 Reactor 删除后留下定时回调访问悬空指针。Callback API 已解决“每条长流占一个同步 handler 线程”的模型问题；仍需通过容量压测确定 EventEngine/Core 线程、活跃 RPC 配额和业务内存预算。

### 6.1 gRPC 与 OpenSSL 必须来自同一套 TLS 体系

远程 Linux 首轮 TLS 黑盒测试曾在 `SSL_CTX_new()` 崩溃。调试栈显示，`TLS_method()` 来自系统 OpenSSL，`SSL_CTX_new()` 却解析进了旧 gRPC 静态库携带的 BoringSSL。两套库导出了同名符号，链接器像把“甲厂钥匙”交给“乙厂锁芯”，函数名能对上，内部对象布局却不兼容。

这不是证书错误，也不能靠调整链接顺序可靠解决。最终做法是让 Web TLS 与 gRPC 都使用系统 OpenSSL：

```text
gRPC source
  → -DgRPC_SSL_PROVIDER=package
  → OPENSSL_SSL_LIBRARY=/usr/lib64/libssl.so
  → OPENSSL_CRYPTO_LIBRARY=/usr/lib64/libcrypto.so
  → 独立安装前缀 ~/.local-grpc-systemssl
  → 本项目的 gRPC_DIR、Protobuf_DIR、protoc、grpc_cpp_plugin 全指向该前缀
```

关键配置如下，路径可按机器调整：

```bash
cmake -S "$HOME/grpc" -B "$HOME/grpc/cmake/build-systemssl" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local-grpc-systemssl" \
  -DgRPC_INSTALL=ON -DgRPC_BUILD_TESTS=OFF -DBUILD_TESTING=OFF \
  -DgRPC_SSL_PROVIDER=package \
  -DOPENSSL_ROOT_DIR=/usr \
  -DOPENSSL_SSL_LIBRARY=/usr/lib64/libssl.so \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib64/libcrypto.so
cmake --build "$HOME/grpc/cmake/build-systemssl" --target install --parallel 4
```

最终链接文件只出现 `/usr/lib64/libssl.so` 与 `/usr/lib64/libcrypto.so`，不再出现 BoringSSL 或旧前缀中的静态 `libssl.a`。部署规则也应固定为：**同一进程只能选定一套 OpenSSL ABI/提供者，所有直接和间接依赖都跟随它。**

## 7. SSE 断线续传

发布事件时，[`SseReplayBuffer`](../../server/sse/SseReplayBuffer.cpp) 先把事件放入按用户分组的有界历史，再投递给在线连接：

```text
publish(uid, event 43)
  → bounded history[uid]
  → online tabs

browser disconnects and reconnects
  → Last-Event-ID: 42
  → ready event
  → replayAfter(42)
       ├─ 找到 42 → 补 43 及之后事件
       └─ 找不到 42 → event: replay-reset
```

`replay-reset` 很关键：历史窗口只有 256 条，若客户端离线太久，服务端不能假装“没有漏消息”。客户端收到它后应请求业务快照，再从新游标继续。

这里的历史只在单进程内存中。进程重启或多实例切换会丢失，因此订单、告警等可靠事件仍应使用 Redis Streams、Kafka、数据库 outbox 等外部持久化日志。

## 8. 协议级优雅下线

```text
SIGTERM
  → ready=false
  → 停止接受新 TCP
  → ReactorGroup::beginDrain()
       ├─ HTTP/2    → GOAWAY(NO_ERROR, last processed stream)
       ├─ WebSocket → Close(1001, "server shutdown")
       └─ SSE       → event: server-shutdown / data: reconnect
  → 最多 500 ms 控制帧排空窗口（可配置）
  → 停 Reactor
  → 排空 Executor
```

HTTP/2 的 GOAWAY 像酒店挂出“停止办理新入住”：编号不大于 `last_stream_id` 的已接纳 stream 可以收尾，更新的请求应换连接重试。WebSocket 1001 明确表达端点离开；SSE 客户端收到 shutdown 或连接关闭后按 `retry` 重连，并用 `Last-Event-ID` 恢复。

`WEB_SHUTDOWN_DRAIN_MS` 默认 500 ms、最大 30 秒；连接提前排空会立即继续关机。它不保证跨公网慢客户端一定收到，生产编排还应先让负载均衡摘除实例，再根据最长在途请求设置窗口。

## 9. 文件索引

| 文件 | 本阶段职责 |
|---|---|
| [`AuthToken.h/.cpp`](../../server/security/AuthToken.h) | 跨协议令牌签发、提取、常量时间校验 |
| [`FixedWindowRateLimiter.h`](../../server/security/FixedWindowRateLimiter.h) | 有界身份表与固定窗口请求配额 |
| [`OperationalMetrics.h`](../../server/ops/OperationalMetrics.h) | 跨协议原子计数和 Prometheus 输出 |
| [`SseReplayBuffer.h/.cpp`](../../server/sse/SseReplayBuffer.h) | 有界事件历史、游标补发和 gap 检测 |
| [`SseSessionManager.cpp`](../../server/sse/SseSessionManager.cpp) | SSE 连接配额、保存后发布、单连接重放和指标 |
| [`HttpSession.cpp`](../../server/http/HttpSession/HttpSession.cpp) | 把可信身份与 Last-Event-ID 交给长连接 Session |
| [`GrpcServer.cpp`](../../server/grpc/GrpcServer.cpp) | CallbackService、四类 Reactor、统一 CallbackCall、TLS、健康检查与优雅停止 |
| [`web_learning.proto`](../../server/grpc/proto/web_learning.proto) | 四类 RPC 的 IDL 契约 |
| [`Http2Codec.cpp`](../../server/http2/Http2Codec.cpp) | GOAWAY 生成与现有 nghttp2 协议状态 |
| [`Session.h`](../../server/session/Session/Session.h) | 新增协议无关 `beginDrain()` 生命周期钩子 |
| [`SubReactor.cpp`](../../server/SubReactor/SubReactor.cpp) | 把跨线程 drain 请求送回连接所属 Reactor 执行 |
| [`SegmentPool.cpp`](../../server/SegmentPool/SegmentPool.cpp) | 有界缓存发送 Block，并在池析构时释放所有权 |
| [`main.cpp`](../../main.cpp) | 安全配置、中间件、探针、指标与 gRPC 组合入口 |
| [`phase9_foundation.cpp`](../../tests/phase9_foundation.cpp) | token、限流、SSE replay 单元测试 |
| [`grpc_integration.cpp`](../../tests/grpc_integration.cpp) | 四类真实 RPC、认证、metadata、配额、取消、Callback 非阻塞等待和 TLS 可选测试 |
| [`phase9_security_blackbox.py`](../../tests/integration/phase9_security_blackbox.py) | HTTP/WS/SSE 身份、Origin、授权、指标与失败关闭 |
| [`phase9_shutdown_blackbox.py`](../../tests/integration/phase9_shutdown_blackbox.py) | 真实 socket 验证 SSE/WS 停机控制消息 |

## 10. 生产适用性与仍缺少的条件

当前版本比 Phase 8 更接近可部署服务，但仍不应直接作为公网、多实例、关键业务的通用生产网关。

| 领域 | 已有 | 仍缺少 |
|---|---|---|
| 身份 | 统一签名、过期、租户、Origin | 密钥轮换与 `kid`、撤销、OIDC/JWT/JWKS、细粒度权限、审计日志 |
| TLS | TLS 1.3、ALPN、启动失败关闭 | 自动证书轮换、mTLS、密码套件/合规策略、真实公网互操作矩阵 |
| HTTP/2 | nghttp2、stream 协程、流控、GOAWAY | 流式/文件响应、完整公平调度、长时间压力与恶意帧专项测试 |
| gRPC | 官方 Core、Callback API、四类 Reactor、配额、取消、健康检查 | 容量基准、拦截器、mTLS 身份、反射策略、重试/幂等契约；只有需要手动事件队列控制时再评估 CQ |
| SSE | 心跳、背压、连接配额、断线重放 | 外部持久化日志、多实例共享游标、快照恢复接口 |
| WebSocket | 帧校验、心跳、背压、ACK/重试、可信身份、1001 下线 | 跨进程 durable outbox、集群会话路由、重复消息持久化去重 |
| 运维 | liveness/readiness、Prometheus、request-id | OpenTelemetry tracing、结构化日志字段、告警规则、仪表盘和 SLO |
| 验证 | 单元/集成/黑盒/故障场景入口 | 长时 soak、容量基准、真实多实例滚动升级、外部依赖故障演练 |

生产上的差距主要不再是“还缺一个帧解析器”，而是跨进程状态、身份治理、可观测闭环和持续容量证据。

## 11. 验证记录

### 本地 Windows 已执行

| 检查 | 结果 |
|---|---|
| `phase9_foundation` 独立编译与运行 | 通过：token 正常/篡改/过期、Bearer/Cookie、限流复位/容量、SSE replay/gap/淘汰 |
| gRPC C++ 真实 socket 集成测试（明文 HTTP/2） | 通过：四种 RPC、认证 metadata、request-id、参数错误、并发拒绝、deadline/取消 |
| gRPC C++ 真实 TLS + ALPN `h2` 集成测试 | 通过：临时自签名证书下重复执行全部上述场景 |
| nghttp2 codec 往返 | 通过：前言拆分、HPACK、多 stream、POST DATA、RST_STREAM、新增 GOAWAY |
| Phase 9 Python 黑盒脚本语法编译 | 通过 |
| Cppcheck exhaustive（本阶段源文件） | 通过，无 warning/performance/portability 报告 |
| `git diff --check` | 通过；仅有 Git 的 LF/CRLF 提示 |
| 完整 C++ 构建 | 不适用：本机没有 Linux epoll 环境，最终结果以远程 Linux 为准 |

### 远程 Linux

验证在远程虚拟机的 `test8.0` 基线 `f11fa90` 上覆盖 Phase 9 源码后执行；Callback API 改造也先在这份测试副本验证，再创建并推送 `test9.0`。

| 项目 | 实际环境 |
|---|---|
| 操作系统 | CentOS Stream 9 |
| 编译器 | GCC/G++ 11.5.0 |
| CMake | 3.31.8 |
| TLS | OpenSSL 3.5.5 |
| HTTP/2 | libnghttp2 1.43.0 |
| gRPC | gRPC C++ 1.82.0，使用 `gRPC_SSL_PROVIDER=package` 重新构建 |

为执行完整检查，GoogleTest 安装到用户前缀；gRPC 安装到独立的 `~/.local-grpc-systemssl` 前缀；虚拟机补装了与 GCC 11 配套的 `libasan`、`libubsan` 运行库。项目链接审计只发现系统 `/usr/lib64/libssl.so`、`/usr/lib64/libcrypto.so`，没有再混入 BoringSSL。

| 最终检查 | 结果 |
|---|---|
| Debug 全量编译 | 通过；主程序、核心库、gRPC 生成代码和全部测试目标成功链接 |
| Debug CTest | **80/80 通过**，Callback 最终版本总计 25.41 秒 |
| gRPC 明文集成 | 通过；四类 Callback Reactor、身份、metadata、限额、deadline/取消均覆盖；慢 Count 等待期间并发 Echo 在 500 ms 门限内完成 |
| gRPC TLS 集成 | 通过；临时 `localhost` 证书下，常规版与 ASan/UBSan 版独立执行均返回 0，使用 HTTP/2 ALPN `h2` |
| TLS/HTTP/WS/SSE 黑盒 | 全部通过；包括 TLS 握手、旧 HTTP 回归、WS、SSE 重放、安全策略和协议下线 |
| ASan + LeakSanitizer + UBSan 全量 CTest | **80/80 通过**，Callback 最终版本总计 30.43 秒；`detect_leaks=1`、遇错立即终止 |
| OpenSSL 路径修复后原 `build` 目录复验 | **80/80 通过**，24.97 秒；配置阶段确认 `SSL_read_ex`、`SSL_write_ex` 均来自系统 OpenSSL 3.5.5 |
| OpenSSL 错误路径防护 | 通过；故意指定 `~/.local/include` 与 `~/.local/lib64/*.a` 时，CMake 在配置阶段停止并打印被选中的三个路径 |

本次验证不是只记录失败，而是逐项闭环：

| 首次暴露的问题 | 根因 | 修复 |
|---|---|---|
| GCC 干净构建时 `HttpCodec.cpp` 类型不完整 | 源文件意外依赖 `SubReactor.h` 的传递包含 | 直接包含 `Router.h` 与 `RequestContext.h`，让依赖关系可见 |
| gRPC TLS 在 `SSL_CTX_new()` 崩溃 | 系统 OpenSSL 与旧 gRPC 的静态 BoringSSL 同进程符号冲突 | 用 `gRPC_SSL_PROVIDER=package` 重建 gRPC，并审计最终链接输入 |
| `TlsTransport.cpp` 提示 `SSL_read_ex`、`SSL_write_ex` 未声明 | CMake 从 `~/.local` 选中了不含所需 API 的旧头文件与静态库；后续 CTest 失败只是未生成可执行文件的连锁结果 | 显式绑定 `/usr` 的系统 OpenSSL；CMake 新增符号检查，使同类问题在配置阶段提前失败 |
| `/delivery-metrics` 被识别成 `text/plain` | 先设置 JSON 头，随后 `text()` 又覆盖了类型 | 先写 body，再把 `Content-Type` 明确设为 JSON |
| Cookie 写请求缺少 Origin 时未被拦截 | 非生产模式放松规则造成 CSRF 缺口 | Cookie 写请求在所有模式都要求白名单 Origin |
| gRPC 并发配额测试偶发失败 | 客户端 deadline 状态可早于服务端 `OnDone()` 释放 `CallbackCall` | 测试在下一次配额断言前给取消 reaction 一个有界释放窗口 |
| 旧 `/admin` 回归得到 404 | 未配置 HMAC 密钥时统一中间件整体旁路 | 学习模式保留旧版缺少 legacy token 返回纯文本 401 的契约 |
| LeakSanitizer 报告每个测试泄漏 2056 字节 | 局部 `SegmentPool` 缓存的 `Block` 没有析构释放 | 增加析构所有权清理，并把每池空闲缓存限制为 256 个 Block |
| Callback 首轮 LeakSanitizer 报告 1080 字节 | `TimerState` 拥有 `Alarm`，Alarm 回调又用 `shared_ptr` 强引用 `TimerState`，形成引用环 | 回调改捕获 `weak_ptr`；执行时临时加锁成强引用，既打破环又保证回调期间状态存活 |

完整过程的数字是：Phase 9 首轮 76/80，修复功能与 TLS 依赖后 79/80，补回兼容语义后 80/80；最初 Sanitizer 78/80，修复 SegmentPool 后 80/80。升级 Callback 后，普通 CTest 首轮即为 80/80；Callback 首轮 Sanitizer 为 79/80，修复 Alarm 引用环后最终恢复 80/80。这里保留中间失败，是为了说明测试确实找到了问题，而不是只展示最后的绿色结果。

## 12. 推荐复习顺序

1. 从 `main.cpp` 的统一中间件开始，跟一次 `/events` 请求，看“不可信 uid”如何变成“已签名身份”。
2. 单步 `SseReplayBuffer::append()` 与 `replayAfter()`，分别模拟正常补发和历史 gap。
3. 对照 `.proto` 和 `GrpcServer.cpp`，依次调用 Echo、Count、Upload、Chat，画出每种方法的读写方向。
4. 在 Count 中设置很短的 deadline，看 `IsCancelled()` 如何让循环停止；再把并发配额设为 1，观察第二个 RPC 的 `RESOURCE_EXHAUSTED`。
5. 跟一次 SIGTERM：acceptor 停止、Reactor drain、协议控制帧、Executor 排空。重点理解“拒绝新工作”和“处理完旧工作”是两个步骤。

下一阶段建议先做外部持久化事件日志与 OIDC/JWT 密钥轮换，再接入 OpenTelemetry，并对 Callback API 做长流容量基准。先把本阶段的身份边界、Reactor 生命周期、重放语义和停机顺序完全掌握，再继续扩展。
