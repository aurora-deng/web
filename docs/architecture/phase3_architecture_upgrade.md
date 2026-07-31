# 第三阶段：Server Framework Architecture Upgrade

> **配套重要知识点笔记（思想补齐）**：[phase3/README.md](phase3/README.md)  
> 含 Reactor / Runtime / Async Runtime / Pipeline / 背压 / 所有权 / Completion / 验收 / 第四阶段桥。

## 一、升级概述

### 1.1 升级目标

将"事件驱动 + 线程池 HTTP 服务器"升级为"C++20 Coroutine Reactor 服务器"，实现：

1. **HTTP 解析独立** — `HttpParser` 不依赖 `SubReactor`
2. **Response 发送独立** — `ResponseSender` 通过依赖注入，不依赖 `SubReactor`
3. **Handler 不在 Reactor 线程** — `Executor` 将业务逻辑移到 Worker 线程
4. **一个 TCP 多个请求** — keep-alive 连接复用，Session 状态机正确切换
5. **文件发送** — sendfile 零拷贝 + Range 206/304

### 1.2 架构对比

```
升级前:                              升级后:
SubReactor                           ServerRuntime
{                                    ├── Router
  socket                             ├── ReactorGroup
  parser                             │   └── SubReactor[]
  router        ← 阻塞Reactor线程        ├── Connection (传输层)
  executor      ← 同步调用               ├── HttpSession (协议生命周期)
  sender                             │   ├── HttpCodec
}                                    │   │   ├── HttpParser
                                     │   │   │   ├── RequestLineParser
                                     │   │   │   ├── HeaderParser
                                     │   │   │   └── BodyParser
                                     │   │   └── dispatch → Executor
                                     │   ├── ResponseSender (依赖注入)
                                     │   └── CoroutineScheduler
                                     └── Executor (Worker线程池)
```

### 1.3 数据流对比

```
升级前 (阻塞):
epoll → wakeReadCoroutine → HttpSession::run()
  → codec.dispatch(ctx)     ← Reactor线程同步执行
  → router.handle(ctx)      ← handler里的sleep()卡死整个Reactor
  → sender.send()

升级后 (异步):
epoll → wakeReadCoroutine → HttpSession::run()
  → executor.submit(task)     ← 投递到Worker线程
  → co_await ExecuteAwaiter   ← 协程挂起，Reactor继续处理其他连接
  ...Worker线程执行handler...
  → notifyExecuteComplete     ← 通过eventfd通知Reactor
  → processComplete           ← Reactor线程消费完成队列
  → wakeExecuteCoroutine      ← 唤醒协程
  → sender.send()             ← Reactor线程发送响应
```

---

## 二、新增文件清单

### 2.1 Executor — 业务执行器

**文件**: [server/Executor/Executor.h](file:///e:/虚拟机高级web开发/web/server/Executor/Executor.h)

封装 `ThreadPool`，将 handler 执行从 Reactor 线程移到 Worker 线程。

| 方法 | 作用 |
|------|------|
| `Executor(size_t workerCount=4)` | 构造函数，默认4个Worker线程 |
| `bool submit(Task task)` | 提交任务到Worker线程，返回false表示队列满(背压) |

### 2.2 ExecuteAwaiter — 执行等待器

**文件**: [server/CoroutineScheduler/AWaiter.h](file:///e:/虚拟机高级web开发/web/server/CoroutineScheduler/AWaiter.h#L45-L56)  
**实现**: [server/CoroutineScheduler/AWaiter.cpp](file:///e:/虚拟机高级web开发/web/server/CoroutineScheduler/AWaiter.cpp#L73-L101)

协程挂起等待 Executor 完成 handler 执行。与 `ReadAwaiter`/`WriteAwaiter` 不同，不注册 epoll 事件。

| 方法 | 作用 |
|------|------|
| `await_ready()` | 检查连接是否已关闭，已关闭时不挂起 |
| `await_suspend(h)` | 设置 `state=EXECUTE`，不注册epoll事件 |
| `await_resume()` | 清理 `waiting` 状态 |

### 2.3 ServerRuntime — 服务器运行时

**文件**: [server/Runtime/ServerRuntime.h](file:///e:/虚拟机高级web开发/web/server/Runtime/ServerRuntime.h)  
**实现**: [server/Runtime/ServerRuntime.cpp](file:///e:/虚拟机高级web开发/web/server/Runtime/ServerRuntime.cpp)

封装所有基础设施生命周期，简化 `main.cpp`。

| 方法 | 作用 |
|------|------|
| `router()` | 返回路由器引用，用于注册路由 |
| `setPort(int)` | 设置监听端口 |
| `setReactorCount(size_t)` | 设置SubReactor数量，0=自动 |
| `start()` | 创建监听套接字、SubReactor，进入acceptor loop |
| `setupListener()` | socket/bind/listen/epoll创建 |
| `createReactors()` | 创建SubReactor数组并启动 |
| `acceptLoop()` | 主Reactor accept循环，round-robin分发fd |

### 2.4 Parser 子对象化

**ParserUtils**: [server/http/HttpParser/ParserUtils.h](file:///e:/虚拟机高级web开发/web/server/http/HttpParser/ParserUtils.h)  
工具函数 `toLower`/`trim` 和大小上限常量 `kMaxRequestLineBytes`/`kMaxHeaderBytes`/`kMaxBodyBytes`。

**RequestLineParser**: [server/http/HttpParser/RequestLineParser.h](file:///e:/虚拟机高级web开发/web/server/http/HttpParser/RequestLineParser.h)  
解析请求行和 query 参数。`parseQuery` 从 static 函数变为私有成员函数。

| 方法 | 作用 |
|------|------|
| `parse(Buffer&, HttpRequest&)` | 解析请求行 (method/path/version) |
| `reset()` | 重置状态 |
| `keepAlive()` | 返回HTTP版本决定的keep-alive默认值 |

**HeaderParser**: [server/http/HttpParser/HeaderParser.h](file:///e:/虚拟机高级web开发/web/server/http/HttpParser/HeaderParser.h)  
解析首部和 Range。`parseRange` 从 static 函数变为私有成员函数。

| 方法 | 作用 |
|------|------|
| `parse(Buffer&, HttpRequest&)` | 解析首部行 |
| `setKeepAlive(bool)` | 由HttpParser传入版本默认值 |
| `contentLength()` / `chunked()` / `range()` | 返回framing状态 |

**BodyParser**: [server/http/HttpParser/BodyParser.h](file:///e:/虚拟机高级web开发/web/server/http/HttpParser/BodyParser.h)  
解析固定长度正文和 chunked 分块正文。

| 方法 | 作用 |
|------|------|
| `parse(Buffer&, HttpRequest&)` | 解析固定长度正文 |
| `parseChunkSize(Buffer&)` | 读取chunk长度行 |
| `parseChunkData(Buffer&, HttpRequest&)` | 读取chunk数据 |
| `parseChunkTrailers(Buffer&)` | 读取trailer字段 |
| `setContentLength/setChunked/setHeaderBytes` | 由HttpParser传入HeaderParser结果 |

---

## 三、修改文件清单

### 3.1 CoroutineScheduler

**文件**: [server/CoroutineScheduler/CoroutineScheduler.h](file:///e:/虚拟机高级web开发/web/server/CoroutineScheduler/CoroutineScheduler.h#L12-L19)

`AwaitType` 枚举新增 `EXECUTE` 状态：

```cpp
enum class AwaitType { NONE, READ, WRITE, EXECUTE, TIMER };
```

**文件**: [server/CoroutineScheduler/CoroutineScheduler.cpp](file:///e:/虚拟机高级web开发/web/server/CoroutineScheduler/CoroutineScheduler.cpp)

`runReady()` 在 resume 后检查 `h.done()` 并调用 `reap()` 回收协程帧。

### 3.2 SubReactor

**文件**: [server/SubReactor/SubReactor.h](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.h)

新增成员：

| 成员 | 作用 |
|------|------|
| `Executor executor` | 业务执行器 |
| `completeMtx` / `completeQueue` | Worker完成通知队列 |
| `zombieWakes` | 僵尸协程唤醒表（防止协程泄漏） |

新增方法：

| 方法 | 作用 |
|------|------|
| `wakeExecuteCoroutine(fd)` | 唤醒执行完成的协程 |
| `notifyExecuteComplete(fd, connId)` | Worker线程投递完成通知（线程安全） |
| `processComplete()` | Reactor线程批量消费完成队列 |

**文件**: [server/SubReactor/SubReactor.cpp](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp)

关键改动：

1. **`fd_close`** — [行147](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp#L147)
   - 参数 `reason` 改为 `std::string_view` 避免拷贝
   - 新增 `wasExecuting` 分支：协程在Executor中执行时，将handle存入`zombieWakes`
   - 修复僵尸协程泄漏

2. **`processComplete`** — [行116](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp#L116)
   - 批量交换队列减少锁竞争
   - 处理僵尸唤醒：连接已删除时通过`zombieWakes`查找handle并调度

3. **`processPendingFds`** — [行404](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp#L404)
   - 批量交换队列减少锁竞争

4. **`loop`** — [行313](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp#L313)
   - eventfd分支后增加 `processComplete()` 调用

5. 移除未使用的 `toLower` 函数

### 3.3 HttpSession

**文件**: [server/http/HttpSession/HttpSession.cpp](file:///e:/虚拟机高级web开发/web/server/http/HttpSession/HttpSession.cpp)

关键改动 — [行119-143](file:///e:/虚拟机高级web开发/web/server/http/HttpSession/HttpSession.cpp#L119-L143):

1. **dispatch 改为异步**:
   - 提交到 `reactor->executor.submit(lambda)`
   - `co_await ExecuteAwaiter(reactor, fd)` 挂起协程

2. **修复 conn 指针悬空**:
   - 在使用 `conn->id` 前重新调用 `getConn()` 检查

3. **修复 Worker 不通知导致协程泄漏**:
   - Worker lambda 中**始终**调用 `notifyExecuteComplete`
   - 无论连接是否已关闭，确保协程能被唤醒

### 3.4 HttpParser

**文件**: [server/http/HttpParser/HttpParser.h](file:///e:/虚拟机高级web开发/web/server/http/HttpParser/HttpParser.h)  
**文件**: [server/http/HttpParser/HttpParser.cpp](file:///e:/虚拟机高级web开发/web/server/http/HttpParser/HttpParser.cpp)

- 持有三个栈上子解析器: `requestLineParser_`, `headerParser_`, `bodyParser_`
- `parse()` 只做状态机协调和子解析器间状态传递
- 对外接口 (`parse`/`reset`/`keepAlive`/`getContentLength`) 保持不变

### 3.5 main.cpp

**文件**: [main.cpp](file:///e:/虚拟机高级web开发/web/main.cpp)

从 247 行简化为 97 行，只含路由注册和 `server.start()`。

### 3.6 CMakeLists.txt

**文件**: [CMakeLists.txt](file:///e:/虚拟机高级web开发/web/CMakeLists.txt)

添加 `server/Runtime/ServerRuntime.cpp` 到 `webserver_core` 库。

### 3.7 验收测试

**文件**: [tests/integration/http_blackbox.py](file:///e:/虚拟机高级web开发/web/tests/integration/http_blackbox.py)

新增测试用例：
- keep-alive 多请求（一次TCP连接发3个请求）
- Executor 验收（/slow 不阻塞 /fast）

---

## 四、逻辑自洽性修复

### 4.1 僵尸协程泄漏（严重）

**问题**: `fd_close` 中 `wasExecuting=true` 时不唤醒协程。Worker 完成后发现连接已消失直接 return，不调用 `notifyExecuteComplete`，导致协程永远挂起，帧和 HttpSession 泄漏。

**修复**:

1. `fd_close` 中 `wasExecuting=true` 时，将 handle 存入 `zombieWakes[connId]`
2. Worker lambda 中**始终**调用 `notifyExecuteComplete`
3. `processComplete` 中，连接已删除时检查 `zombieWakes`，找到则直接调度 handle

**涉及文件**:
- [SubReactor.h](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.h#L174-L176) — 新增 `zombieWakes` 成员
- [SubReactor.cpp fd_close](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp#L170-L176) — 存入 zombieWakes
- [SubReactor.cpp processComplete](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp#L134-L142) — 检查 zombieWakes
- [HttpSession.cpp](file:///e:/虚拟机高级web开发/web/server/http/HttpSession/HttpSession.cpp#L139-L142) — 始终通知

### 4.2 conn 指针悬空

**问题**: `HttpSession::run()` 中 `conn->id` 使用的 `conn` 是循环开始前获取的，中间经历了 `co_await ReadAwaiter`，连接可能已关闭。

**修复**: 在使用 `conn->id` 前重新调用 `getConn()` 检查。

**涉及文件**: [HttpSession.cpp 行122-127](file:///e:/虚拟机高级web开发/web/server/http/HttpSession/HttpSession.cpp#L122-L127)

### 4.3 Worker 不通知导致协程泄漏

**问题**: Worker lambda 中连接已关闭时直接 return，不调用 `notifyExecuteComplete`，协程永远挂起。

**修复**: Worker lambda 中**始终**调用 `notifyExecuteComplete`，让 `processComplete` 通过 `zombieWakes` 处理。

**涉及文件**: [HttpSession.cpp 行139-142](file:///e:/虚拟机高级web开发/web/server/http/HttpSession/HttpSession.cpp#L139-L142)

---

## 五、性能优化

### 5.1 fd_close reason 参数优化

**优化**: `std::string` → `std::string_view`，避免每次关闭连接时的字符串拷贝。

**涉及文件**: [SubReactor.h 行219](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.h#L219), [SubReactor.cpp 行147](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp#L147)

### 5.2 processComplete 批量交换

**优化**: 从逐条加锁解锁改为一次 `swap` 整个队列，N个完成通知只需1次锁。

**涉及文件**: [SubReactor.cpp 行116-122](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp#L116-L122)

### 5.3 processPendingFds 批量交换

**优化**: 同上，从逐条加锁改为一次 `swap`。

**涉及文件**: [SubReactor.cpp 行404-410](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp#L404-L410)

### 5.4 移除死代码

**优化**: 移除 SubReactor.cpp 中未使用的 `toLower` 函数。

---

## 六、核心数据流

### 6.1 请求处理完整流程

```
1. ServerRuntime::acceptLoop() accept新连接
   → round-robin分发到SubReactor
   → SubReactor::addFd(fd) 入队pendingFds + eventfd唤醒

2. SubReactor::loop() eventfd可读
   → processPendingFds(): 创建Connection、HttpSession、启动协程
   → processComplete(): 消费Worker完成通知

3. HttpSession::run() 协程主循环
   → state=READING/PARSING
   → readRequest(): HttpCodec::decode + recvSocket
   → 如果NEED_MORE: co_await ReadAwaiter → 挂起等待EPOLLIN
   
4. 解析完成
   → state=EXECUTE
   → executor.submit(lambda): Worker线程执行codec.dispatch → router.handle
   → co_await ExecuteAwaiter → 挂起等待Worker完成
   
5. Worker线程完成
   → notifyExecuteComplete(fd, connId): 入队completeQueue + eventfd唤醒
   
6. SubReactor::loop() eventfd可读
   → processComplete(): wakeExecuteCoroutine → scheduler.schedule(handle)
   
7. HttpSession::run() 协程恢复
   → state=WRITING
   → sender.send(fd, response)
   → 如果SEND_AGAIN: co_await WriteAwaiter → 挂起等待EPOLLOUT
   → SEND_OK: afterSend() → 恢复EPOLLIN或fd_close
   
8. 循环回到步骤3 (keep-alive) 或 co_return (close)
```

### 6.2 连接关闭安全流程

```
fd_close(fd, reason, fromCoroutine):
  ├── fromCoroutine=true: 协程即将co_return，帧由scheduler.reap()回收
  ├── wasExecuting=true: 存入zombieWakes，Worker完成后通过processComplete唤醒
  │   → 协程恢复 → getConn()==nullptr → 释放response → co_return
  └── 其他: 存入coroutineToWake，erase后scheduler.schedule(h)
      → 协程恢复 → getConn()==nullptr → co_return
```

### 6.3 背压机制

```
recvSocket:
  readableBytes > MAX_PENDING_BYTES(1MB)
  → state.pauseByMemory = true
  → updateEvent: 撤销EPOLLIN
  → 后续不再读取数据

afterSend (发送完成后):
  readableBytes < MAX_PENDING_BYTES/2 (512KB)
  → state.pauseByMemory = false
  → updateEvent: 恢复EPOLLIN
```

---

## 七、验收标准

| 标准 | 验证方式 | 状态 |
|------|----------|------|
| HTTP解析独立 | `HttpParser parser; parser.parse();` 不依赖SubReactor | ✅ |
| Response发送独立 | `ResponseSender sender(segPool, wheel); sender.send(fd, response);` | ✅ |
| Handler不在Reactor线程 | `/slow` sleep(10) 期间 `/fast` 立即返回 | ✅ |
| 一个TCP多个请求 | keep-alive黑盒测试: GET /a, /b, /c | ✅ |
| 文件发送 | `/logo` sendfile + Range 206/304 | ✅ |

### 编译验证

```bash
cd /path/to/web && rm -rf build && mkdir build && cd build && cmake .. && make -j$(nproc)
```

### 运行测试

```bash
cd build && ctest --verbose
```

### 压测验证

```bash
wrk -t8 -c1000 -d30s http://127.0.0.1:8080/
```

### Executor 验收

```bash
# 终端1: 请求/slow (会sleep 10秒)
curl http://127.0.0.1:8080/slow &

# 终端2: 立即请求/fast，应立即返回"fast"
curl http://127.0.0.1:8080/fast
```

---

## 八、架构层次索引

```
main.cpp
  └── ServerRuntime                          server/Runtime/ServerRuntime.h
        ├── Router                           server/Route/Router.h
        ├── ReactorGroup                     server/Reactor/ReactorGroup.h
        │     └── SubReactor[]               server/SubReactor/SubReactor.h
        │     ├── Connection (分层)
        │     │     ├── ConnTransport        传输层: fd, readBuffer, state
        │     │     └── ConnTimer            定时器层: expireSlot
        │     ├── HttpSession                server/http/HttpSession/HttpSession.h
        │     │     ├── HttpCodec            server/http/HttpCodec/HttpCodec.h
        │     │     │     ├── HttpParser     server/http/HttpParser/HttpParser.h
        │     │     │     │     ├── RequestLineParser
        │     │     │     │     ├── HeaderParser
        │     │     │     │     └── BodyParser
        │     │     │     └── dispatch → Executor
        │     │     ├── ResponseSender       server/http/ResponseSender/ResponseSender.h
        │     │     └── CoroutineScheduler   server/CoroutineScheduler/CoroutineScheduler.h
        │     │           ├── Task           server/CoroutineScheduler/Task.h
        │     │           ├── ReadAwaiter    server/CoroutineScheduler/AWaiter.h
        │     │           ├── WriteAwaiter
        │     │           └── ExecuteAwaiter
        ├── Executor（所有 Reactor 共享）     server/Executor/Executor.h
        │     └── ThreadPool                 server/threadpoll/thread_pool.h
        │     ├── TimerWheel                 server/timer/TimeWheel.h
        │     ├── SegmentPool                server/SegmentPool/SegmentPool.h
        │     ├── ObjectPool                 server/ObjectPool/ObjectPool.h
        │     └── Buffer                     server/Buffer/Buffer.h
        └── acceptLoop (MainReactor)
```
