# Phase 10：HTTP/gRPC 性能路径与架构拆分

> 基线提交：`test9.0` 的 `fa15039`。本阶段实现与验证结果收录在 `test9.1`。

## 1. 为什么约 2 万 QPS 不足以直接判定“gRPC 把 HTTP 拖慢了”

一次 `wrk` 数字同时受 CPU 核数、虚拟机调度、Release/Debug、连接数、日志、测试路径和同机客户端影响。历史文档里也出现过约 1.98 万、2.33 万和 3.10 万的不同结果，所以只比较一次数字会把环境噪声误当成代码回归。

不过，代码审查确实找到了几条会稳定增加开销的路径：

1. **所有 HTTP handler 都绕行 Worker**：连 `/fast` 这种只写 4 字节的处理器，也要经历线程池互斥队列、Worker 调度、完成队列、`eventfd` 和协程恢复。
2. **所有请求争用单个指标原子变量**：多个 CPU 核不断抢同一条缓存行。
3. **对象池和 Buffer 池各只有一把全局锁**：多个 Reactor 最终又在池门口串行排队。
4. **响应头产生临时字符串**：状态码、长度和每一行首部使用 `operator+`/`to_string` 拼装，增加分配与复制。
5. **路由和认证有可避免的临时对象**：静态路由每次拼接 `"METHOD path"`；令牌拆分创建动态 `vector`。Bearer/Cookie 的返回值仍必须拥有数据，不能用可能悬空的 `string_view` 换取表面上的零复制。
6. **集成 gRPC 与 Web 共用一个进程预算**：它便于教学和统一启动，但压测时 gRPC Core、Web Reactor 和两个 Executor 会竞争同一组 CPU，难以判断瓶颈属于哪一侧。

本阶段把这些问题逐项消除，并提供可重复的五轮中位数基准流程。优化目标是：HTTP 与 gRPC 都提升吞吐，同时保持错误为 0，P99 退化不超过 10%。

## 2. HTTP 新执行路径

### 2.1 两类 handler

```text
HTTP bytes
  → HttpParser
  → Router::resolve（只读，不运行用户代码）
       ├─ ReactorSafe
       │    → middleware → handler → encode → OutboundQueue
       └─ Worker（默认）
            → Executor queue → handler
            → completion queue/eventfd → Reactor
            → encode → OutboundQueue
```

`Worker` 仍是默认值。它像后厨：阻塞 I/O、休眠、复杂计算和未知业务全部送进去，避免卡住负责整层连接的 Reactor。

`ReactorSafe` 像前台当场盖章：只允许时间有明确上界、不会阻塞、不会等待外部资源的短处理器。它省掉一次跨线程往返，但错误标记一个慢 handler 会同时拖慢该 Reactor 上的其他连接。

当前显式标记为 `ReactorSafe` 的路径只有：

- `/`
- `/fast`
- `/health/live`
- `/health/ready`
- `/user/:id`

文件、分块流、SSE、WebSocket、运维指标和 `/slow` 仍走 Worker。路由 API 示例：

```cpp
router.GET("/fast", [](RequestContext &ctx) {
    ctx.response->text("fast");
    return true;
}, RouteOptions{ExecutionPolicy::ReactorSafe});
```

### 2.2 为什么必须冻结 Router

`resolve()` 会把命中的 `RouteEntry*` 放进 `RequestContext`，稍后执行 handler。若启动后还能注册路由，`unordered_map`/`vector` 扩容可能让这个指针失效。

所以 `ServerRuntime::start()` 在监听前调用 `Router::freeze()`。冻结后：

- Reactor 可以并发只读路由表；
- `GET`、`POST`、`use` 再注册会抛 `logic_error`；
- 所有业务路由必须在 `start()` 前装配完。

这里像演出开场前封存座位表：观众入场后，工作人员只查表，不再移动座位编号。

### 2.3 路由与中间件的小对象优化

- 静态路由从单表 `"METHOD path"` 改为 `method → path → RouteEntry`，查询时不再拼接 key。
- 中间件的 `next()` 从递归 `std::function<void()>` 改为“状态指针 + 普通函数指针”的 `MiddlewareNext`，每次请求不再为 continuation 构造可拥有闭包。
- `resolve()` 与 `handle()` 分离：前者只选执行策略，后者才运行中间件和 handler。

约束：中间件不应在 `resolve()` 后改写 `request.method/path`。鉴权、限流、日志和上下文字段填充不受影响。

## 3. 共享热锁的拆分

### 3.1 分片指标

旧 `OperationalMetrics` 的每个指标只有一个原子变量。多核写同一原子值时，那条缓存行会在 CPU 核之间反复搬运。

`StripedCounter` 为每个线程稳定选择 64 个槽位中的一个；请求只累加自己的槽，Prometheus 抓取时再求和。可以把它理解成“写入时开放 64 个收费口，读报表时由财务汇总”。指标是观测数据，抓取瞬间允许各字段相差一个并发事件，不参与业务正确性判断。

### 3.2 分片对象池与 Buffer 池

`ObjectPoll` 和 `BufferPoll` 也从单锁改为 64 个缓存行隔离的分片：

- 优先从当前线程分片借还，快路径只触碰一把局部锁；
- 本地为空时依次尝试其他分片，支持 Worker 借出、Reactor 归还这种跨线程交接；
- 每个分片有容量上限，流量尖峰后不会无限保留对象。

普通 HTTP 的 `HttpResponse` 现在在所属 Reactor 上先借出，再交给 Worker 填写，最终由同一 Reactor 的 Writer 归还。对象的所有权路径更清楚，也提高本地分片命中率。

## 4. 响应与认证热路径

### 4.1 响应头直接写 Buffer

生产版 `HttpResponse::buildHeader()` 现在：

- 使用 `std::to_chars` 把状态码、长度和 Range 数字直接写入栈数组；
- 分段 `Buffer::append`，不构造 `"key: " + value + "\r\n"` 临时串；
- 直接做大小写不敏感的 header 名比较，不为每个 header 创建小写副本；
- `text/html/json` 接收 `string_view`，字符串字面量不必先构造成 `std::string`。

### 4.2 令牌解析不再为切分分配容器

HMAC 令牌固定为 5 段，`AuthToken` 使用栈上的 `array<string_view, 5>`，不再为切分创建动态 `vector`。Bearer 和 Cookie 提取结果返回拥有自身存储的 `string`，因此调用者即使传入临时字符串也不会得到悬空视图；最终身份对象只长期保存确实需要的 tenant。

这里刻意保留了一次安全拷贝。`string_view` 像“货架位置”，它不拥有货物；原 header 或临时字符串一销毁，位置还在，货物却没了。Debug 测试曾准确抓到这类生命周期错误，所以认证边界选择清楚的所有权，而不是用危险的借用换一次小拷贝。

## 5. gRPC 的架构调整

### 5.1 集成进程与独立进程同时保留

本阶段保留原 `webserver` 内的 gRPC listener，便于“一次启动看到全部协议”。同时新增 `webserver-grpc`：

```text
集成模式
webserver
  ├─ Web Reactor/Executor
  └─ gRPC Core

隔离模式
webserver (WEB_GRPC_ADDRESS=off)     webserver-grpc
  └─ Web Reactor/Executor              └─ gRPC Core/CallbackService
```

隔离模式的价值：

- Web 与 gRPC 可分别设置 CPU/容器配额并独立扩容；
- 一侧流量尖峰不会直接挤占另一侧进程的线程预算；
- 基准结果能区分 HTTP 改动和 gRPC 改动；
- 崩溃与发布边界更小。

独立进程沿用 `WEB_GRPC_*`、`WEB_AUTH_SECRET`、`WEB_PRODUCTION_MODE` 配置。它在创建 gRPC 线程前阻塞 `SIGINT/SIGTERM`，主线程通过 `sigwait` 收到信号后执行有期限的优雅停止。

### 5.2 Callback 热路径

- metadata 在一次认证调用内部通过 `grpc::string_ref`/`string_view` 借用，避免仅为查找构造 key/value 字符串；令牌提取完成后转为安全的拥有型结果；
- 并发准入从 CAS 自旋改为一次 `fetch_add`，超限立即回滚；
- 高频原子状态按缓存行对齐，减少无关计数器之间的伪共享。

### 5.3 真正的服务端总时限

旧实现只在读、写或 Count 间隔回调发生时检查 `maxRpcDuration`。若 Upload/Chat 客户端连上后一直不发消息，服务端没有回调，就不能主动结束该 RPC。

现在 Count、Upload、Chat 在准入后各自设置独立 `grpc::Alarm`。时间到时调用一次受互斥终态保护的 `Finish(DEADLINE_EXCEEDED)`。Alarm 回调和 `OnDone` 可能并发，因此 Reactor 使用侵入式引用计数：

```text
gRPC 生命周期引用 = 1
schedule Alarm       +1

OnDone       → Cancel Alarm → release gRPC 引用
Alarm callback       → Finish(必要时) → release Alarm 引用
最后一个 release     → delete this
```

它像两名收尾人员各拿一把仓库钥匙：只有 gRPC 和定时器都交回钥匙，对象才能销毁，避免定时回调访问已经删除的 Reactor。

Linux 首轮集成测试还揭示了另一个次序问题：Alarm 引用可能让 C++ Reactor 活得比 `CallbackServerContext` 更久，若析构函数再调用 `context.IsCancelled()`，就会访问已经结束的 gRPC 上下文。修复后的规则是：

1. `OnDone()` 在 gRPC 上下文仍有效时调用幂等的 `CallbackCall::complete()`，完成指标和并发配额归还；
2. Reactor 析构函数不再读取 context；
3. `OnDone()` 释放 gRPC 引用，Alarm 回调独立释放 Alarm 引用；
4. 最后一个引用只负责销毁 C++ 对象，不再触碰 gRPC 生命周期对象。

这条规则把“协议完成”和“内存销毁”分开：前者必须在 `OnDone()` 内完成，后者可以等所有异步回调退场后发生。

## 6. 可调容量参数

| 变量 | 默认 | 作用 |
|---|---:|---|
| `WEB_HTTP_WORKERS` | 0，自动 | 普通 HTTP Worker 数，范围 1–32 |
| `WEB_WS_WORKERS` | 0，自动 | WebSocket Worker 数，范围 1–32 |
| `WEB_GRPC_MAX_WORKER_THREADS` | 64 | gRPC Core 线程预算 |
| `WEB_GRPC_MAX_CONCURRENT_RPCS` | 256 | 活跃 RPC 状态上限 |
| `WEB_GRPC_MAX_RPC_MS` | 30000 | 服务端 RPC 总时限 |
| `WEB_GRPC_MAX_RECEIVE_BYTES` | 1 MiB | gRPC 接收消息传输上限 |
| `WEB_GRPC_MAX_SEND_BYTES` | 1 MiB | gRPC 发送消息传输上限 |

0 表示 HTTP/WS 使用原来的 CPU 自动分配。调参时先固定虚拟机 vCPU；CPU 密集 handler 的 Worker 通常接近 vCPU 数，阻塞业务需要结合下游连接池容量，不能盲目加线程。

## 7. 构建与运行

```bash
cmake -S . -B build-release \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DWEBSERVER_ENABLE_GRPC=ON \
  -DWEBSERVER_BUILD_BENCHMARKS=ON \
  -DCMAKE_PREFIX_PATH="$HOME/.local-grpc-systemssl" \
  -DOPENSSL_ROOT_DIR=/usr \
  -DOPENSSL_INCLUDE_DIR=/usr/include \
  -DOPENSSL_SSL_LIBRARY=/usr/lib64/libssl.so \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib64/libcrypto.so \
  -DOPENSSL_USE_STATIC_LIBS=OFF
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure
```

独立运行：

```bash
WEB_GRPC_ADDRESS=off ./build-release/webserver
WEB_GRPC_ADDRESS=127.0.0.1:50051 ./build-release/webserver-grpc
```

## 8. 基准流程与验收门槛

仓库内的 [`benchmark_phase10.sh`](../../scripts/benchmark_phase10.sh) 会：

1. 独立启动 Web 进程；
2. 对 `/fast` 连续执行 5 轮 `wrk --latency`；
3. 独立启动 gRPC 进程；
4. 对 Echo、Count、Upload、Chat 各跑 5 轮；
5. 保存所有原始输出，并生成五轮中位数 `summary.json`。

```bash
BUILD_DIR=build-release WEB_SERVER_REACTORS=2 \
HTTP_THREADS=2 HTTP_CONNECTIONS=64 RUN_SECONDS=15 \
GRPC_CONCURRENCY=32 \
bash scripts/benchmark_phase10.sh
```

同机压测时应给负载发生器保留 CPU。本文实测虚拟机只有 4 个 vCPU，因此 HTTP 验收使用 2 个 Reactor + 2 个 `wrk` 线程。若压测端在另一台机器，可让服务端按全部 CPU 自动创建 Reactor。脚本会把 HTTP 非 2xx、connect/read/write/timeout socket error 全部计入 `http_errors`，不能只看 QPS。

若启用认证，把签发好的令牌通过 `GRPC_TOKEN` 环境变量传入。不要把令牌写进脚本或提交到仓库。

基线和当前版本必须使用同一台空闲虚拟机、相同 vCPU/内存、相同编译器和参数，并交替执行，避免温度与宿主机负载只偏向一边。比较命令：

```bash
python3 scripts/compare_phase10_results.py \
  benchmark-results/baseline/summary.json \
  benchmark-results/current/summary.json
```

验收门槛：

- HTTP `/fast` 五轮中位 QPS 至少提升 20%；
- gRPC Echo 五轮中位 QPS 至少提升 15%；
- HTTP 与四类 gRPC 调用错误数都为 0；
- HTTP 和 gRPC Echo 的中位 P99 不得比基线恶化超过 10%。

未达到门槛时保留原始结果，先用 `perf record`/火焰图确认新瓶颈，再决定是否扩大 Reactor、队列或协议层改造。不能为追求 QPS 把未知业务批量标成 `ReactorSafe`。

## 9. 文件索引

| 文件 | 变更目的 |
|---|---|
| [`Router.h`](../../server/Route/Router.h)、[`Router.cpp`](../../server/Route/Router.cpp) | ExecutionPolicy、只读 resolve、freeze、无分配 MiddlewareNext、静态路由双层索引 |
| [`HttpCodec`](../../server/http/HttpCodec/HttpCodec.h) | 暴露只解析执行策略的接口 |
| [`HttpSession`](../../server/http/HttpSession/HttpSession.cpp) | ReactorSafe 直达与 Worker 默认路径、统一异常/超时收口、响应对象本地借还 |
| [`ServerRuntime`](../../server/Runtime/ServerRuntime.h) | 冻结路由、HTTP/WS Worker 独立配置 |
| [`ObjectPool`](../../server/ObjectPool/ObjectPool.h)、[`BufferPoll`](../../server/buffer_pool/BufferPoll.h) | 64 分片池与跨分片窃取，移除全局热锁 |
| [`StripedCounter`](../../server/ops/StripedCounter.h)、[`OperationalMetrics`](../../server/ops/OperationalMetrics.h) | 分片指标与一致快照接口 |
| [`http.cpp`](../../server/http/http.cpp) | 响应头直接写 Buffer、`to_chars`、`string_view` 响应体入口 |
| [`AuthToken.cpp`](../../server/security/AuthToken.cpp)、[`AuthToken.h`](../../server/security/AuthToken.h) | 固定数组拆令牌；Bearer/Cookie 返回拥有型字符串，避免临时输入造成悬空视图 |
| [`GrpcServer.cpp`](../../server/grpc/GrpcServer.cpp) | metadata/准入热路径、服务端总时限 Alarm、Reactor 安全销毁 |
| [`GrpcMain.cpp`](../../server/grpc/GrpcMain.cpp) | 独立 gRPC 进程与信号驱动优雅停机 |
| [`grpc_benchmark.cpp`](../../benchmarks/grpc_benchmark.cpp) | 四类 RPC 的仓库内并发吞吐/P50/P95/P99 客户端 |
| [`benchmark_phase10.sh`](../../scripts/benchmark_phase10.sh) | 五轮 HTTP/gRPC 基准、原始结果与中位数汇总 |
| [`compare_phase10_results.py`](../../scripts/compare_phase10_results.py) | 自动检查吞吐、错误和 P99 验收门槛 |
| [`unit_tests.cpp`](../../tests/unit_tests.cpp)、[`grpc_integration.cpp`](../../tests/grpc_integration.cpp) | 执行策略、冻结、并发计数和服务端 deadline 回归测试 |

## 10. Linux 实测结果

### 10.1 环境与正确性验证

测试日期为 2026-09-29。环境为 CentOS Stream 9 虚拟机、4 vCPU、GCC 11.5、OpenSSL 3.5.5、libnghttp2 1.43.0、gRPC C++ 1.82.0。基线是 `fa15039`，当前版对应 `test9.1`；两版均使用 Release 构建并在同一台虚拟机执行。

| 检查 | 结果 | 用时 |
|---|---:|---:|
| Linux Release CTest | 84/84 通过 | 25.98 s |
| Linux Debug CTest（启用 `assert`） | 84/84 通过 | 26.74 s |
| Linux ASan + LSan + UBSan CTest | 84/84 通过 | 29.77 s |
| `git diff --check` | 通过 | — |
| Python 比较工具 `py_compile` 与合成样例 | 通过 | — |
| 修改范围 `cppcheck` | 未发现 warning/performance 诊断；只有分析分支上限提示 | — |

Debug 测试曾发现 Bearer/Cookie 返回悬空 `string_view`，现已改为拥有型结果。Release 首轮 gRPC 集成测试曾发现 Alarm 延长 Reactor 生命周期后，析构阶段访问失效 `CallbackServerContext`；现已按 5.3 节把协议完成固定在 `OnDone()` 内。两项修复后，Release、Debug 和 Sanitizer 三套全量测试均通过。

### 10.2 五轮中位数

HTTP 受控配置：2 个服务端 Reactor、`wrk -t2 -c64 -d5s --latency`，连续 5 轮。gRPC 配置：独立 `webserver-grpc` 进程、并发 32、预热 2 秒、每轮 5 秒、每种模式 5 轮。认证关闭，因此后续令牌所有权修复不位于本次基准热路径。

| 指标 | `fa15039` 基线 | 当前版 | 变化 | 错误 |
|---|---:|---:|---:|---:|
| HTTP `/fast` QPS | 33,957.99 | 49,459.19 | **+45.65%** | 0 |
| HTTP `/fast` P99 | 418.76 ms | 377.63 ms | **-9.82%** | 0 |
| gRPC Echo QPS | 12,389.40 | 16,389.00 | **+32.28%** | 0 |
| gRPC Echo P99 | 6.176 ms | 4.990 ms | **-19.21%** | 0 |
| gRPC Count QPS | 3,755.20 | 4,631.20 | **+23.33%** | 0 |
| gRPC Upload QPS | 4,843.40 | 6,168.00 | **+27.35%** | 0 |
| gRPC Chat QPS | 2,658.20 | 3,559.00 | **+33.89%** | 0 |

自动比较工具的六项门槛全部为 `PASS`：HTTP QPS、HTTP 零错误、HTTP P99、gRPC Echo QPS、gRPC 零错误、gRPC Echo P99。

另做了一组 4 Reactor + `wrk -t4 -c128` 的同机饱和诊断：当前版 HTTP 中位数 53,100.59 QPS，基线 36,272.07 QPS，但当前版一轮出现 94 个 1 秒 timeout。同机 4 vCPU 同时运行 4 个 Reactor 和 4 个 `wrk` 线程会严重超卖 CPU，所以该组只证明吞吐趋势，不用于“零错误”验收。这个异常也促使脚本补上 socket error 统计，旧脚本只统计非 2xx 会错误地报告 0。

## 11. 当前架构评价与仍需生产验证的点

这一版的架构边界比 Phase 9 清楚：Reactor 快任务、Worker 阻塞隔离、官方 gRPC Core、协议出站单写者和长连接 Session 各自有明确所有者；性能工具也进入仓库，不再只凭一次手工 `wrk` 判断。

它已经通过功能、内存安全和短时性能门槛，但仍不能仅凭这些短测试直接投入关键公网生产。至少还需要：

1. 在最终容器镜像单独运行 TSan，并做 30–60 分钟稳态、过载、慢客户端和连接抖动压测；
2. 将压测端移到独立机器，固定 CPU 亲和性后复核吞吐、P99/P999、内存高水位和恢复时间；
3. 验证独立 gRPC 进程的 readiness、滚动升级和真实负载均衡；
4. 接入进程外指标、日志采样、trace、告警与容量看板；
5. 用正式 CA/mTLS、密钥轮换、细粒度授权和外部限流替代示例配置；
6. 做故障注入：下游超时、客户端半关闭、连接风暴、证书过期、磁盘/文件描述符耗尽；
7. 对 `ReactorSafe` handler 建立代码审查规则和耗时观测，防止后续业务把阻塞调用带回 Reactor。

适用场景：高并发学习项目、内部服务原型、经过压测后的小型单机/容器服务基线。优点是协议与执行边界清楚、快慢路径可选择、gRPC 可独立扩容；代价是多了一种路由执行契约、分片池和 Reactor 引用生命周期，维护者必须理解这些并发不变量。
