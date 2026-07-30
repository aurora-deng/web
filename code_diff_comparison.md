# 原型 vs 最新版本：代码差异对比文档

> 原型目录: `e:\虚拟机高级web开发\orign`（仅含 `.gitignore` 和 `LICENSE`，源代码已不在）  
> 最新版本: `e:\虚拟机高级web开发\web`  
> 本文档基于会话历史中记录的原型架构与当前代码的实际差异编写。

---

## 一、架构总览对比

### 1.1 原型架构（升级前）

```
main.cpp (~247行)
  ├── 手动 socket/bind/listen/epoll
  ├── 手动创建 SubReactor[]
  ├── 手动 acceptor loop
  └── 手动 round-robin 分发

SubReactor (上帝类)
  ├── socket (fd)
  ├── parser (HttpParser，含 static 函数)
  ├── router (直接持有 Router 引用)
  ├── executor (同步调用 router.handle)
  └── sender (依赖全局 wheel/segPool)
```

**问题**：
- `router.handle(ctx)` 在 Reactor 线程同步执行，handler 里的 `sleep()` 卡死整个 Reactor
- SubReactor 承担了传输、协议、路由、发送全部职责
- main.cpp 包含大量基础设施代码
- 无异常处理、无背压机制

### 1.2 最新架构（升级后）

```
main.cpp (~110行，仅路由注册)
  └── ServerRuntime                          server/Runtime/ServerRuntime.h
        ├── Router (owned)
        ├── HttpCodec (owned，共享给所有 SubReactor)
        ├── Executor (owned，共享给所有 SubReactor)
        └── ReactorGroup                    server/Reactor/ReactorGroup.h
              └── SubReactor[]              server/SubReactor/SubReactor.h
                    ├── Connection (分层)
                    │     ├── ConnTransport   传输层: fd, readBuffer, state
                    │     └── ConnTimer       定时器层: expireSlot
                    ├── HttpSession           server/http/HttpSession/HttpSession.h
                    │     ├── HttpCodec&      (借用)
                    │     ├── ResponseSender  (依赖注入)
                    │     └── CoroutineScheduler
                    │           ├── ReadAwaiter
                    │           ├── WriteAwaiter
                    │           └── ExecuteAwaiter  ← 新增
                    ├── Executor&             (借用)
                    ├── TimerWheel
                    ├── SegmentPool
                    └── Buffer
```

---

## 二、函数级差异清单

### 2.1 main.cpp

**文件**: [main.cpp](file:///e:/虚拟机高级web开发/web/main.cpp)

| 原型 | 最新版本 | 差异说明 |
|------|----------|----------|
| ~247 行手动创建 socket/epoll/SubReactor | ~110 行，仅路由注册 + `server.start()` | 基础设施全部下沉到 ServerRuntime |
| 手动 `socket()` + `fcntl(O_NONBLOCK)` | ServerRuntime 内部用 `SOCK_NONBLOCK \| SOCK_CLOEXEC` 一次到位 | 减少一次 fcntl 系统调用 |
| 手动 `accept()` + `fcntl` | `accept4(SOCK_NONBLOCK \| SOCK_CLOEXEC)` | 原子设置非阻塞，无竞态窗口 |
| 手动 round-robin `subs_[idx]->addFd(fd)` | `reactorGroup_->dispatch(fd)` | 分发逻辑封装到 ReactorGroup |
| 无 `/slow` `/fast` `/large` 路由 | 新增验收路由 | 验证 Executor 异步、大文件 sendfile |

**为什么这么做**：main.cpp 只应关心业务路由，不应了解 socket/epoll/线程模型。基础设施封装到 ServerRuntime 后，main.cpp 从 247 行降到 110 行，且更换部署方式（如 systemd socket activation）时只需改 ServerRuntime。

---

### 2.2 SubReactor

**文件**: [server/SubReactor/SubReactor.h](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.h)  
**实现**: [server/SubReactor/SubReactor.cpp](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp)

#### 2.2.1 构造函数

| 原型 | 最新版本 | 差异 |
|------|----------|------|
| `SubReactor(Router &router)` | `SubReactor(HttpCodec &codec, Executor &executor)` | 依赖注入 Codec 和 Executor |

**为什么**：原型中每个 SubReactor 各自持有 Router，导致路由表 N 份拷贝。最新版本中 HttpCodec 和 Executor 由 ServerRuntime 统一管理，所有 SubReactor 共享同一份路由表和 Worker 线程池，避免线程数平方级膨胀。

#### 2.2.2 新增成员

| 成员 | 位置 | 作用 |
|------|------|------|
| `Executor &executor` | [SubReactor.h 行153](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.h#L153) | 借用共享的 Worker 线程池 |
| `HttpCodec &codec` | [SubReactor.h 行152](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.h#L152) | 借用共享的编解码器 |
| `completeMtx` / `completeQueue` | [SubReactor.h 行156-157](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.h#L156-L157) | Worker 完成通知队列 |
| `zombieWakes` | [SubReactor.h 行160](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.h#L160) | 僵尸协程唤醒表 |

#### 2.2.3 新增方法

| 方法 | 位置 | 作用 |
|------|------|------|
| `wakeExecuteCoroutine(fd)` | [SubReactor.cpp 行100](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp#L100) | 唤醒执行完成的协程 |
| `notifyExecuteComplete(fd, connId)` | [SubReactor.cpp 行116](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp#L116) | Worker 线程投递完成通知（线程安全） |
| `processComplete()` | [SubReactor.cpp 行131](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp#L131) | Reactor 线程批量消费完成队列 |
| `stop()` / `join()` | [SubReactor.h 行215-216](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.h#L215-L216) | 优雅停止 |

#### 2.2.4 fd_close 修改

**文件**: [SubReactor.cpp fd_close](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp#L147)

| 原型 | 最新版本 | 差异 |
|------|----------|------|
| `void fd_close(int fd, std::string reason, bool fromCoroutine=false)` | `void fd_close(int fd, std::string_view reason, bool fromCoroutine=false)` | `string` → `string_view`，消除拷贝 |
| 无 `wasExecuting` 判断 | 新增 `wasExecuting` 分支 | 协程在 Executor 中执行时，handle 存入 `zombieWakes` |
| 无 `zombieWakes` | 新增僵尸唤醒表 | 防止协程泄漏 |
| `cout << reason << endl` | `LOG_DEBUG(...)` | 异步日志，不阻塞 Reactor |

**为什么**：原型中 `fd_close` 以 `std::string` 按值接收 reason，每次关闭连接都产生一次堆分配。改成 `string_view` 后零拷贝。更重要的是，原型在协程执行 handler 期间关闭连接会导致协程永远挂起（僵尸泄漏），新增的 `zombieWakes` 机制确保 Worker 完成后协程能被唤醒并安全退出。

#### 2.2.5 processPendingFds 修改

**文件**: [SubReactor.cpp processPendingFds](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp#L404)

| 原型 | 最新版本 | 差异 |
|------|----------|------|
| 逐条加锁解锁 `pending_mtx` | 一次 `swap` 整个队列 | N 个 fd 从 N 次锁降为 1 次 |

**为什么**：高并发接入时（如 wrk 1000 连接同时建立），原型逐条加锁导致 `pending_mtx` 成为瓶颈。批量交换后，Worker 线程投递 fd 和 Reactor 消费 fd 之间只需一次锁竞争。

#### 2.2.6 loop 修改

**文件**: [SubReactor.cpp loop](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.cpp#L313)

| 原型 | 最新版本 | 差异 |
|------|----------|------|
| eventfd 分支后仅 `processPendingFds()` | 增加 `processComplete()` | 消费 Worker 完成通知 |

#### 2.2.7 移除死代码

| 原型 | 最新版本 |
|------|----------|
| 文件级 `static toLower()` 函数 | 已移除（未使用） |

---

### 2.3 HttpSession

**文件**: [server/http/HttpSession/HttpSession.h](file:///e:/虚拟机高级web开发/web/server/http/HttpSession/HttpSession.h)  
**实现**: [server/http/HttpSession/HttpSession.cpp](file:///e:/虚拟机高级web开发/web/server/http/HttpSession/HttpSession.cpp)

#### 2.3.1 状态机

| 原型 | 最新版本 | 差异 |
|------|----------|------|
| 无显式状态机 | `enum SessionState { READING, EXECUTING, WRITING, CLOSED }` | 显式状态转换 |

**为什么**：原型中协程的执行流程隐含在 `while(true)` 循环中，难以追踪当前处于哪个阶段。显式状态机让调试和日志能清晰反映连接生命周期。

#### 2.3.2 dispatch 异步化

**文件**: [HttpSession.cpp 行154-203](file:///e:/虚拟机高级web开发/web/server/http/HttpSession/HttpSession.cpp#L154-L203)

| 原型 | 最新版本 | 差异 |
|------|----------|------|
| `reactor->codec.dispatch(ctx)` 同步调用 | `reactor->executor.submit(lambda)` + `co_await ExecuteAwaiter` | handler 在 Worker 线程执行 |
| 无异常处理 | `try/catch` 将异常转为 500 | 防止 `std::terminate` |
| 无背压 | `submit` 返回 false 时返回 503 | 有界队列满时明确拒绝 |
| `conn->id` 可能悬空 | 使用前重新 `getConn()` 检查 | 修复 co_await 后指针失效 |
| Worker 连接关闭时直接 return | 始终调用 `notifyExecuteComplete` | 修复协程泄漏 |

**为什么**：原型中 `router.handle(ctx)` 在 Reactor 线程同步执行，handler 里的 `sleep(10)` 会卡住整个 Reactor，导致该 Reactor 上所有其他连接都无法响应。异步化后，handler 在 Worker 线程执行，Reactor 线程可以继续处理其他连接的 I/O。

#### 2.3.3 RequestContext 所有权

| 原型 | 最新版本 | 差异 |
|------|----------|------|
| `RequestContext ctx` 局部变量 | `context_` 成员变量 | 生命周期随 HttpSession |

**为什么**：原型中 `ctx` 是协程帧上的局部变量，Worker lambda 通过 `[&ctx]` 捕获引用。如果协程帧被销毁（如连接关闭后强制唤醒），`ctx` 引用悬空。改为成员变量后，`ctx` 的生命周期随 HttpSession（由 shared_ptr 管理），即使协程帧销毁，HttpSession 仍然存活，Worker 可以安全访问 `context_`。

---

### 2.4 Executor（新增）

**文件**: [server/Executor/Executor.h](file:///e:/虚拟机高级web开发/web/server/Executor/Executor.h)

| 原型 | 最新版本 |
|------|----------|
| 不存在 | 封装 ThreadPool，提供 `submit(Task)` |

**为什么**：需要一个独立的业务执行器，将 handler 执行从 Reactor 线程移到 Worker 线程。Executor 由 ServerRuntime 统一管理，所有 SubReactor 共享，避免每个 Reactor 各自创建线程池导致线程数平方级膨胀。

---

### 2.5 ExecuteAwaiter（新增）

**文件**: [server/CoroutineScheduler/AWaiter.h](file:///e:/虚拟机高级web开发/web/server/CoroutineScheduler/AWaiter.h#L45-L56)  
**实现**: [server/CoroutineScheduler/AWaiter.cpp](file:///e:/虚拟机高级web开发/web/server/CoroutineScheduler/AWaiter.cpp#L73-L101)

| 原型 | 最新版本 |
|------|----------|
| 不存在 | 协程挂起等待 Executor 完成 |

| 方法 | 作用 |
|------|------|
| `await_ready()` | 检查连接是否已关闭 |
| `await_suspend(h)` | 设置 `state=EXECUTE`，不注册 epoll 事件 |
| `await_resume()` | 清理 `waiting` 状态 |

**为什么**：与 `ReadAwaiter`（等待 EPOLLIN）和 `WriteAwaiter`（等待 EPOLLOUT）不同，`ExecuteAwaiter` 不等待 I/O 事件，而是等待 Worker 线程完成 handler 执行。唤醒由 `processComplete → wakeExecuteCoroutine` 完成。

---

### 2.6 AwaitType 枚举

**文件**: [server/CoroutineScheduler/CoroutineScheduler.h](file:///e:/虚拟机高级web开发/web/server/CoroutineScheduler/CoroutineScheduler.h#L12-L19)

| 原型 | 最新版本 |
|------|----------|
| `enum AwaitType { NONE, READ, WRITE, TIMER }` | `enum AwaitType { NONE, READ, WRITE, EXECUTE, TIMER }` |

**为什么**：新增 `EXECUTE` 状态让 `fd_close` 能区分协程是在等待 I/O 还是在等待 Worker 完成，从而采用不同的唤醒策略（I/O 等待直接 schedule，EXECUTE 等待走 zombieWakes）。

---

### 2.7 HttpParser 子对象化

**文件**: [server/http/HttpParser/HttpParser.h](file:///e:/虚拟机高级web开发/web/server/http/HttpParser/HttpParser.h)

| 原型 | 最新版本 |
|------|----------|
| `static parseQuery()` 文件级函数 | `RequestLineParser::parseQuery()` 私有成员函数 |
| `static parseRange()` 文件级函数 | `HeaderParser::parseRange()` 私有成员函数 |
| 单体解析器 | 持有 `RequestLineParser` + `HeaderParser` + `BodyParser` 三个子对象 |

**新增文件**:
- [ParserUtils.h](file:///e:/虚拟机高级web开发/web/server/http/HttpParser/ParserUtils.h) — 工具函数和常量
- [RequestLineParser.h](file:///e:/虚拟机高级web开发/web/server/http/HttpParser/RequestLineParser.h) — 请求行解析
- [HeaderParser.h](file:///e:/虚拟机高级web开发/web/server/http/HttpParser/HeaderParser.h) — 首部解析
- [BodyParser.h](file:///e:/虚拟机高级web开发/web/server/http/HttpParser/BodyParser.h) — 正文解析

**为什么**：原型中 `parseQuery` 和 `parseRange` 是文件级 static 函数，不符合"完全对象化"目标。拆分为子对象后，每个解析器管理自己的状态（contentLength、chunked、range 等），HttpParser 只做状态机协调。对外接口 `parse()`/`reset()` 不变，不影响调用方。

---

### 2.8 ServerRuntime（新增）

**文件**: [server/Runtime/ServerRuntime.h](file:///e:/虚拟机高级web开发/web/server/Runtime/ServerRuntime.h)  
**实现**: [server/Runtime/ServerRuntime.cpp](file:///e:/虚拟机高级web开发/web/server/Runtime/ServerRuntime.cpp)

| 原型 | 最新版本 |
|------|----------|
| 不存在，main.cpp 手动管理 | 封装所有基础设施生命周期 |

| 方法 | 作用 |
|------|------|
| `router()` | 返回路由器引用 |
| `setPort(int)` | 设置监听端口 |
| `setReactorCount(size_t)` | 设置 SubReactor 数量 |
| `start()` | 创建监听套接字、SubReactor，进入 acceptor loop |
| `setupListener()` | `SOCK_NONBLOCK \| SOCK_CLOEXEC` 一次到位 |
| `createReactors()` | 通过 ReactorGroup 创建 SubReactor |
| `acceptLoop()` | `accept4(SOCK_NONBLOCK \| SOCK_CLOEXEC)` |

**为什么**：原型中 main.cpp 包含大量基础设施代码（socket 创建、epoll 配置、线程启动），职责过重。ServerRuntime 封装后，main.cpp 只需注册路由和调用 `start()`。

---

### 2.9 ReactorGroup（新增）

**文件**: [server/Reactor/ReactorGroup.h](file:///e:/虚拟机高级web开发/web/server/Reactor/ReactorGroup.h)  
**实现**: [server/Reactor/ReactorGroup.cpp](file:///e:/虚拟机高级web开发/web/server/Reactor/ReactorGroup.cpp)

| 原型 | 最新版本 |
|------|----------|
| SubReactor 数组直接在 main.cpp 中管理 | ReactorGroup 封装 SubReactor 生命周期和分发 |

| 方法 | 作用 |
|------|------|
| `start(count)` | 创建指定数量的 SubReactor 并启动 |
| `dispatch(fd)` | Round-robin 分发新连接 |
| `stop()` / `join()` | 优雅停止 |

**为什么**：ServerRuntime 不应了解单个 Reactor 的队列、epoll 或线程细节。ReactorGroup 作为中间层，只负责 SubReactor 的生命周期和连接分发。

---

### 2.10 HttpCodec 依赖注入

**文件**: [server/http/HttpCodec/HttpCodec.h](file:///e:/虚拟机高级web开发/web/server/http/HttpCodec/HttpCodec.h)

| 原型 | 最新版本 |
|------|----------|
| SubReactor 直接持有 Router | HttpCodec 持有 Router 引用，SubReactor 借用 HttpCodec |

**为什么**：原型中 SubReactor 直接接触 Router，违反单一职责。HttpCodec 作为传输层与 HTTP 语义之间的适配层，集中管理解析和分发。后续增加协议版本或替换分发策略时有明确扩展点。

---

### 2.11 ResponseSender 依赖注入

**文件**: [server/http/ResponseSender/ResponseSender.h](file:///e:/虚拟机高级web开发/web/server/http/ResponseSender/ResponseSender.h)

| 原型 | 最新版本 |
|------|----------|
| 依赖全局 `wheel` / `segPool` | 构造函数注入 `SegmentPool&` 和 `TimerWheel&` |

**为什么**：原型中 ResponseSender 依赖全局变量，无法独立测试。依赖注入后，`ResponseSender sender(segPool, wheel); sender.send(fd, response);` 不依赖 SubReactor，可独立单元测试。

---

### 2.12 Connection 分层

**文件**: [server/SubReactor/SubReactor.h](file:///e:/虚拟机高级web开发/web/server/SubReactor/SubReactor.h#L78-L118)

| 原型 | 最新版本 |
|------|----------|
| Connection 包含 fd + parser + request + response 全部 | Connection 分为 ConnTransport + ConnTimer，HTTP 生命周期由 HttpSession 持有 |

**为什么**：原型中 Connection 承担了传输、协议、请求、响应全部状态，职责过重。分层后：
- **ConnTransport**: 只管 fd、readBuffer、state（传输层）
- **ConnTimer**: 只管超时（定时器层）
- **HttpSession**: 管 parser、request、response、状态机（协议生命周期）

---

### 2.13 CoroutineScheduler

**文件**: [server/CoroutineScheduler/CoroutineScheduler.h](file:///e:/虚拟机高级web开发/web/server/CoroutineScheduler/CoroutineScheduler.h)

| 原型 | 最新版本 |
|------|----------|
| 无 CompletionCallback | 新增 `setCompletionCallback` | 协程完成后清理 coroutine_context |
| `inQueueSet` 去重 | `owned` + `scheduled` 去重 | 更完善的所有权管理 |
| 无 `reap()` | 新增 `reap()` | 回收已完成协程帧 |

**为什么**：协程完成后需要安全清理 `coroutine_context`（handle、state、waiting），但不能误清后续协程的句柄。CompletionCallback 通过地址比对确保只清理匹配的句柄。

---

## 三、关键修复总结

### 3.1 僵尸协程泄漏（严重）

| 问题 | 修复 |
|------|------|
| 协程在 Executor 中执行时连接被关闭，Worker 完成后不通知，协程永远挂起 | `zombieWakes` 表 + Worker 始终通知 + `processComplete` 检查僵尸唤醒 |

### 3.2 conn 指针悬空

| 问题 | 修复 |
|------|------|
| `co_await` 后使用旧 conn 指针访问 `conn->id` | 使用前重新 `getConn()` 检查 |

### 3.3 Worker 异常导致 std::terminate

| 问题 | 修复 |
|------|------|
| handler 抛异常逃出线程入口 | `try/catch` 转为 500 响应 |

### 3.4 Executor 队列满导致协程泄漏

| 问题 | 修复 |
|------|------|
| `submit` 返回 false 时协程仍进入 `co_await ExecuteAwaiter`，无 Worker 会通知 | 返回 503 响应，不进入等待 |

### 3.5 processPendingFds/processComplete 锁竞争

| 问题 | 修复 |
|------|------|
| 逐条加锁解锁 | 批量 `swap` 队列，N 次锁降为 1 次 |

---

## 四、性能优化总结

| 优化项 | 原型 | 最新版本 | 效果 |
|--------|------|----------|------|
| fd_close reason | `std::string` 按值 | `std::string_view` 零拷贝 | 消除每次关闭的堆分配 |
| processPendingFds | 逐条锁 | 批量 swap | N 次锁 → 1 次锁 |
| processComplete | 逐条锁 | 批量 swap | N 次锁 → 1 次锁 |
| accept | `accept()` + `fcntl` | `accept4(SOCK_NONBLOCK\|SOCK_CLOEXEC)` | 减少 1 次系统调用 |
| listen socket | `socket()` + `fcntl` | `SOCK_NONBLOCK\|SOCK_CLOEXEC` | 减少 1 次系统调用 |
| 日志 | `cout << endl` | `LOG_DEBUG` 异步 | 不阻塞 Reactor 线程 |
| 死代码 | 未使用的 `toLower` | 已移除 | 减少编译产物 |
| Handler 执行 | Reactor 线程同步 | Worker 线程异步 | 不阻塞其他连接 |
| 线程池 | 每 Reactor 独立 | 全局共享 | 避免线程数平方膨胀 |

---

## 五、验收标准对照

| 标准 | 原型 | 最新版本 | 验证方式 |
|------|------|----------|----------|
| HTTP 解析独立 | ❌ 依赖 SubReactor | ✅ `HttpParser parser; parser.parse();` | 单元测试 |
| Response 发送独立 | ❌ 依赖全局变量 | ✅ `ResponseSender sender(segPool, wheel);` | 单元测试 |
| Handler 不在 Reactor 线程 | ❌ 同步调用 | ✅ Executor + ExecuteAwaiter | `/slow` + `/fast` 并发 |
| 一个 TCP 多个请求 | ⚠️ 未验证 | ✅ keep-alive 黑盒测试 | 3 请求一次 TCP |
| 文件发送 | ⚠️ 基本支持 | ✅ sendfile + Range 206/304 | `/logo` + `/large` |
| 异常安全 | ❌ std::terminate | ✅ try/catch 转 500 | 模拟异常 handler |
| 背压 | ❌ 无 | ✅ 503 + 暂停读 | 模拟队列满 |
