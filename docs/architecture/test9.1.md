# test9.1 版本差异：HTTP/gRPC 架构与性能升级

> 对比基线：`test9.0` 分支提交 `fa15039`
>
> 当前版本：`test9.1`
>
> 详细实现讲解：[Phase 10：HTTP/gRPC 性能路径与架构拆分](phase10.md)

## 1. 版本定位

`test9.0` 完成了 gRPC Callback API、跨协议认证、资源限制、SSE 重放和优雅停机。`test9.1` 不再增加新的业务协议，重点是整理运行边界并优化 HTTP 与 gRPC 的公共热路径。

可以把两个版本理解为：

- `test9.0` 先把 WebSocket、SSE、HTTP/2、TLS 和 gRPC 等“车道”修完整；
- `test9.1` 再改造收费站、调度室和车辆复用区，减少线程切换、全局锁和无效分配。

## 2. 架构差异总览

| 维度 | test9.0 | test9.1 | 改动目的 |
|---|---|---|---|
| HTTP handler 执行 | 所有 handler 都进入 Worker | 默认仍进 Worker；审计过的短路由可标记 `ReactorSafe` | 让 `/fast`、健康检查等短任务省掉线程池排队与 `eventfd` 往返 |
| Router 生命周期 | 启动后仍没有显式冻结状态 | 监听前 `freeze()`，运行期只读 | 避免并发注册导致容器扩容和已解析路由指针失效 |
| 静态路由索引 | 每次构造 `"METHOD path"` 查询键 | `method → path → RouteEntry` 双层索引 | 减少热路径临时字符串 |
| 中间件 continuation | 递归 `std::function` | 状态指针 + 普通函数指针 | 避免每请求构造拥有型闭包 |
| 对象池与 Buffer 池 | 全局容器和单锁 | 64 分片、线程本地优先、跨分片窃取 | 降低多个 Reactor 在同一把锁上的竞争 |
| 运行指标 | 每项一个全局 atomic | 64 分片 `StripedCounter`，抓取时汇总 | 降低多核写同一缓存行的伪共享 |
| HTTP 响应头 | 多次字符串拼接和 `to_string` | `to_chars` + 直接 `Buffer::append` | 减少分配、复制和小字符串临时对象 |
| gRPC 部署 | 只随 `webserver` 集成启动 | 保留集成模式，新增独立 `webserver-grpc` | 支持 Web/gRPC 独立扩容、配额和故障隔离 |
| gRPC 服务端总时限 | 依赖后续读写回调才能检查 | Count、Upload、Chat 各自使用独立 `grpc::Alarm` | 即使客户端静默，也能主动结束超时 RPC |
| gRPC Reactor 销毁 | `OnDone()` 直接删除或旧 Timer 状态间接保活 | gRPC 与 Alarm 双引用退场后销毁 | 防止异步定时回调访问已释放 Reactor |
| 认证解析 | 动态容器拆分 token | 定长 `array<string_view, 5>`；跨 API 返回拥有型字符串 | 减少切分分配，同时避免临时输入产生悬空视图 |
| 性能验证 | 主要依赖手工 `wrk` | 仓库内 HTTP/gRPC 五轮基准和自动门槛比较 | 让 QPS、错误数和 P99 可复现、可审查 |

## 3. HTTP 调用链变化

### test9.0

```text
HTTP request
  → Parser
  → Router
  → HTTP Executor queue
  → Worker handler
  → completion eventfd
  → Reactor resume
  → encode / sendQueue
```

即使 handler 只返回 `"fast"`，也必须完整走一次 Worker 往返。

### test9.1

```text
HTTP request
  → Parser
  → Router::resolve
       ├─ Worker（默认，阻塞/复杂/未知任务）
       │    → Executor → completion eventfd → Reactor
       └─ ReactorSafe（显式标记的有界短任务）
            → Reactor 内直接执行
  → encode / OutboundQueue / Writer
```

当前只有 `/`、`/fast`、`/health/live`、`/health/ready` 和 `/user/:id` 被标记为 `ReactorSafe`。文件访问、SSE、WebSocket、指标生成和慢业务仍走 Worker。

`ReactorSafe` 不是“更高级的默认值”。它像前台当场处理的小业务：只能做确定时间内完成、不会阻塞、不会等待外部资源的工作。若把数据库查询或磁盘 I/O 标成 `ReactorSafe`，同一 Reactor 上的其他连接都会被拖住。

## 4. gRPC 调用链变化

### 部署边界

```text
test9.0
webserver process
  ├─ Web Reactor / Executor
  └─ gRPC Core / CallbackService

test9.1 可选隔离模式
webserver                         webserver-grpc
  └─ Web Reactor / Executor         └─ gRPC Core / CallbackService
```

集成模式仍然可用；压测和生产容量隔离时可设置 `WEB_GRPC_ADDRESS=off` 启动 Web，再单独启动 `webserver-grpc`。

### 流式 RPC 生命周期

Count、Upload、Chat 现在都有服务端总时限 Alarm。Reactor 同时持有两类生命周期引用：

```text
初始 gRPC 引用 = 1
注册 Alarm      +1

OnDone
  → 在 CallbackServerContext 仍有效时完成指标和配额归还
  → Cancel Alarm
  → 释放 gRPC 引用

Alarm callback
  → 必要时 Finish(DEADLINE_EXCEEDED)
  → 释放 Alarm 引用

最后一个引用离开
  → delete Reactor
```

Linux 首轮测试曾发现：Alarm 可能让 C++ Reactor 活得比 `CallbackServerContext` 更久，若析构时再调用 `IsCancelled()` 就会访问失效上下文。`test9.1` 把协议完成固定在 `OnDone()` 内，析构阶段只释放普通 C++ 对象。

## 5. 新增配置和目标

| 名称 | 默认值 | 说明 |
|---|---:|---|
| `WEB_HTTP_WORKERS` | 自动 | HTTP Worker 数量 |
| `WEB_WS_WORKERS` | 自动 | WebSocket Worker 数量 |
| `WEB_GRPC_MAX_RPC_MS` | 30000 | gRPC 服务端总时限 |
| `WEB_GRPC_MAX_RECEIVE_BYTES` | 1 MiB | gRPC 单消息接收上限 |
| `WEB_GRPC_MAX_SEND_BYTES` | 1 MiB | gRPC 单消息发送上限 |
| `webserver-grpc` | 新目标 | 独立 gRPC 服务进程 |
| `grpc_benchmark` | 新目标 | Echo/Count/Upload/Chat 并发基准客户端 |

构建基准目标需要同时开启：

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DWEBSERVER_ENABLE_GRPC=ON \
  -DWEBSERVER_BUILD_BENCHMARKS=ON
```

## 6. 同机实测结果

环境：CentOS Stream 9、4 vCPU、GCC 11.5、OpenSSL 3.5.5、gRPC C++ 1.82.0。基线与当前版在同一台虚拟机、相同 Release 参数下执行五轮，取中位数。

| 指标 | test9.0 | test9.1 | 变化 | test9.1 错误数 |
|---|---:|---:|---:|---:|
| HTTP `/fast` QPS | 33,957.99 | 49,459.19 | **+45.65%** | 0 |
| HTTP `/fast` P99 | 418.76 ms | 377.63 ms | **-9.82%** | 0 |
| gRPC Echo QPS | 12,389.40 | 16,389.00 | **+32.28%** | 0 |
| gRPC Echo P99 | 6.176 ms | 4.990 ms | **-19.21%** | 0 |
| gRPC Count QPS | 3,755.20 | 4,631.20 | **+23.33%** | 0 |
| gRPC Upload QPS | 4,843.40 | 6,168.00 | **+27.35%** | 0 |
| gRPC Chat QPS | 2,658.20 | 3,559.00 | **+33.89%** | 0 |

自动门槛检查全部通过：HTTP QPS 至少提升 20%、gRPC Echo 至少提升 15%、错误数为 0、P99 不恶化超过 10%。

## 7. 验证结果

| 验证 | 结果 |
|---|---:|
| Linux Release CTest | 84/84 通过 |
| Linux Debug CTest | 84/84 通过 |
| Linux ASan + LSan + UBSan CTest | 84/84 通过 |
| Bash 基准脚本语法 | 通过 |
| Python 比较工具语法与合成数据 | 通过 |
| `git diff --check` | 通过 |

Sanitizer 和 Debug 断言额外发现并修复了两个所有权问题：gRPC Alarm/Context 的销毁顺序，以及认证提取结果的 `string_view` 生命周期。

## 8. 关键文件索引

| 文件 | 作用 |
|---|---|
| [`Router.h`](../../server/Route/Router.h)、[`Router.cpp`](../../server/Route/Router.cpp) | `ExecutionPolicy`、`resolve()`、`freeze()` 和低分配中间件链 |
| [`HttpSession.cpp`](../../server/http/HttpSession/HttpSession.cpp) | ReactorSafe/Worker 分流和统一 handler 收尾 |
| [`ObjectPool.h`](../../server/ObjectPool/ObjectPool.h)、[`BufferPoll.cpp`](../../server/buffer_pool/BufferPoll.cpp) | 64 分片复用池 |
| [`StripedCounter.h`](../../server/ops/StripedCounter.h)、[`OperationalMetrics.h`](../../server/ops/OperationalMetrics.h) | 分片指标和快照 |
| [`http.cpp`](../../server/http/http.cpp) | 响应头直接编码到 Buffer |
| [`GrpcServer.cpp`](../../server/grpc/GrpcServer.cpp) | gRPC 准入、deadline Alarm 和 Reactor 生命周期 |
| [`GrpcMain.cpp`](../../server/grpc/GrpcMain.cpp) | 独立 gRPC 进程入口 |
| [`grpc_benchmark.cpp`](../../benchmarks/grpc_benchmark.cpp) | 四种 RPC 基准客户端 |
| [`benchmark_phase10.sh`](../../scripts/benchmark_phase10.sh) | HTTP/gRPC 五轮基准和错误汇总 |
| [`compare_phase10_results.py`](../../scripts/compare_phase10_results.py) | 自动验收 QPS、错误数与 P99 |
| [`phase10.md`](phase10.md) | 全部设计理由、风险、命令和测试过程 |

## 9. 兼容性与使用建议

- 已有路由默认仍是 `Worker`，旧业务不需要修改执行策略。
- 集成式 gRPC 启动方式继续可用；`webserver-grpc` 是新增的隔离选项。
- Router 必须在 `ServerRuntime::start()` 前完成注册；启动后继续注册会抛出 `logic_error`。
- 自定义 `ReactorSafe` 路由必须经过代码审查和耗时监控。
- 当前版本已经通过短时功能、内存安全和性能门槛；关键公网生产仍应补充独立压测机、TSan、长时间稳态压测、故障注入、正式 mTLS/密钥轮换和外部可观测系统。

学习时建议先看第 3 节理解为什么短 handler 可以直达 Reactor，再看第 4 节理解 gRPC 的 `OnDone + Alarm + 引用计数`。这两个部分分别代表本阶段最主要的性能收益和最容易出错的生命周期边界。
