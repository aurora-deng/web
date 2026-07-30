# 收口阶段 A：TCP_NODELAY、优雅退出与连接背压

**变更代号**：`closeout-A`  
**日期**：2026-07-29（含同日后续补丁：早释端口、`main` 捕获异常、Ctrl+Z 说明）  
**范围**：在第三阶段对象化已完成的前提下，补齐延迟地板修复、可回收停机、accept 限流，并落盘压测与停扩约定。  
**对照文档**：
- 总清单：[docs/CLOSEOUT.md](docs/CLOSEOUT.md)
- 第三阶段实现：[phase3_architecture_upgrade.md](phase3_architecture_upgrade.md)
- 架构说明：[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)

---

## 一、为什么叫「收口阶段 A」

此前链路是：

```text
Session/Parser → Executor/Runtime → 取消/背压/优雅退出/测试
→ Sanitizer/故障注入 → 压测/P99 → 设计文档 → Web 停扩 → ROS2…
```

第三阶段已把 **Session / Parser / Sender / Executor / Runtime** 拆清。  
本变更不扩张协议（不做 WebSocket / io_uring），只做收口链上的 **A 切片**：

| 切片 | 内容 |
|------|------|
| A1 | 修复 ~40ms 延迟地板（`TCP_NODELAY`） |
| A2 | 信号驱动优雅退出（accept + Reactor 可 join） |
| A3 | accept 侧最大连接数软限流 |
| A4 | 文档与压测快照对齐，明确 Web 停扩 |
| A5（后续补丁） | `releaseListener` 早释端口；`main` try/catch 避免核心转储；文档澄清 Ctrl+Z |

后续若继续：`closeout-B` = Linux 测试/Sanitizer 跑绿；`closeout-C` = 火焰图/竞品正式对比。

---

## 二、改动文件清单（含后续补丁）

| 文件 | 动作 | 为何改 |
|------|------|--------|
| `server/Runtime/ServerRuntime.h` | 修改 | 增加 `requestStop` / `releaseListener` / `maxConnections_` / `wakeFd_` / `running_` |
| `server/Runtime/ServerRuntime.cpp` | 修改 | NODELAY、信号、wake、限流、早关 listen、可退出 accept |
| `server/SubReactor/SubReactor.h` | 修改 | `activeConns_` 与 `activeConnections()` |
| `server/SubReactor/SubReactor.cpp` | 修改 | loop 看 `running`；建连/关连计数；停机丢弃 pending |
| `server/Reactor/ReactorGroup.h/.cpp` | 修改 | `activeConnections()` 汇总全组 |
| `main.cpp` | 修改 | try/catch，启动失败不核心转储，并提示 Ctrl+Z |
| `docs/CLOSEOUT.md` | 新增 | 收口总清单 + VM 压测快照 |
| `README.md` / `docs/BENCHMARK.md` | 修改 | 去掉过时描述，指向快照与停扩 |
| 本文 | 维护 | 记录全部修改、理由、逻辑与代码片段 |

---

## 三、首轮修改：逐步说明 + 逻辑 + 代码

### 步骤 1：对 `newfd` 设置 `TCP_NODELAY`

**为何改**  
压测发现低并发延迟锁在 ~41ms，QPS≈连接数/0.041。根因是 Nagle + Delayed ACK；响应头/体两次 `writev` 时极易触发。  
虚拟机上曾误写 `setsockopt(fd, …)`（`fd` 是 listen），无效；必须对 **`newfd`**。

**逻辑**

```text
accept4 成功得到客户端连接 newfd
  → setsockopt(newfd, IPPROTO_TCP, TCP_NODELAY)
  → 之后小包可立即发出，不再等 ~40ms ACK
```

**代码**（`ServerRuntime::acceptLoop`）：

```cpp
int newfd = accept4(listenFd_, ..., SOCK_NONBLOCK | SOCK_CLOEXEC);
// ...

// 关闭 Nagle，避免小响应头/体分写触发 Delayed ACK ~40ms 地板。
int yes = 1;
if (setsockopt(newfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == -1)
{
    LOG_ERROR(std::string("TCP_NODELAY: ") + strerror(errno));
    close(newfd);
    continue;
}
```

需 `#include <netinet/tcp.h>`。

**验收**（`wrk -t2 -c16`）：~385 QPS / P50~41ms → ~23329 QPS / P50~0.56ms。

---

### 步骤 2：`wakeFd_`（eventfd）挂进 acceptor epoll

**为何改**  
`epoll_wait(epfd_, …, -1)` 无事件会永久阻塞。信号到达时若不能唤醒主线程，就无法退出 accept 循环。

**逻辑**

```text
setupListener 创建 wakeFd_ = eventfd(非阻塞)
  → epoll_ctl ADD wakeFd_
信号/停机 → write(wakeFd_) → epoll_wait 返回 → 主线程看见 running_==false 退出
```

**代码**：

```cpp
wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
// ...
ev.events = EPOLLIN;
ev.data.fd = wakeFd_;
epoll_ctl(epfd_, EPOLL_CTL_ADD, wakeFd_, &ev);
```

accept 循环里：

```cpp
if (fd == wakeFd_)
{
    uint64_t cnt = 0;
    while (read(wakeFd_, &cnt, sizeof(cnt)) > 0)
        ;
    continue;
}
```

---

### 步骤 3：`requestStop()` + 注册 SIGINT/SIGTERM

**为何改**  
需要 Ctrl+C / `kill -TERM` 走可控停机，而不是只能 `kill -9`。  
信号处理函数只能做 async-signal-safe 操作，不能在里面 `join`、打日志、关一堆带锁资源。

**逻辑**

```text
start()：g_runtime = this；signal(SIGINT/SIGTERM, handleStopSignal)
信号 → handleStopSignal → requestStop()
       requestStop：running_=false；write(wakeFd_)   // 仅此
主线程被唤醒后做真正的收尾
```

**代码**：

```cpp
namespace {
std::atomic<ServerRuntime *> g_runtime{nullptr};

void handleStopSignal(int)
{
    if (auto *runtime = g_runtime.load(std::memory_order_acquire))
        runtime->requestStop();
}
}

void ServerRuntime::requestStop()
{
    running_.store(false, std::memory_order_release);
    if (wakeFd_ >= 0)
    {
        uint64_t one = 1;
        (void)write(wakeFd_, &one, sizeof(one));
    }
}

// start() 中：
g_runtime.store(this, std::memory_order_release);
std::signal(SIGINT, handleStopSignal);
std::signal(SIGTERM, handleStopSignal);
```

头文件新增成员示意：

```cpp
void requestStop();
void releaseListener();
void setMaxConnections(size_t n);

size_t maxConnections_ = 10000;
int wakeFd_ = -1;
std::atomic<bool> running_{true};
```

---

### 步骤 4：accept 循环可退出

**为何改**  
原先 `while (true)` 永不离开，`start()` 无法返回，进程只能被强杀。

**逻辑**：每次 `epoll_wait` 前后检查 `running_`；为 false 则跳出，进入收尾。

**代码**：

```cpp
void ServerRuntime::acceptLoop()
{
    epoll_event events[1024];
    while (running_.load(std::memory_order_acquire))
    {
        int n = epoll_wait(epfd_, events, 1024, -1);
        // ...
        if (!running_.load(std::memory_order_acquire))
            break;
        // 处理 wakeFd_ / listenFd_ ...
    }
}
```

---

### 步骤 5：修复 `SubReactor::loop` 的 `while (true)`

**为何改**  
`stop()` 已清 `running` 并写 eventfd，但 loop 若仍是 `while (true)`，线程永不退出 → `join()` 永久挂起，优雅退出名存实亡。

**逻辑**：与 Runtime 相同——事件循环条件绑定 `running`；`stop()` 写 eventfd 保证及时醒来检查标志。

**代码**：

```cpp
void SubReactor::loop()
{
    // stop() 会清 running 并写 eventfd；必须在每次 wait 后检查，否则 join() 永久挂起。
    while (running.load(std::memory_order_acquire))
    {
        int n = epoll_wait(epfd, events, MAX_EVENTS, 100);
        if (!running.load(std::memory_order_acquire))
            break;
        // ...
    }
}
```

---

### 步骤 6：停机后丢弃 pending fd

**为何改**  
停机窗口里可能已有 fd 进入 `pendingFds` 队列；若仍建连，会留下「服务已停但仍有连接对象」的半状态。

**逻辑**：`processPendingFds` 若 `!running`，对取出的 fd 直接 `close`，不建 `Connection`。

**代码**：

```cpp
while (!local.empty())
{
    int fd = local.front();
    local.pop();

    if (!running.load(std::memory_order_acquire))
    {
        close(fd);
        continue;
    }
    // epoll_ctl ADD + 创建 Connection + 启动协程 ...
}
```

`addFd` 在 `!running` 时本来就会 `close(fd)` 返回，两边闭环。

---

### 步骤 7：活跃连接计数 + 默认最大 10000

**为何改**  
读侧有 1MiB、Executor 满有 503，但 accept 无上限时仍可能把 fd/内存打满。需要 accept 侧软限流。

**逻辑**

```text
processPendingFds 建连成功 → activeConns_++
fd_close 删连接           → activeConns_--
ReactorGroup 对各 SubReactor 求和
accept 后若 total >= maxConnections_ → close(newfd)，不分发
```

**代码**：

```cpp
// SubReactor.h
std::atomic<size_t> activeConns_{0};
size_t activeConnections() const {
    return activeConns_.load(std::memory_order_relaxed);
}

// 建连
conns.emplace(fd, std::move(conn));
activeConns_.fetch_add(1, std::memory_order_relaxed);

// 关连（fd_close）
conns.erase(it);
activeConns_.fetch_sub(1, std::memory_order_relaxed);

// ReactorGroup.cpp
size_t ReactorGroup::activeConnections() const
{
    size_t total = 0;
    for (const auto &reactor : reactors_)
        total += reactor->activeConnections();
    return total;
}

// acceptLoop
if (maxConnections_ > 0 &&
    reactorGroup_->activeConnections() >= maxConnections_)
{
    close(newfd);
    continue;
}
```

---

### 步骤 8：文档收口

**为何改**  
README 仍写「handler 在 Reactor 同步」「线程 detach」「无压测数据」，与代码和实测不符；需要停扩约定与 VM 快照，避免继续堆协议。

**产物**：`docs/CLOSEOUT.md`、更新 `README.md` / `BENCHMARK.md`、本文。

---

## 四、后续补丁（Ctrl+C 完善 / Ctrl+Z 踩坑之后）

压测与使用中发现两类问题，追加 A5：

1. Ctrl+Z 挂起旧进程 → 再启动 `bind: Address already in use` → 未捕获异常 → **核心转储**。  
2. Ctrl+C 路径若等 Reactor `join` 才关 listen，端口释放偏晚；需要 **accept 退出后立刻关 listen**。

### 补丁 A5-1：`releaseListener()` 早释端口

**为何改**  
`join` 可能较慢；若 listen 仍开着，用户立刻重启仍可能撞端口。停机语义应是：**先不再听、尽快还端口，再收线程**。

**逻辑**（插入在 `acceptLoop` 返回之后、`stop/join` 之前）：

```text
acceptLoop 退出
  → releaseListener()：epoll_ctl DEL + close(listenFd_)；listenFd_=-1
  → reactorGroup_->stop()/join()
  → 打印 server stopped.
```

**代码**：

```cpp
void ServerRuntime::releaseListener()
{
    // 尽早关闭 listen，把 8080 还给系统；不必等 Reactor join 结束。
    if (listenFd_ >= 0)
    {
        if (epfd_ >= 0)
            (void)epoll_ctl(epfd_, EPOLL_CTL_DEL, listenFd_, nullptr);
        close(listenFd_);
        listenFd_ = -1;
    }
}

void ServerRuntime::start()
{
    setupListener();
    createReactors();
    g_runtime.store(this, std::memory_order_release);
    std::signal(SIGINT, handleStopSignal);
    std::signal(SIGTERM, handleStopSignal);

    acceptLoop();

    // Ctrl+C 路径：立刻释放监听端口，再回收 Reactor
    releaseListener();

    if (reactorGroup_)
    {
        reactorGroup_->stop();
        reactorGroup_->join();
    }
    g_runtime.store(nullptr, std::memory_order_release);
    std::cout << "server stopped." << std::endl;
}
```

析构里若 `listenFd_ >= 0` 仍会再关一次（幂等：已 `-1` 则跳过）。

---

### 补丁 A5-2：`main` try/catch，避免核心转储

**为何改**  
`setupListener` 里 `bind` 失败抛 `std::runtime_error`。若 `main` 不接：

```text
uncaught exception → std::terminate → abort → 「已放弃 (核心已转储)」
```

用户误以为是框架崩溃；实际只是端口被 **Ctrl+Z 挂起的旧进程** 占用。

**逻辑**：启动路径包在 try 中；失败打印 `what()` + 操作提示，`return 1`。

**代码**（`main.cpp`）：

```cpp
int main()
{
    try
    {
        ServerRuntime server;
        // ... 注册路由 ...
        server.start();
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << "webserver failed to start: " << ex.what() << "\n"
                  << "Hint: Ctrl+Z only suspends the process and keeps port 8080; "
                     "use Ctrl+C / kill -TERM, or: fg then Ctrl+C, "
                     "or kill $(pgrep -n webserver)\n";
        return 1;
    }
}
```

---

## 五、完整停机 / 建连数据流

### 5.1 Ctrl+C / SIGTERM（正确路径）

```text
SIGINT/SIGTERM
  → handleStopSignal
  → requestStop()：running_=false + write(wakeFd_)
  → acceptLoop 被唤醒并退出
  → releaseListener()：立刻 close(listen) 释放 8080
  → reactorGroup_->stop()：各 SubReactor running=false + eventfd
  → 各 loop 退出 → join
  → 打印 server stopped. → main 返回 0
  → 析构 drain Executor、关 wake/epoll
```

### 5.2 Ctrl+Z（错误用法，不要当退出）

```text
SIGTSTP → 内核挂起进程（不调用 requestStop）
  → listen 仍被该进程持有
  → 再开 ./webserver → bind 失败
  → （补丁前）核心转储；（补丁后）打印错误提示并 return 1
```

| 按键 | 信号 | 行为 | 释放 8080？ |
|------|------|------|-------------|
| Ctrl+C | SIGINT | 优雅退出 | 是 |
| Ctrl+Z | SIGTSTP | **仅挂起** | **否** |
| kill -TERM | SIGTERM | 同 Ctrl+C | 是 |

误按 Ctrl+Z 后恢复：

```bash
jobs
fg                 # 再 Ctrl+C
# 或
kill $(pgrep -n webserver)
ss -ltnp | grep 8080
```

### 5.3 新连接

```text
accept4 → newfd
  → TCP_NODELAY(newfd)
  → 若 activeConnections >= max → close
  → dispatch → addFd → processPendingFds → activeConns_++
```

---

## 六、Ctrl+C「干净退出」设计理由（汇总）

1. **信号里只唤醒**：保证 async-signal-safe；真正收尾在主线程。  
2. **eventfd 唤醒**：否则 `epoll_wait(-1)` 醒不过来。  
3. **先停 accept、早关 listen**：端口尽快可复用。  
4. **SubReactor loop 必须看 `running`**：否则 `join` 死等。  
5. **启动失败要 catch**：端口占用是运维问题，不应 abort 出 core。  
6. **尚未做在途排空**：当前是停听后强制停 Reactor；更强优雅退出留给后续切片。

---

## 七、虚拟机如何验证本批次（含补丁）

```bash
cd web
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-release --parallel
./build-release/webserver

# 延迟：期望 P50 亚毫秒
wrk -t2 -c16 -d30s --latency http://127.0.0.1:8080/

# 干净退出：期望 "server stopped."，8080 释放，可立刻重启
# 前台按 Ctrl+C，或：
kill -TERM $(pgrep -n webserver)

# 不要用 Ctrl+Z 当退出；若已误按：
fg   # 再 Ctrl+C
```

VM 压测快照（NODELAY 修复后）见 [docs/CLOSEOUT.md](docs/CLOSEOUT.md)：约 **2.0万～2.3万 QPS**（该虚拟机上限，非框架理论极限）。

---

## 八、本批次不包含（避免范围膨胀）

- 通用 cancellation token  
- 进程级内存预算 / 每 IP 限流  
- 在途请求排空超时  
- 把 Ctrl+Z 改成退出（刻意不改：保持 shell 作业控制语义；用文档约束）  
- Sanitizer 实跑、故障注入框架、火焰图、nginx 正式对比  
- WebSocket / io_uring / TLS  

以上归入 `closeout-B/C` 或明确不做。

---

## 九、一句话总结

**`closeout-A` = 延迟地板修复（NODELAY）+ 可信号停机（含早释端口与 loop 可 join）+ 连接上限 + 启动失败友好退出 + 文档停扩；后续补丁专门解决 Ctrl+Z 误用导致的端口占用与核心转储体验问题。**
