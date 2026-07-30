# C++20 多 Reactor 协程 HTTP Server

这是一个面向 Linux 的学习型 HTTP/1.1 服务端：主线程负责 `accept`，连接按轮询分配给多个
SubReactor；每个 SubReactor 拥有独立的线程、`epoll`、连接表、时间轮、段池和协程调度器。
每条连接由一个 C++20 协程串行完成读取、增量解析、路由和非阻塞发送。

> 当前状态：第三阶段对象化（Session/Parser/Sender/Executor/Runtime）已完成；已合入
> `TCP_NODELAY`、SIGINT/SIGTERM 优雅退出与默认最大连接数限流。收口清单见
> [docs/CLOSEOUT.md](docs/CLOSEOUT.md)。虚拟机上已有 `GET /` 压测快照（约 2.3 万 QPS），
> **不是**物理机上限，也未完成正式竞品对比与火焰图。Web 侧停止扩张 WS/HTTP2/io_uring 等协议。

## 阅读目标与设计主线

这份 README 用于先回答“项目能做什么、当前完成到哪里”，再把实现、验证和发展方向串成一条主线。
多 Reactor、协程会话、增量 HTTP 解析和分层发送共同构成当前框架：它们不是为了堆叠技术名词，
而是为了把连接归属、异步等待、协议状态与资源生命周期放到可解释、可验证的边界内。

建议按需继续阅读：

- [架构说明](docs/ARCHITECTURE.md) 解释组件如何协作、为何采用线程内状态与每连接协程，以及这些选择的边界；
- [收口清单](docs/CLOSEOUT.md) 跟踪验证缺口、停扩约定与压测快照；
- [收口阶段 A 改动说明](closeout-A_tcp_nodelay_graceful_shutdown.md) 逐步解释 TCP_NODELAY、优雅退出与连接限流；
- [测试说明](docs/TESTING.md) 把协议、生命周期和并发假设变成可重复的正确性检查；
- [性能实验](docs/BENCHMARK.md) 规定如何用证据评价吞吐、尾延迟与资源开销，避免用未经验证的 QPS 代替结论；
- [机器人路线图](docs/ROBOTICS_ROADMAP.md) 说明如何复用这里积累的 C++、Linux、通信、测试和性能分析能力，
  同时把 Web 收口为稳定作品与轻量诊断入口，不再横向扩张框架。

因此，当前 HTTP Server 的作用既是一个可运行的 Web 系统软件框架，也是后续转向 ROS 2/DDS、
实时通信与设备接入的工程训练底座。Web 侧后续优先补齐配置、优雅停机、安全边界和验证闭环；
机器人侧则复用其线程模型、状态机、背压、可观测性与基准方法，减少转向时从零重建工程习惯。

## 构建与运行

要求 Linux、CMake 3.16+、支持 C++20 的 GCC/Clang 和 pthread。仅构建服务端：

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build build-release --parallel
./build-release/webserver
```

服务固定监听 `0.0.0.0:8080`。请从仓库根目录启动，以便 `/logo` 找到
`./static/logo.svg`。目前没有命令行端口、线程数或配置文件支持。

运行完整测试（需要系统安装 GoogleTest 和 Python 3）：

```bash
bash scripts/run_tests.sh
```

等价入口、Sanitizer 和模糊测试说明见 [docs/TESTING.md](docs/TESTING.md)。架构细节见
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)，可复现压测流程见
[docs/BENCHMARK.md](docs/BENCHMARK.md)。

## 当前路由

- `GET /`：返回 `<h1>hello</h1>`。
- `GET /user/:id`：返回路径参数 `id`。
- `GET /stream1`：返回由 `hello` 和 ` world` 组成的 chunked 响应。
- `GET /stream2`：每秒产生一个 `hello\n`，共 10 次；`sleep(1)` 在 **Executor Worker** 中执行，
  不阻塞 Reactor，但仍占用 Worker 槽位，**不适合作为性能路由**。
- `GET /logo`：发送 `static/logo.svg`，支持现有文件响应中的 Range/ETag 路径。
- `GET /slow`：`sleep(10)`，用于验证慢请求隔离。
- `GET /fast`：应在 `/slow` 期间仍能快速返回。
- 其他路径返回 `404 Not Found`。

全局中间件记录请求日志。访问路径为 `/admin` 且请求头中没有 `token` 时会短路为 401；
当前没有注册 `/admin` 路由，因此携带 token 后会得到 404。Router API 支持注册 GET/POST，
但示例程序目前没有 POST 路由。

## 压测快速入口

脚本支持 `wrk` 和 `wrk2`，会保存环境、完整命令、原始输出和机器可读汇总；可选
`BASELINE_URL` 用同一组参数对比另一个服务：

```bash
# wrk：固定并发
TOOL=wrk THREADS=4 CONNECTIONS=128 DURATION=30s \
  BASELINE_URL=http://127.0.0.1:8081/ \
  bash scripts/benchmark.sh

# wrk2：固定请求速率，必须设置 RATE
TOOL=wrk2 RATE=50000 THREADS=4 CONNECTIONS=128 DURATION=30s \
  bash scripts/benchmark.sh
```

默认目标是 `http://127.0.0.1:8080/`，服务端程序为 `build-release/webserver`。
脚本会检查端口、以独立进程组启动服务、等待就绪，并在退出时回收整个进程组。
结果写入 `benchmark-results/<UTC 时间>/`；该目录不应作为性能结论直接提交，先按
`docs/BENCHMARK.md` 的规范审阅。所有指标来自工具输出和 `ps` 采样，脚本不会填充或推测缺失数据。

## 项目亮点（面试讲述）

1. **线程归属清晰的多 Reactor**：acceptor 只接收连接，通过加锁队列和 `eventfd` 将 fd
   交给 SubReactor；连接表、`epoll_ctl` 和时间轮变更收敛到所属 Reactor 线程，降低共享状态竞争。
2. **协程表达连接状态机**：`ReadAwaiter`/`WriteAwaiter` 把 `EAGAIN` 转换为挂起，
   `EPOLLIN`/`EPOLLOUT` 到来后由调度器去重唤醒；一条连接内请求串行处理，天然保持响应顺序。
3. **增量 HTTP 解析与连接复用**：解析器覆盖 Content-Length、chunked 请求体、Range 和
   半包/流水线残留；已完成请求消费后保留缓冲区中的下一条请求。
4. **显式响应所有权**：响应从对象池借出，发送成功、连接消失或失败路径均归还；
   协程帧由调度器统一 `adopt`/`reap`，避免悬挂句柄被提前销毁。
5. **分层发送路径**：内存响应使用 `writev`，文件响应按大小选择缓冲、mmap 或
   `sendfile`，写阻塞后等待可写事件继续发送。
6. **可复现实验材料**：压测脚本同时记录吞吐、平均/P95/P99 延迟、错误、RSS/CPU、
   环境和命令，支持同机同参数 baseline 对比，且明确区分观测值与解释。

## 边界与未完成项

- 仅支持 Linux `epoll`/`eventfd`/`sendfile`；**刻意不做** TLS、HTTP/2、WebSocket、io_uring（见 CLOSEOUT）。
- 端口等仍多为默认值；可用 `setPort` / `setReactorCount` / `setMaxConnections`。
  SIGINT/SIGTERM 会停止 accept 并 `stop`/`join` SubReactor（不再 detach）。
- 业务 handler 在共享 **Executor** 上执行；队列满返回 503。空路径也过 Executor，峰值低于“Reactor 内联回包”。
- 每连接同一时刻只处理一个请求；可保留 pipeline 输入，但不并行执行。
- 读侧 1 MiB 水位；写侧依赖 `EAGAIN`；accept 侧默认最大连接 10000。
- 畸形/超限请求当前直接断开，未统一返回 400/413。
- 正式竞品对比、火焰图与物理机长稳仍待补；虚拟机快照见 [docs/CLOSEOUT.md](docs/CLOSEOUT.md)。

## 目录

```text
server/                 Reactor、协程、HTTP、路由、缓冲和响应实现
tests/                  GoogleTest 单测与 Linux HTTP 黑盒测试
fuzz/                   HttpParser libFuzzer 入口
scripts/run_tests.sh    构建并运行测试
scripts/benchmark.sh    wrk/wrk2 可复现压测与进程采样
scripts/wrk_report.lua  精确导出平均/P95/P99 和错误计数
docs/                   测试、架构、压测方法、收口清单与机器人路线图
static/logo.svg         /logo 的最小文本演示资源
```
