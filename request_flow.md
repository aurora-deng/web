# HTTP 请求处理完整流程

> 本文档梳理从 Accept 到 Send 的完整 HTTP 请求处理流程，包含所有关键节点、函数调用链和文件索引。

---

## 一、总体流程概览

```
① Accept ──→ ② Parse ──→ ③ Dispatch ──→ ④ Route ──→ ⑤ Send
 接受连接      解析请求      投递到 Executor    handler      发送响应
```

### 架构层次

```
┌─────────────────────────────────────────────────────────────┐
│                        主线程                                │
│  ServerRuntime::acceptLoop() → accept → addFd()             │
└──────────────────────────────┬──────────────────────────────┘
                               │ round-robin 分发 fd
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                      Reactor 线程（SubReactor::loop）        │
│  epoll_wait() → 事件处理 → scheduler.runReady()             │
│                                                              │
│  ┌───────────────────────────────────────────────────────┐  │
│  │ 协程执行：HttpSession::run()                           │  │
│  │                                                       │  │
│  │  读循环 → Executor 等待 → 写循环 → afterSend           │  │
│  └───────────────────────────────────────────────────────┘  │
│                               │                              │
│                               │ submit / notify              │
│                               ▼                              │
│  ┌───────────────────────────────────────────────────────┐  │
│  │               Worker 线程（Executor）                  │  │
│  │  Router::handle() → handler 业务逻辑                   │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

---

## 二、阶段 ①：Accept — 接受新连接

### 流程

```
主线程:
  ServerRuntime::acceptLoop()
    ├─ accept(listenFd) → 获取新 fd
    ├─ round-robin 选择 SubReactor
    └─ reactor->addFd(fd)
         │
         ▼
SubReactor::addFd(fd)
    ├─ fd_unblock(fd)              ← 设置非阻塞
    ├─ pendingFds.push(fd)         ← 入队待处理
    └─ write(event_fd, &one)       ← 唤醒 Reactor 线程
         │
         ▼
SubReactor::loop() → epoll_wait() 返回
    ├─ 检测到 event_fd 可读
    ├─ processPendingFds()         ← 消费新连接
    │   ├─ epoll_ctl(EPOLL_CTL_ADD)  ← 注册 EPOLLIN|ET|ONESHOT
    │   ├─ 创建 Connection
    │   ├─ 创建 HttpSession
    │   ├─ session->run() → 创建协程
    │   ├─ wheel.add(fd)            ← 注册时间轮
    │   └─ scheduler.adopt(fd, h, session)  ← 协程交给调度器
    │
    └─ scheduler.runReady()        ← 执行就绪协程
```

### 关键文件与函数

| 文件 | 函数 | 行号 | 作用 |
|------|------|------|------|
| `server/Runtime/ServerRuntime.cpp` | `acceptLoop()` | — | 主 Reactor，accept 新连接 |
| `server/SubReactor/SubReactor.cpp` | `addFd()` | L407-L435 | 跨线程安全投递 fd |
| `server/SubReactor/SubReactor.cpp` | `processPendingFds()` | L438-L477 | 注册 epoll + 创建 Connection/Session/协程 |
| `server/SubReactor/SubReactor.cpp` | `fd_unblock()` | L214-L219 | 设置 fd 非阻塞 |

### 关键设计

| 设计点 | 说明 |
|--------|------|
| **eventfd 唤醒** | 主线程通过 eventfd 通知 Reactor，避免竞态 |
| **pendingFds 队列** | 跨线程传递 fd 的唯一入口，保证单线程所有权 |
| **adopt 所有权** | 协程句柄交给 scheduler 统一管理，防止重复销毁 |

---

## 三、阶段 ②：Parse — 解析请求

### 流程

```
HttpSession::run() 协程开始执行
    │
    ├─ state = READING
    ├─ 进入读循环
    │   │
    │   ├─ readRequest()
    │   │   │
    │   │   ├─ codec.decode(readBuffer, parser_, request, keepAlive_)
    │   │   │   └─ HttpParser::parse(buffer, req)
    │   │   │       ├─ RequestLineParser::parse()    ← 请求行解析
    │   │   │       ├─ HeaderParser::parse()         ← 首部解析
    │   │   │       ├─ BodyParser::parse()           ← 正文/chunked 解析
    │   │   │       └─ PARSE_OK → 消费已解析字节
    │   │   │
    │   │   ├─ PARSE_OK → 返回 COMPLETE
    │   │   ├─ PARSE_ERROR → 返回 ERROR
    │   │   └─ PARSE_NEED_MORE
    │   │       │
    │   │       ├─ recvSocket(fd)
    │   │       │   ├─ recv(fd, buffer, 65536)
    │   │       │   ├─ append 到 readBuffer
    │   │       │   └─ 返回 READY/PAUSED/CLOSED
    │   │       │
    │   │       └─ 再次 decode → 仍不够？
    │   │           └─ 返回 NEED_MORE
    │   │
    │   └─ NEED_MORE → co_await ReadAwaiter(reactor, fd)
    │       │
    │       │  （协程挂起，Reactor 继续处理其他事件）
    │       │
    │       │  ... epoll_wait 收到 EPOLLIN ...
    │       │
    │       ├─ wakeReadCoroutine(fd)
    │       │   └─ scheduler.schedule(h)
    │       │
    │       └─ scheduler.runReady() → 协程恢复
    │           └─ 回到读循环继续
    │
    └─ COMPLETE → 退出读循环
```

### 关键文件与函数

| 文件 | 函数 | 行号 | 作用 |
|------|------|------|------|
| `server/http/HttpSession/HttpSession.cpp` | `readRequest()` | L43-L100 | 读请求 + 增量解析 |
| `server/http/HttpParser/HttpParser.cpp` | `parse()` | L6-L106 | 协调三个子解析器的状态机 |
| `server/http/HttpParser/RequestLineParser.h` | `parse()` | — | 请求行 + query 解析 |
| `server/http/HttpParser/HeaderParser.h` | `parse()` | — | 首部 + range 解析 |
| `server/http/HttpParser/BodyParser.h` | `parse()` | — | 正文 + chunked 解析 |
| `server/http/HttpCodec/HttpCodec.cpp` | `decode()` | L4-L22 | Codec 层包装 parse + reset |
| `server/SubReactor/SubReactor.cpp` | `recvSocket()` | L247-L320 | 从内核读数据到 readBuffer |
| `server/SubReactor/SubReactor.cpp` | `wakeReadCoroutine()` | L57-L78 | 唤醒挂起的读协程 |
| `server/CoroutineScheduler/AWaiter.h` | `ReadAwaiter` | — | 读等待器（co_await 挂起） |

### 关键设计

| 设计点 | 说明 |
|--------|------|
| **增量解析** | 边收边解析，buffer 累积，天然支持分包/粘包 |
| **状态机** | HttpParser 维护 stage，跨多次 parse 保持状态 |
| **子解析器** | RequestLineParser/HeaderParser/BodyParser 职责分离 |
| **双重检查** | await_ready() 和 await_suspend() 都检查连接状态 |
| **背压** | readBuffer 超 MAX_PENDING_BYTES 时暂停读事件 |

---

## 四、阶段 ③④：Dispatch + Route — 投递到 Worker 执行

### 流程

```
HttpSession::run() 协程恢复
    │
    ├─ state = EXECUTING
    ├─ executor.submit(lambda)           ← 提交到 Worker 线程
    │   │
    │   │  lambda 内容（Worker 线程执行）：
    │   │   ├─ codec.dispatch(context_)
    │   │   │   └─ Router::handle(ctx)
    │   │   │       ├─ 中间件链
    │   │   │       ├─ 动态路由匹配（/user/:id）
    │   │   │       └─ handler 执行（sleep/IO/计算）
    │   │   │
    │   │   └─ notifyExecuteComplete(fd, connId)
    │   │       └─ write(event_fd, &one)
    │   │
    │   └─ co_await ExecuteAwaiter(reactor, fd)
    │       │
    │       │  （协程挂起，Reactor 继续处理其他事件）
    │       │
    │       │  ... epoll_wait 收到 event_fd ...
    │       │
    │       ├─ processComplete()
    │       │   ├─ 匹配 connId
    │       │   ├─ 连接仍存在 → wakeExecuteCoroutine(fd)
    │       │   └─ 连接已关闭 → zombieWakes 查找并调度
    │       │
    │       └─ scheduler.runReady() → 协程恢复
    │
    └─ 拿到 response → 进入发送阶段
```

### 关键文件与函数

| 文件 | 函数 | 行号 | 作用 |
|------|------|------|------|
| `server/http/HttpSession/HttpSession.cpp` | `executor.submit()` | L164 | 提交业务到 Worker 线程 |
| `server/Executor/Executor.h` | `submit()` | — | 线程池任务提交 |
| `server/http/HttpCodec/HttpCodec.cpp` | `dispatch()` | L24-L33 | 路由分发 + 响应对象获取 |
| `server/Route/Router.cpp` | `handle()` | — | 中间件链 + 路由匹配 + handler |
| `server/SubReactor/SubReactor.cpp` | `notifyExecuteComplete()` | L117-L134 | Worker→Reactor 完成通知（线程安全） |
| `server/SubReactor/SubReactor.cpp` | `processComplete()` | L138-L166 | 消费完成通知，唤醒协程 |
| `server/SubReactor/SubReactor.cpp` | `wakeExecuteCoroutine()` | L101-L114 | 唤醒执行完成的协程 |
| `server/CoroutineScheduler/AWaiter.h` | `ExecuteAwaiter` | — | 执行等待器 |

### 关键设计

| 设计点 | 说明 |
|--------|------|
| **IO 与业务分离** | handler 在 Worker 线程执行，不阻塞 Reactor |
| **connId 匹配** | 防止 Worker 完成时连接已被替换 |
| **zombieWakes** | 连接关闭后协程仍能被唤醒以安全退出 |
| **eventfd 解耦** | Worker 不直接触碰 epoll/conns，通过 eventfd 通知 |
| **有界队列** | Executor 队列满时返回 503，拒绝新请求（背压） |

---

## 五、阶段 ⑤：Send — 发送响应

### 流程

```
HttpSession::run() 协程恢复
    │
    ├─ response->buildHeader()           ← 构建响应头
    ├─ state = WRITING
    ├─ 进入发送循环
    │   │
    │   ├─ sender.send(fd, *response)    ← ★ 发送入口
    │   │   │
    │   │   ├─ sendHeader(fd, resp)
    │   │   │   └─ writev() + consume()
    │   │   │
    │   │   ├─ 如果有 Body：
    │   │   │   ├─ sendFileBody() → sendfile()    ← 零拷贝
    │   │   │   └─ sendMemoryBody() → writev()    ← 内存正文
    │   │   │
    │   │   └─ 返回 SendState:
    │   │       ├─ SEND_OK    → 发送完成
    │   │       ├─ SEND_AGAIN → EAGAIN，等待 EPOLLOUT
    │   │       └─ SEND_CLOSED → 连接关闭
    │   │
    │   ├─ SEND_AGAIN → co_await WriteAwaiter(reactor, fd)
    │   │   │
    │   │   │  ... epoll_wait 收到 EPOLLOUT ...
    │   │   │
    │   │   ├─ wakeWriteCoroutine(fd)
    │   │   │   └─ scheduler.schedule(h)
    │   │   │
    │   │   └─ 协程恢复 → continue 续发
    │   │
    │   ├─ SEND_OK → responsePool.release() + afterSend()
    │   │   │
    │   │   └─ afterSend()
    │   │       ├─ 恢复读事件（keep-alive）
    │   │       └─ fd_close（非 keep-alive）
    │   │
    │   └─ SEND_CLOSED → fd_close + co_return
    │
    └─ 回到主循环开头 → 读下一个请求（keep-alive）
```

### 关键文件与函数

| 文件 | 函数 | 行号 | 作用 |
|------|------|------|------|
| `server/http/HttpSession/HttpSession.cpp` | `sender.send()` | L240 | 发送入口 |
| `server/http/ResponseSender/ResponseSender.cpp` | `send()` | L3-L19 | 协调 header + body 发送 |
| `server/http/ResponseSender/ResponseSender.cpp` | `sendHeader()` | L21-L57 | 发送响应头（writev） |
| `server/http/ResponseSender/ResponseSender.cpp` | `sendFileBody()` | L120-L146 | 发送文件正文（sendfile 零拷贝） |
| `server/http/ResponseSender/ResponseSender.cpp` | `sendMemoryBody()` | L59-L118 | 发送内存正文（writev） |
| `server/http/HttpSession/HttpSession.cpp` | `afterSend()` | L14-L41 | 发送完成后恢复/关闭连接 |
| `server/SubReactor/SubReactor.cpp` | `wakeWriteCoroutine()` | L81-L98 | 唤醒写协程 |
| `server/CoroutineScheduler/AWaiter.h` | `WriteAwaiter` | — | 写等待器 |

### 关键设计

| 设计点 | 说明 |
|--------|------|
| **三态驱动** | SEND_OK / SEND_AGAIN / SEND_CLOSED 驱动发送循环 |
| **协程挂起** | SEND_AGAIN 时 co_await WriteAwaiter，让出 Reactor |
| **续发安全** | Body 保存消费偏移，SEND_AGAIN 后从断点续发 |
| **零拷贝** | 文件正文走 sendfile()，不经用户态 |
| **公平预算** | sendFileBody 4MB 预算，防止大文件独占 Reactor |
| **对象池** | Response 从对象池借出/归还，避免频繁分配 |

---

## 六、完整时序图

```
时间轴 ──────────────────────────────────────────────────────────→

主线程:  accept → addFd() → event_fd
                                      │
Reactor:                     epoll_wait 唤醒
                                      │
                              processPendingFds()
                              创建 Connection+Session+协程
                                      │
                              scheduler.runReady()
                              协程开始执行
                                      │
                     ┌──────── 读循环 ────────┐
                     │  readRequest()          │
                     │  decode→NEED_MORE?      │
                     │  recv→decode→OK         │
                     │  co_await ReadAwaiter   │
                     │  ↑ 被 EPOLLIN 唤醒     │
                     └─────────────────────────┘
                                      │
                              EXECUTING 阶段
                              executor.submit() → Worker
                              co_await ExecuteAwaiter
                              ↑ 被 eventfd 唤醒
                              processComplete()
                                      │
                              response->buildHeader()
                                      │
                     ┌──────── 写循环 ────────┐
                     │  sender.send()          │
                     │  sendHeader()→writev    │
                     │  sendFileBody→sendfile  │
                     │  SEND_AGAIN?           │
                     │  co_await WriteAwaiter  │
                     │  ↑ 被 EPOLLOUT 唤醒     │
                     │  SEND_OK→release+after │
                     └─────────────────────────┘
                                      │
                              keep-alive?
                              ├─ Yes → 回到读循环
                              └─ No  → fd_close + co_return
```

---

## 七、协程状态机

```
                    co_await ReadAwaiter
  ┌───────────────────────────────────────────┐
  │                                           │
  ▼                                           │
 READING ──→ PARSING ──→ EXECUTING ──→ WRITING │
  │              │          │          │      │
  │              │          │          │      │
  │              │          │          └──────┘
  │              │          │                   │
  │              │          └─ co_await ExecuteAwaiter
  │              │                              │
  │              └─ PARSE_ERROR/CLOSED           │
  │                 → CLOSED                     │
  │                                              │
  └─ co_await ReadAwaiter (need more)            │
                                                 │
              SEND_OK + keep-alive ──────────────┘
              (回到 READING)
```

### AwaitType 状态映射

| AwaitType | 触发阶段 | 等待事件 | 唤醒函数 |
|-----------|---------|---------|---------|
| `READ` | 读循环 | EPOLLIN | `wakeReadCoroutine()` |
| `EXECUTE` | Executor 等待 | eventfd（Worker 完成） | `wakeExecuteCoroutine()` |
| `WRITE` | 写循环 | EPOLLOUT | `wakeWriteCoroutine()` |

---

## 八、核心数据结构

### Connection

```cpp
// server/SubReactor/SubReactor.h
struct Connection {
    int fd;
    uint64_t id;                    // 唯一标识，防竞态
    ConnState state;                // closed/peerClosed/pauseByMemory/wantWrite
    Buffer readBuffer;              // 读缓冲区
    size_t pendingBytes;            // 未消费字节数（背压）
    std::shared_ptr<HttpSession> session;  // 所属 Session
};
```

### HttpSession

```cpp
// server/http/HttpSession/HttpSession.h
class HttpSession {
    int fd;
    SubReactor *reactor;
    HttpParser parser_;              // 增量解析器（有状态）
    RequestContext context_;         // 请求上下文
    SessionState state;             // READING/PARSING/EXECUTING/WRITING/CLOSED
    bool keepAlive_;
    
    struct CoroutineContext {
        std::coroutine_handle<> handle;
        AwaitType state;            // NONE/READ/EXECUTE/WRITE
        bool waiting;
    } coroutine_context;
};
```

### SubReactor

```cpp
// server/SubReactor/SubReactor.h
class SubReactor {
    int epfd;
    int event_fd;
    std::unordered_map<int, std::unique_ptr<Connection>> conns;
    CoroutineScheduler scheduler;
    Executor executor;
    ResponseSender sender;
    HttpCodec codec;
    TimeWheel wheel;
    
    // 跨线程通信
    std::queue<int> pendingFds;           // 新 fd 队列
    std::queue<std::pair<int,uint64_t>> completeQueue;  // 完成通知队列
    std::unordered_map<uint64_t, std::coroutine_handle<>> zombieWakes;  // 僵尸协程唤醒表
    std::atomic<bool> notified;
};
```

---

## 九、错误处理路径

| 错误类型 | 触发场景 | 处理方式 |
|---------|---------|---------|
| PARSE_ERROR | 畸形 HTTP 请求 | `fd_close` + `co_return` |
| SEND_CLOSED | 写失败/连接断开 | `fd_close` + `co_return` |
| Executor 队列满 | 业务过载 | 返回 503 Service Unavailable |
| Worker handler 异常 | 业务代码抛异常 | 捕获后转 500 Internal Server Error |
| 连接被对端关闭 | peerClosed=true | 标记后优雅关闭 |
| epoll 错误 | EPOLLERR | 立即 `fd_close` |
| EPOLLHUP/RDHUP | 对端半关闭 | 标记 peerClosed，延迟关闭 |
| 僵尸协程 | Worker 完成时连接已删除 | zombieWakes 查找并调度 |

---

## 十、文件索引

| 目录 | 文件 | 职责 |
|------|------|------|
| `server/Runtime/` | `ServerRuntime.h/cpp` | 服务器运行时，accept loop + ReactorGroup |
| `server/Reactor/` | `ReactorGroup.h/cpp` | 多 Reactor 管理，round-robin 分发 |
| `server/SubReactor/` | `SubReactor.h/cpp` | 核心 Reactor，epoll 事件循环 + 协程调度 |
| `server/Executor/` | `Executor.h` | 业务执行器，封装 ThreadPool |
| `server/CoroutineScheduler/` | `CoroutineScheduler.h/cpp` | 协程调度器，adopt/schedule/runReady/reap |
| `server/CoroutineScheduler/` | `AWaiter.h/cpp` | ReadAwaiter/WriteAwaiter/ExecuteAwaiter |
| `server/CoroutineScheduler/` | `Task.h` | Task 协程包装器，suspend_never |
| `server/http/HttpSession/` | `HttpSession.h/cpp` | HTTP 会话协程，读→执行→写循环 |
| `server/http/HttpCodec/` | `HttpCodec.h/cpp` | Codec 层，decode + dispatch 包装 |
| `server/http/HttpParser/` | `HttpParser.h/cpp` | 解析器协调器，状态机 |
| `server/http/HttpParser/` | `RequestLineParser.h` | 请求行解析 |
| `server/http/HttpParser/` | `HeaderParser.h` | 首部解析 |
| `server/http/HttpParser/` | `BodyParser.h` | 正文/chunked 解析 |
| `server/http/ResponseSender/` | `ResponseSender.h/cpp` | 响应发送器，header→body 发送 |
| `server/http/RequestContext/` | `RequestContext.h/cpp` | 请求上下文，请求+响应+参数 |
| `server/Route/` | `Router.h/cpp` | 路由匹配 + 中间件链 |
| `server/Buffer/` | `Buffer.h/cpp` | 缓冲区，append/retrieve/find |
| `server/BufferPoll/` | `BufferPoll.h/cpp` | 缓冲区池 |
| `server/SegmentPool/` | `SegmentPool.h/cpp` | 段池，ResponseSender 借用 |
| `server/ObjectPool/` | `ObjectPool.h/cpp` | 对象池，HttpResponse 复用 |
| `server/timer/` | `TimeWheel.h/cpp` | 时间轮，连接超时管理 |
| `server/Repsonse/` | `RespBody.h` | 响应体抽象基类 |
| `server/Repsonse/` | `StringBody.h/cpp` | 字符串正文 |
| `server/Repsonse/` | `FileBody.h/cpp` | 文件正文（sendfile） |
| `server/Repsonse/` | `ChunkedBody.h/cpp` | chunked 正文 |
| `server/Repsonse/` | `HeaderBody.h/cpp` | 响应头 |
| `server/threadpoll/` | `thread_pool.h/cpp` | 线程池（Executor 底层） |
| `log/logger/` | `logger.h/cpp` | 日志系统 |
