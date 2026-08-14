# webserver

一个基于 **C++20 协程 + epoll + Multi-Reactor** 的 HTTP/WebSocket 服务器项目。  
它不只是“能跑”的 Demo，而是把 **Reactor 线程模型、协程调度、协议编解码、会话生命周期、WebSocket 读写分离、测试与性能实验** 串成了一条可学习、可复现、可继续演进的工程链路。

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey)
![Build](https://img.shields.io/badge/build-CMake-green)
![Protocol](https://img.shields.io/badge/protocol-HTTP%20%2F%20WebSocket-orange)

> 说明：项目目标是 Linux 环境运行；Windows 可用于编辑代码，实际构建与运行建议使用 Linux 或 WSL。

---

## 项目亮点

| 亮点 | 说明 |
|---|---|
| C++20 协程原生异步 | 自研 `Task` / `Awaiter` / `CoroutineScheduler`，用 `co_await` 表达读、写、业务执行等待 |
| Multi-Reactor 线程模型 | 主线程 accept，多个 `SubReactor` 分管连接，连接资源由所属 Reactor 独占，减少锁竞争 |
| HTTP/WebSocket 双协议 | `Session` 抽象统一承载 `HttpSession` 与 `WebSocketSession`，支持 HTTP Upgrade 到 WebSocket |
| WebSocket 完整协议栈 | 握手、SHA-1/Base64、增量帧解析、掩码处理、分片重组、Ping/Pong、Close 握手 |
| WebSocket 读写协程分离 | 读协程负责收帧与业务分发，写协程负责出站队列冲刷与 EPOLLOUT 续传 |
| 消息分发与在线会话管理 | `WebSocketDispatcher` 按消息 `type` 分发，`WebSocketSessionManager` 支持按 uid 跨 Reactor 推送 |
| 工程化验证闭环 | 单元测试、HTTP 黑盒测试、Sanitizer、libFuzzer、benchmark 脚本与性能实验文档 |
| 教学型代码与文档 | 大量架构注释、阶段文档、测试学习笔记，适合作为网络编程与服务器架构学习样本 |

---

## 当前架构

```text
ServerRuntime
  ├── Router                    # HTTP 路由
  ├── WebSocketDispatcher       # WebSocket 消息按 type 分发
  ├── WebSocketSessionManager   # uid -> session / reactor / fd 映射
  ├── Executor                  # 业务线程池
  └── ReactorGroup
        └── SubReactor x N
              ├── epoll loop
              ├── conns
              ├── TimerWheel
              ├── CoroutineScheduler
              ├── enqueueWrite / flushWriteQueue
              └── Connection::session
                        |
                  +-----+-----+
                  |           |
            HttpSession   WebSocketSession
                  |           |
        HttpParser/Codec    WebSocketParser/Codec
        ResponseSender      WebSocketSender
```

### HTTP 请求链路

```text
accept
  -> HttpSession::run
  -> HttpCodec::decode
  -> HttpCodec::dispatch(Router)
  -> HttpResponse
  -> HttpCodec::encode
  -> ResponseSender::send
```

### WebSocket 链路

```text
HTTP Upgrade /ws
  -> HttpSession 发送 101
  -> Connection::session 切换为 WebSocketSession
  -> WebSocketSession::run
       ├── readLoop: decode -> dispatch -> enqueueOutbound
       └── writeLoop: wait outbound -> flushWriteQueue -> wait EPOLLOUT
```

---

## 快速开始

### 依赖

```bash
sudo apt update
sudo apt install build-essential cmake python3 libgtest-dev
```

### 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
```

### 运行测试

```bash
ctest --test-dir build --output-on-failure
```

也可以直接跑完整测试脚本：

```bash
bash scripts/run_tests.sh
```

### 启动服务

```bash
./build/webserver
```

默认监听端口：`8080`

---

## HTTP 示例

项目内置了一些演示路由：

```bash
curl http://127.0.0.1:8080/
curl http://127.0.0.1:8080/user/42
curl http://127.0.0.1:8080/fast
curl http://127.0.0.1:8080/stream1
curl http://127.0.0.1:8080/logo
```

其中：

- `/`：基础 HTML 响应
- `/user/:id`：动态路由参数
- `/stream1` / `/stream2`：chunked 响应
- `/logo`：静态文件与 Range 请求
- `/slow` / `/fast`：验证 Executor 异步执行，慢请求不阻塞快请求

---

## WebSocket 示例

WebSocket 入口：

```text
ws://127.0.0.1:8080/ws?uid=1001
```

连接时会通过 `uid` 注册到 `WebSocketSessionManager`，方便做点对点推送。

### 消息格式

支持两种输入形式：

```json
{"type":"chat","to":1002,"content":"hello"}
```

或简写：

```text
@1002:hello
```

默认未匹配 `type` 的消息会走 echo 回显。

### 使用 websocat 测试

```bash
websocat "ws://127.0.0.1:8080/ws?uid=1001"
websocat "ws://127.0.0.1:8080/ws?uid=1002"
```

然后在 `1001` 侧发送：

```text
@1002:hello
```

或：

```json
{"type":"chat","to":1002,"content":"hello"}
```

---

## 设计纪律

这个项目比较重视几条工程不变量：

1. **Reactor 线程独占连接表**  
   `SubReactor` 管自己的 `conns`，跨线程投递通过 eventfd / pending queue 回到所属 Reactor 线程处理。

2. **协程挂起期间不缓存 Connection 指针**  
   每次 `co_await` 恢复后重新查表，避免连接关闭后访问悬空对象。

3. **协议栈下沉到 Session**  
   HTTP 与 WebSocket 各自持有 Parser / Codec / Sender，Reactor 不堆协议细节。

4. **读写职责分离**  
   WebSocket 已拆成 `readLoop` 与 `writeLoop`，读阻塞时不影响业务侧主动推送。

5. **先正确，再性能**  
   测试、Sanitizer、黑盒验证优先，性能实验单独记录，不用偶然峰值冒充稳定能力。

---

## 测试与验证

项目把验证分成几层：

- **单元测试**：Buffer、HttpParser、Router、WebSocket 握手/编解码/解析等
- **黑盒测试**：启动真实服务，验证 HTTP 行为
- **Sanitizer**：ASan / UBSan / TSan 检查内存与并发问题
- **Fuzzing**：libFuzzer 压 HTTP 增量解析器
- **性能实验**：wrk / wrk2 + 环境记录 + 原始输出留存

相关文档：

- [docs/testing/TESTING.md](docs/testing/TESTING.md)
- [docs/performance/BENCHMARK.md](docs/performance/BENCHMARK.md)
- [docs/architecture/phase4_upgrade.md](docs/architecture/phase4_upgrade.md)
- [docs/architecture/ARCHITECTURE.md](docs/architecture/ARCHITECTURE.md)

---

## 当前边界

为了保持 README 诚实，这里也写清楚当前还没做完的部分：

- HTTP 发送路径还没有完全并入统一写队列模型
- WebSocket 写路径已经做了读写协程分离，但仍有进一步收敛空间
- 还没有 TLS、配置文件体系、完整 Metrics 面板
- 项目偏教学与架构验证，不是直接拿去替代 nginx/envoy 的生产级网关

---

## 未来展望

### 近期优化

- 统一 HTTP/WebSocket 出站路径，进一步收敛写职责
- 补齐出站背压与关闭所有权策略
- 将部分 WebSocket 业务 handler 接入 Executor，避免重业务占用 Reactor
- 增强 WebSocket 消息解析能力，减少 Codec 层业务解析痕迹

### 协议演进

- SSE
- HTTP/2
- gRPC
- TLS

### 工程化方向

- Metrics / Logging / Tracing
- CI 流水线
- Docker 化部署
- 更完整的 benchmark 报告与回归对比

### 更长期方向

这个项目后续也可以继续往机器人/实时通信方向延展，例如设备网关、实时遥测、ROS 2/DDS 通信验证等。  
对应路线文档见：

- [docs/roadmap/ROBOTICS_ROADMAP.md](docs/roadmap/ROBOTICS_ROADMAP.md)

---

## 仓库结构

```text
.
├── main.cpp
├── CMakeLists.txt
├── server/
│   ├── Runtime/
│   ├── Reactor/
│   ├── SubReactor/
│   ├── CoroutineScheduler/
│   ├── session/
│   ├── protocol/
│   ├── http/
│   ├── websocket/
│   ├── transport/
│   ├── timer/
│   └── ...
├── tests/
├── scripts/
├── docs/
└── static/
```

---

## 适合谁看

这个项目适合：

- 想学 **Linux 网络编程 / epoll / Reactor** 的人
- 想学 **C++20 协程在服务器中的真实用法** 的人
- 想理解 **HTTP 与 WebSocket 如何共存于同一套连接生命周期** 的人
- 想把“写服务器”从玩具 Demo 推进到 **可测试、可验证、可演进架构** 的人

---

## 一句话总结

这是一个以教学清晰度为优先级、以真实工程结构为骨架的 C++20 协程 Web 服务器项目：  
**既能读，也能跑；既能做协议实验，也能继续扩成更完整的实时通信基础设施。**
