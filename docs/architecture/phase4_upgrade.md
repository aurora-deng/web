# 第四阶段升级文档 · WebSocket 介入与跨 Reactor 出站体系

> **阶段定位**：在第三阶段对象化 Runtime / Session 完成的基础上，**不推倒 Reactor**，换会话语义与帧模型，介入 WebSocket（RFC 6455），并引入统一的出站任务体系（OutboundTask + OutboundQueue + TransportWriter + writerLoop）替代旧版 HTTP 专用发送器。
> **文档职责**：记录本阶段架构演进的全部技术决策、控制流与跨 Reactor 投递链路；面向需要理解第四阶段架构的工程师。
> **当前版本差异见 → [项目 README](../../README.md)**，中文教学版见 → [第四阶段升级文档.md](第四阶段升级文档.md)。

---

## 一、升级目标与架构对齐

### 1.1 目标

1. 引入协议无关的 **Session** 基类（`server/session/Session/Session.h`，纯虚 `run()` + `onTimeout()`/`onClose()` 生命周期钩子），使 HTTP 与 WebSocket 可并列扩展。
2. 在既有 `ServerRuntime → ReactorGroup → SubReactor → Connection` 上，支持 **HTTP 握手升级 → WebSocket 会话交接 → 帧循环**。
3. 业务通过路由 `ctx.acceptWebSocket()` 显式接受升级；通过 `WebSocketDispatcher::on(type, handler)` 注册业务分发；通过 `WebSocketSessionManager` 实现跨 Reactor 主动推送（点对点聊天）。
4. 用统一的 **出站任务体系（OutboundTask + OutboundQueue + TransportWriter + writerLoop）** 取代旧版 HTTP 专用发送器，承载 HTTP 响应与 WS 帧的出站发送，支持 writev 聚集、sendfile 零拷贝与背压。
5. 保持第三阶段不变量：Reactor 独占 `conns`、跨 `co_await` 不缓存 `Connection*`、Completion / zombieWakes 语义不变。

### 1.2 目标架构（本阶段落地）

```text
                         ServerRuntime
                              │
                              ├─ Router            (协议无关路由)
                              ├─ Executor          (业务线程池)
                              ├─ WebSocketDispatcher wsDispatcher_  (按 type 注册 handler)
                              ├─ WebSocketSessionManager wsManager_ (全局会话目录)
                              ├─ WebSocketSessionFactory wsFactory_ (SessionFactory 实现，注入 SubReactor)
                              │
                         ReactorGroup(持 Router&/Executor&/SessionFactory&) ──postOutbound(reactorIndex,fd,connId,task)──┐
                              │                                                     │
                    ┌─────────┴─────────┐                                           │
                 SubReactor[0]      SubReactor[n]                                   │
                 (车间主任)          (车间主任)                                       │
                    │                   │                                           │
                 Connection          Connection                                     │
                 ├ ConnTransport     ├ ConnTransport  ◄── enqueueOutbound ──────────┘
                 │  outboundQueue    │  outboundQueue
                 │  pendingWriteBytes │  pendingWriteBytes
                 │  tickets           │  tickets
                 ├ ConnTimer          ├ ConnTimer
                 └ shared_ptr<Session>└ shared_ptr<Session>
                       │                    │
                  ┌────┴────┐          ┌────┴────┐
               HttpSession  ...    HttpSession  WebSocketSession
               │                    │              │
            HttpCodec            HttpCodec     WebSocketCodec
            HttpParser           HttpParser    WebSocketParser
            RequestContext       RequestContext
```

**关键变化**：协议编解码下沉到各 Session 子类内部；SubReactor 只持协议无关的 `Router&`；出站不再有 Sender 类，统一由 `OutboundQueue` + `TransportWriter` + `writerLoop` 协程驱动；SubReactor 持 `sessionFactory_` 抽象指针（依赖倒置），不直持 wsManager_/wsDispatcher_。

### 1.3 与第三阶段的关系

| 层次 | 第三阶段 | 第四阶段变化 |
|------|----------|--------------|
| ServerRuntime | 持 `HttpCodec codec_` | 删除 codec_；新增 `wsDispatcher_` + `wsManager_` + `wsFactory_`（WebSocketSessionFactory） |
| ReactorGroup | 构造 `(HttpCodec&, Executor&)` | 改 `(Router&, Executor&, SessionFactory&)`；`start` 时注入 `SessionFactory` 到每个 SubReactor；新增 `postOutbound` 跨 Reactor 投递 |
| SubReactor | 持 `HttpCodec&` + 旧版发送器 | 改持 `Router&`；新增 `transportWriter` + `OutboundQueue outbound_` + `sessionFactory_` + `reactorIndex_` + `writerLoop` 协程（依赖倒置） |
| Connection | 持 `shared_ptr<HttpSession>` + 内联传输字段 | 抽到 `transport/Connection.h`；持 `shared_ptr<Session>` + `outboundQueue` + 票据体系 + `CoroutineSlots` |
| Session 主循环 | 仅 HTTP 请求-响应 | HTTP 轮次 **或** WS 双向帧流；自持 Codec/Parser |
| 出站发送 | 旧版 HTTP 发送器 | `OutboundTask` + `OutboundQueue` + `TransportWriter` + `writerLoop` 协程（writev 聚集 / sendfile / 背压） |
| 业务分发 | 仅 HTTP 路由 | HTTP 路由 + `WebSocketDispatcher` 按 type 分发 + `WebSocketSessionManager` 跨 Reactor 推送 |

---

## 二、控制流：握手升级 → 会话交接 → 帧循环 → 业务分发

### 2.1 HTTP → WebSocket 升级与帧循环

```text
Accept → SubReactor::addFd → epoll_ctl(ADD, EPOLLIN|ONESHOT)
   → processPendingFds: make_shared<HttpSession>(fd, this, router)
                        + 启动 writerLoop 协程(CoroutineRole::Writer) + scheduler.adopt
   → HttpSession::run() 协程
        ├─ ReadAwaiter 挂起 → EPOLLIN → recvSocket
        ├─ HttpCodec::decode（HTTP 请求）
        ├─ ExecuteAwaiter → Executor Worker 执行 router.handle()
        │      └─ handler 调 ctx.acceptWebSocket() → webSocketAccepted=true
        ├─ 检查 webSocketAccepted
        │      ├─ 否 → encode 响应 → reserveOutboundTicket
        │      │        → OutboundTask::http(PooledHttpResponse) → enqueueOutbound
        │      │        → SendCompletionAwaiter 等完成 → afterSend（keep-alive/关闭）
        │      └─ 是 → prepareWebSocketUpgrade()
        │              ├─ WebSocketHandshake::isUpgradeRequest / validate
        │              ├─ fillResponse(101) + Sec-WebSocket-Accept
        │              ├─ reserveOutboundTicket + OutboundTask::http → enqueueOutbound
        │              ├─ SendCompletionAwaiter 等 101 发送完成
        │              └─ handoffWebSocket()
        │                     ├─ 从 queryParams 取 uid
        │                     ├─ 配置 wsHeartbeat 心跳参数
        │                     ├─ sessionFactory_->createWebSocketSession(fd, this, uid)  ← 经工厂造 WS 会话（依赖倒置）
        │                     ├─ conn->session = ws
        │                     └─ scheduler.adopt(WS 协程)
        └─ HttpSession co_return（协程结束，帧销毁由 scheduler.reap 回收）

   → WebSocketSession::run() 协程（由 runReady 排空队列时启动）
        ├─ manager->registerSession(uid, self, reactorIndex, fd, connId)
        └─ while (state == Open)
              ├─ WebSocketCodec::decode(readBuffer, frame)
              ├─ NeedMore → recvSocket → 再 decode（仍 NeedMore 则 co_await ReadAwaiter）
              ├─ Close → 回写 Close → fd_close → co_return
              ├─ Ping → enqueueOutbound(OutboundTask::encoded(encodePong))
              ├─ Pong → touchActivity → continue
              ├─ Continuation → 累积 fragmentBuf_（FIN=1 时重组走下方处理）
              └─ Text/Binary → startAppMessage()
                      ├─ messageFromFrame + 构造成员 WsMessageContext
                      ├─ submit Executor → co_await ExecuteAwaiter
                      ├─ Worker: codec_.dispatch(ctx)
                      ├─ 完成通知后回 Reactor: finishAppMessage()
                      └─ ctx.hasOutbound → enqueueOutbound(OutboundTask::encoded(outbound.payload()))
```

### 2.2 点对点聊天：跨 Reactor 主动推送（A 在 Reactor0 给 B 在 Reactor2 发消息）

```text
A(Reactor0) ──Text 帧 {"type":"chat","to":B,"content":"hi"}──►
   WebSocketSession[0]::startAppMessage → Executor Worker
      └─ codec_.dispatch(ctx)
            └─ 命中 "chat" handler
                  └─ ctx.manager->sendText(B_uid, text)
                        │
                  WebSocketSessionManager::sendText
                        ├─ WebSocketCodec::encodeText(text)   [A 线程编码]
                        └─ sendEncoded(B_uid, bytes)
                              ├─ 锁内：查 sessions_[B] → 拷贝 Locator{reactorIndex=2, fd, connId}
                              │         检查 weak_ptr 未 expired（过期则惰性 erase）
                              └─ 锁外：reactorGroup_->postOutbound(2, fd, connId,
                                              OutboundTask::encoded(bytes))
                                    │
                              ReactorGroup::postOutbound
                                    └─ reactors_[2]->postOutbound(fd, connId, task)
                                          │
                                    SubReactor[2]::postOutbound  [跨线程]
                                          ├─ 当前线程==目标线程? → 直接 enqueueOutbound
                                          └─ 否 → 锁 pendingSendMtx_ → push pendingOutbound_
                                                  → write(eventfd) 唤醒
                                          │
                                    SubReactor[2]::loop 醒来（eventfd 可读）
                                          └─ processPendingOutbound()
                                                ├─ 锁内 swap 出 pendingOutbound_ 队列
                                                └─ 逐条：按 fd+connId 校验 Connection 仍存活
                                                      └─ enqueueOutbound(fd, task)
                                                            ├─ transportWriter.enqueue
                                                            │   (挂到 Connection::outboundQueue)
                                                            ├─ updateEvent
                                                            └─ wakeCoroutine(fd, Writer, OUTBOUND)
                                                  │
                                            writerLoop 协程被唤醒
                                                  └─ transportWriter.flush(conn)
                                                        └─ flushEncoded: writev 聚集发送
                                                              └─ B 收到 Text 帧
```

**关键纪律**：
- 跨 Reactor 投递**不直接碰目标 Reactor 的 `conns`**——一律经 `postOutbound → eventfd → processPendingOutbound` 转交目标线程自己处理。
- `connId` 校验防止 fd 复用误投：旧连接关闭后系统可能把同一 fd 编号分配给新连接，ticket/connId 拒绝投递到"同 fd 不同连接"。
- 锁内只做查表拷贝 Locator，编码与 postOutbound 都在锁外执行，临界区极短。

---

## 三、新增文件清单（按模块分组）

### 3.1 传输出站任务体系（`server/transport/`）

| 路径 | 作用 |
|------|------|
| [`transport/OutboundTask.h`](../../server/transport/OutboundTask.h) | `OutboundTask`（payload 为 `variant<EncodedBufferTask, SharedEncodedBufferTask, HttpStreamTask>`）；静态工厂 `encoded`/`http`/`sharedEncoded`；`OutboundCompletion` 枚举；`PooledHttpResponse`（RAII 归还对象池） |
| [`transport/OutboundTask.cpp`](../../server/transport/OutboundTask.cpp) | 移动语义 + `remainingBytes()` |
| [`transport/OutboundQueue.h`](../../server/transport/OutboundQueue.h) | `OutboundQueue`（2.0 新增）：本线程 `enqueue` + 跨线程 `post` + `processPending`；回调注入解耦 SubReactor |
| [`transport/OutboundQueue.cpp`](../../server/transport/OutboundQueue.cpp) | 跨线程 pending 队列 + connId 代际校验 |
| [`transport/TransportWriter.h`](../../server/transport/TransportWriter.h) | `TransportWriter`（持 `SegmentPool&`+`TimerWheel&`）；`EnqueueResult`/`FlushStatus` 枚举；`enqueue`/`flush`；内部 `flushEncoded`（writev 聚集）/`flushHttp`（sendfile+writev）；背压常量 |
| [`transport/TransportWriter.cpp`](../../server/transport/TransportWriter.cpp) | writev 聚集 / sendfile / 高低水位背压实现 |
| [`transport/Connection.h`](../../server/transport/Connection.h) | `ConnTransport`（`deque<OutboundTask>` + `pendingWriteBytes` + tickets + `pauseByWrite`）+ `ConnTimer` + `Connection`（聚合 + `shared_ptr<Session>` + `CoroutineSlots`） |

### 3.2 会话基类（`server/session/`）

| 路径 | 作用 |
|------|------|
| [`session/Session/Session.h`](../../server/session/Session/Session.h) | 协议抽象基类：纯虚 `run()` + `onTimeout()`/`onClose()` 生命周期钩子。**唯一的协议抽象基类**，不存在 ProtocolCodec/Sender 基类 |

### 3.3 WebSocket 协议层五件套

| 路径 | 作用 |
|------|------|
| [`websocket/WebSocketTypes/WebSocketTypes.h`](../../server/websocket/WebSocketTypes/WebSocketTypes.h) | `WsOpcode` / `WsDecodeResult` / `WsFrame` / `WsCloseCode` |
| [`websocket/WebSocketTypes/UserId.h`](../../server/websocket/WebSocketTypes/UserId.h) | `using UserId = uint64_t;` |
| [`websocket/WebSocketHandshake/WebSocketHandshake.{h,cpp}`](../../server/websocket/WebSocketHandshake/WebSocketHandshake.h) | `isUpgradeRequest` / `validate` / `fillResponse`(101) / `computeAcceptKey`；内置 SHA-1+Base64（无 OpenSSL） |
| [`websocket/WebSocketParser/WebSocketParser.{h,cpp}`](../../server/websocket/WebSocketParser/WebSocketParser.h) | 帧解析状态机 `BASE_HEADER→EXT_LENGTH→MASK_KEY→PAYLOAD→COMPLETE`；组合 4 个子解析器（栈上成员） |
| [`websocket/WebSocketParser/FrameHeaderParser.h`](../../server/websocket/WebSocketParser/FrameHeaderParser.h) | 基础 2 字节头拆解 |
| [`websocket/WebSocketParser/ExtendedLengthParser.h`](../../server/websocket/WebSocketParser/ExtendedLengthParser.h) | 126/127 扩展长度展开 |
| [`websocket/WebSocketParser/MaskKeyParser.h`](../../server/websocket/WebSocketParser/MaskKeyParser.h) | 4 字节掩码密钥收取 |
| [`websocket/WebSocketParser/PayloadParser.h`](../../server/websocket/WebSocketParser/PayloadParser.h) | payload 取出 + 解掩码 |
| [`websocket/WebSocketParser/ParserUtils.h`](../../server/websocket/WebSocketParser/ParserUtils.h) | `kWsMaxPayloadBytes=1MB` / `WsParseContext` / `applyWsMask` / `isWsControlOpcode` |
| [`websocket/WebSocketCodec/WebSocketCodec.{h,cpp}`](../../server/websocket/WebSocketCodec/WebSocketCodec.h) | `class WebSocketCodec`（无继承）：`decode` 委托 Parser、`messageFromFrame`、`dispatch` 委托 Dispatcher、`encode`+静态 `encodeText/Binary/Close/Ping/Pong`（服务端发帧不掩码）；构造绑定 `WebSocketDispatcher&` |

### 3.4 WebSocket 业务层三件套（旧文档完全缺失）

| 路径 | 作用 |
|------|------|
| [`websocket/WebSocketMessage/WebSocketMessage.h`](../../server/websocket/WebSocketMessage/WebSocketMessage.h) | 应用信封：`v/type/id/replyTo/status/attempt/text/from/to/ack` + `payload()` / `isBinary()` |
| [`websocket/WebSocketDispatcher/WebSocketDispatcher.{h,cpp}`](../../server/websocket/WebSocketDispatcher/WebSocketDispatcher.h) | `WsHandler = function<bool(WsMessageContext&)>`；`on(type, handler)` / `onDefault(handler)` / `dispatch(ctx)`（先精确 type，再 fallback default） |
| [`websocket/WebSocketDispatcher/WsMessageContext.h`](../../server/websocket/WebSocketDispatcher/WsMessageContext.h) | `struct WsMessageContext { inbound; outbound; hasOutbound; keepConnection; uid; session*; manager*; }` |
| [`websocket/WebSocketSessionManager/WebSocketSessionManager.{h,cpp}`](../../server/websocket/WebSocketSessionManager/WebSocketSessionManager.h) | 全局会话目录 `unordered_map<UserId, Locator>`；`bind` / `registerSession` / `unregister` / `sendEncoded` / `sendText` / `broadcastText`（sharedEncoded 共享帧）/ `onlineCount` |
| [`websocket/WebSocketDelivery/WebSocketDeliveryTracker.{h,cpp}`](../../server/websocket/WebSocketDelivery/WebSocketDeliveryTracker.h) | 应用层消息 ID、ACK 归属校验、幂等窗口与超时重试决策；不执行 I/O |
| [`websocket/WebSocketDelivery/WebSocketDeliveryService.{h,cpp}`](../../server/websocket/WebSocketDelivery/WebSocketDeliveryService.h) | 观察最终写回执、周期自动重试、失败通知与 stop/join 生命周期 |

### 3.5 WebSocket 会话层

| 路径 | 作用 |
|------|------|
| [`websocket/WebSocketSession/WebSocketSession.{h,cpp}`](../../server/websocket/WebSocketSession/WebSocketSession.h) | `run()` 帧循环；`startAppMessage/finishAppMessage` 跨 Executor 两段式业务处理；`enqueueOutbound` 出站 |
| [`session/ProtocolSessionFactory.{h,cpp}`](../../server/session/ProtocolSessionFactory.h) | 本阶段曾使用 `WebSocketSessionFactory`；接入 SSE 后改为 `ProtocolSessionFactory`，继续承担 `SessionFactory` 的具体装配 |

### 3.6 其它

| 路径 | 作用 |
|------|------|
| [`CoroutineScheduler/CoroutineSlot.h`](../../server/CoroutineScheduler/CoroutineSlot.h) | `CoroutineSlots`（Reader/Writer/Executor 槽位），供 `Connection` 持有 |
| [`common/Units.h`](../../server/common/Units.h) | `server/common/` 新目录；通用单位/常量 |

---

## 四、修改文件清单（按文件列改动与作用）

### 4.1 SubReactor

**路径**：[`server/SubReactor/SubReactor.h`](../../server/SubReactor/SubReactor.h) / [`SubReactor.cpp`](../../server/SubReactor/SubReactor.cpp)

| 改动 | 作用 |
|------|------|
| 删旧版发送器 / `HttpCodec &codec`，改持 `Router &router` | 协议相关 codec/sender 下沉到 Session；SubReactor 只管 I/O 与生命周期 |
| 新增 `TransportWriter transportWriter` + `OutboundQueue outbound_` | 统一出站冲刷入口 + 跨线程投递窗口，替代旧 Sender |
| 新增 `sessionFactory_` / `reactorIndex_` | 依赖倒置：协议升级经 `SessionFactory` 抽象指针创建子类，不直持 wsManager_/wsDispatcher_ |
| `OutboundQueue outbound_`（内含 pending 队列 + `PendingOutbound`） | 跨线程/跨 Reactor 投递的回压通道（独立类，回调注入解耦） |
| `Connection` 持 `shared_ptr<Session>`（移到 transport/Connection.h） | 多态派发到 HTTP/WS |
| `processPendingFds`：建 HttpSession 后启动 `writerLoop` 协程并 `adopt` | 每连接一个 Writer 协程独占出站 |
| `loop()`：eventfd 分支调 `processPendingOutbound()` | 消费跨 Reactor 投递的任务 |
| `onWheelTimeout`：wsHeartbeat 心跳（Ping→waitingPong 超时→fd_close） | WS 长连接保活 |

### 4.2 ReactorGroup

**路径**：[`server/Reactor/ReactorGroup.h`](../../server/Reactor/ReactorGroup.h) / [`ReactorGroup.cpp`](../../server/Reactor/ReactorGroup.cpp)

| 改动 | 作用 |
|------|------|
| 构造 `(HttpCodec&, Executor&)` → `(Router&, Executor&, SessionFactory&)` | 2 参 → 3 参，注入协议升级工厂（依赖倒置） |
| 新增成员 `sessionFactory_` | 持有 `SessionFactory&` 抽象引用（WS 具体类型下沉到 websocket 模块） |
| 新增 `postOutbound(reactorIndex, fd, connId, OutboundTask)` | 跨 Reactor 投递出站任务的唯一入口 |
| `start()`：对每个 SubReactor 调 `setSessionFactory(&sessionFactory_)` / `setReactorIndex` | 注入协议升级工厂与下标；`wsManager_` 的 `bind` 由 ServerRuntime 侧完成 |

### 4.3 ServerRuntime

**路径**：[`server/Runtime/ServerRuntime.h`](../../server/Runtime/ServerRuntime.h) / [`ServerRuntime.cpp`](../../server/Runtime/ServerRuntime.cpp)

| 改动 | 作用 |
|------|------|
| 删 `HttpCodec codec_` | 协议无关化 |
| 新增 `WebSocketDispatcher wsDispatcher_` + `WebSocketSessionManager wsManager_` + `WebSocketSessionFactory wsFactory_` | WS 业务层宿主 + 协议升级工厂（经 ReactorGroup 注入 SubReactor） |
| 新增 `wsDispatcher()` / `wsManager()` 访问器 | 供 main.cpp 注册 handler |
| 初始化列表 `ReactorGroup(*router_, executor_, *wsFactory_)` | 串接 3 参构造（依赖倒置） |

### 4.4 HttpSession

**路径**：[`server/http/HttpSession/HttpSession.h`](../../server/http/HttpSession/HttpSession.h) / [`HttpSession.cpp`](../../server/http/HttpSession/HttpSession.cpp)

| 改动 | 作用 |
|------|------|
| `class HttpSession : public Session` | 纳入统一 Session 树 |
| 自持 `HttpCodec codec_(router)`（旧版经 `reactor->codec`） | 协议栈下沉 |
| **无 `sender_` 成员** | 发送改走 OutboundTask + OutboundQueue |
| 发送路径：`reserveOutboundTicket` + `OutboundTask::http` + `enqueueOutbound` + `SendCompletionAwaiter` | 异步等出站完成 |
| 新增 `prepareWebSocketUpgrade()` / `handoffWebSocket()` | 校验→101→会话交接 |
| `run()` 在 dispatch 后检查 `webSocketAccepted` | 业务显式接受后才升级 |

### 4.5 其它修改文件

| 文件 | 改动 | 作用 |
|------|------|------|
| [`RequestContext.h`](../../server/http/RequestContext/RequestContext.h) | `fd=-1`；新增 `webSocketAccepted` + `acceptWebSocket()` | 升级意图标志 |
| [`http.cpp`](../../server/http/http.cpp) | `buildHeader`：`hasConnection` 检测 + `status==101` 跳过 Content-Length | 保留 `Connection: Upgrade`，符合 Switching Protocols 无体语义 |
| [`HttpCodec.h`](../../server/http/HttpCodec/HttpCodec.h) | `class HttpCodec`（无继承）；`HttpCodec(Router&)` | 旧文档"继承 ProtocolCodec"为错误描述 |
| [`main.cpp`](../../main.cpp) | `/ws` 路由 + `wsDispatcher().on("chat",...)` + `onDefault(echo)` | 暴露 WS 端点与点对点聊天 |
| [`CMakeLists.txt`](../../CMakeLists.txt) | webserver_core 分 4 组源文件 + 6 个测试 | 构建系统对齐 |

---

## 五、跨 Reactor 出站投递体系（核心，旧文档完全缺失）

> 旧版文档用专门的 HTTP 发送器类描述出站，但新版**不存在 Sender 类**。真实的出站机制是 **OutboundTask + OutboundQueue + TransportWriter + writerLoop 协程 + postOutbound** 这套统一任务体系。本章是理解第四阶段的关键。

### 5.1 OutboundTask：协议无关的出站任务

```cpp
// server/transport/OutboundTask.h
struct EncodedBufferTask      { std::string bytes; std::size_t offset = 0; };  // WS 帧 / 已编码字节
struct SharedEncodedBufferTask { std::shared_ptr<const std::string> bytes;       // 广播共享同一帧
                                 std::size_t offset = 0; };
struct HttpStreamTask         { PooledHttpResponse response; };                 // HTTP 响应（对象池 RAII）

class OutboundTask {
  public:
    using Payload = std::variant<EncodedBufferTask, SharedEncodedBufferTask, HttpStreamTask>;
    static OutboundTask encoded(std::string bytes, uint64_t ticket=0, OutboundCompletion=None);
    static OutboundTask http(PooledHttpResponse, uint64_t ticket, OutboundCompletion=None);
    static OutboundTask sharedEncoded(std::shared_ptr<const std::string>, uint64_t ticket=0, ...);
    Payload payload; uint64_t ticket; OutboundCompletion completion;
};
```

- **三种 payload 覆盖全部出站场景**：WS 帧（encoded）、HTTP 响应（http，含 sendfile）、广播（sharedEncoded，多个目标共享同一 `shared_ptr<const string>` 避免重复拷贝）。
- `ticket` 是**顺序票据**：HTTP 响应必须按请求顺序发出，`reserveOutboundTicket` 分配递增 ticket，`writerLoop` 按 `completedTicket` 推进，保证乱序入队后仍按序完成。
- `OutboundCompletion::CloseConnection` 表示发完即关连接（HTTP `Connection: close`）。

### 5.2 TransportWriter：writev 聚集 + sendfile + 背压

```text
TransportWriter（持 SegmentPool& + TimerWheel&）
  ├─ enqueue(Connection&, OutboundTask)  →  挂到 Connection::outboundQueue
  │     返回 EnqueueResult{Ok, Backpressure, Closed}
  └─ flush(Connection&, byteBudget=256KiB)
        → 返回 FlushStatus{Drained, Blocked, Yielded, Error} + bytesWritten
        ├─ flushEncoded: 聚集 outboundQueue 中所有 EncodedBufferTask
        │     → writev(iovec[], n) 一次性发出（减少 syscall）
        ├─ flushHttp: HttpStreamTask
        │     若 body 是 FileBody → sendfile() 零拷贝
        │     否则 writev 头部 + body
        ├─ 公平预算：encoded / HTTP 内存体 / sendfile 共用每轮 256KiB
        │     预算耗尽且队列未空 → Yielded，重新经过 epoll 调度
        └─ 背压：pendingWriteBytes 超过 kWriteHighWatermark(4MB)
                → 暂停读(EPOLLIN 撤销)，避免读太快撑爆写缓冲
                降到 kWriteLowWatermark(2MB) → 恢复读
```

| 常量 | 值 | 作用 |
|------|----|------|
| `kWriteHighWatermark` | 4MB | 写积压超此值触发背压，撤销读关注 |
| `kWriteLowWatermark` | 2MB | 写积压降到此值恢复读 |
| `kMaxOutboundTasks` | 4096 | 单连接出站任务上限，防 OOM |
| `kWriteQuantumBytes` | 256KiB | 单连接一次 flush 的实际写出上限 |

### 5.3 writerLoop：每连接单出站协程

```text
SubReactor::processPendingFds
  → make_shared<HttpSession>(fd, this, router)
  → 启动 writerLoop(fd, connId) 协程（CoroutineRole::Writer）
  → scheduler.adopt  (与 HttpSession 的 Reader 协程并列)

writerLoop 协程循环:
  while (running && conn 存活):
    co_await wakeCoroutine  ←─ 等待 OUTBOUND 唤醒
    transportWriter.flush(conn)
      ├─ Drained → 队列空，继续等
      ├─ Blocked → EAGAIN，updateEvent(EPOLLOUT) + co_await 等可写
      ├─ Yielded → 公平预算耗尽，rearm EPOLLOUT + co_await 让出 Reactor
      └─ Error → fd_close
```

**为何每连接单 Writer 协程**：把出站 I/O 串行化，避免多协程并发 write 交错破坏帧边界；与 Reader 协程并列但互不干扰，读快写慢时由背压自动节流。

### 5.4 postOutbound：跨 Reactor / 跨线程投递

```text
任意线程调 ReactorGroup::postOutbound(reactorIndex, fd, connId, task)
  → reactors_[reactorIndex]->postOutbound(fd, connId, task)
        │
  SubReactor::postOutbound(fd, connId, task)  [线程安全]
        ├─ 若 std::this_thread::get_id() == 本 Reactor 线程
        │     → 直接 enqueueOutbound(fd, task)   (同线程，免锁)
        └─ 否
              → outbound_.post(fd, connId, task, event_fd, notified)
              → write(eventfd) 唤醒目标 Reactor 线程
        │
  目标 Reactor 线程 loop() 醒来（eventfd 可读）
        → processPendingOutbound()
              ├─ 锁内 swap pending_ 到局部队列（OutboundQueue 内部，临界区极短）
              └─ 逐条：按 fd+connId 校验 Connection 仍存活（防 fd 复用）
                    → enqueueOutbound(fd, task)  [同线程]
```

**纪律**：跨线程**绝不直接访问目标 Reactor 的 `conns`**——一律经 eventfd 转交目标线程自己处理，消除数据竞争。

### 5.5 SessionManager Locator：跨 Reactor 定位

```cpp
// server/websocket/WebSocketSessionManager/WebSocketSessionManager.h
struct Locator {
    std::weak_ptr<WebSocketSession> session;  // 弱引用：Session 销毁自动失效，无循环引用
    size_t reactorIndex = 0;                  // 所在 SubReactor 下标（postOutbound 据此定位线程）
    int fd = -1;                              // 目标 Reactor 用它在 conns 找 Connection
    uint64_t connId = 0;                      // 防 fd 复用错位
};
std::unordered_map<UserId, Locator> sessions_;
```

- **为何 weak_ptr**：Session 真实所有权在 `Connection::session`（shared_ptr）。Manager 若持 shared_ptr 会形成"Connection 持 Session，Session 经 Manager 间接持 Connection"循环引用，导致内存泄漏。weak_ptr 只观察不拥有，expired 时惰性 erase。
- **为何单独记 reactorIndex/fd/connId**：weak_ptr 只能告诉你"还活着"，不能告诉你"在哪条线程"。跨 Reactor 投递必须知道 reactorIndex 才能调 postOutbound。

### 5.6 完整跨 Reactor 投递链路（A→B）

| 步骤 | 位置 | 动作 |
|------|------|------|
| 1 | Reactor0 · A 的 WSSession | 收帧→`codec_.decode`→`messageFromFrame`→提交 Executor，根协程等待 |
| 2 | Worker · dispatcher | `dispatch(ctx)`→命中 "chat" handler→`ctx.manager->sendText(B, text)` |
| 3 | Worker · SessionManager | `sendText`→`sendEncoded`→`WebSocketCodec::encodeText` 编码 |
| 4 | Worker · SessionManager | 加锁查 `sessions_[B]` 拿 Locator(reactorIndex=2)，检查 weak_ptr 未 expired |
| 5 | Worker · SessionManager | 释放锁→`reactorGroup_->postOutbound(2, fd, connId, OutboundTask::encoded(bytes))` |
| 6 | Worker → ReactorGroup | `reactors_[2]->postOutbound(fd, connId, task)` |
| 7 | Reactor2 · SubReactor::postOutbound | 跨线程→锁 push `pendingOutbound_` + write eventfd |
| 8 | Reactor2 · loop | eventfd 醒来→`processPendingOutbound`→swap 队列→按 fd+connId 校验→`enqueueOutbound` |
| 9 | Reactor2 · enqueueOutbound | `transportWriter.enqueue`（挂 outboundQueue）+ `updateEvent` + `wakeCoroutine(Writer, OUTBOUND)` |
| 10 | Reactor2 · writerLoop | 被唤醒→`transportWriter.flush`→`writev` 发出 |

---

## 六、WebSocket 行为约定（能力矩阵）

| 能力 | 状态 | 说明 |
|------|------|------|
| HTTP/1.1 Upgrade 握手（GET + Version 13） | ✅ | `WebSocketHandshake::validate` |
| Text / Binary 帧 | ✅ | `WebSocketCodec::decode` |
| 分片重组（Continuation） | ✅ | `fragmentBuf_` + `fragmentOpcode_` |
| Ping → 自动 Pong | ✅ | 帧循环内 `enqueueOutbound(encodePong)` |
| Close 对端关闭码回写 | ✅ | RFC6455 §7.1.2 双向确认 |
| 默认 echo | ✅ | `onDefault` handler |
| 自定义业务分发（按 type） | ✅ | `WebSocketDispatcher::on(type, handler)` |
| 点对点聊天（跨 Reactor 推送） | ✅ | `WebSocketSessionManager::sendText` |
| 广播（共享同一帧） | ✅ | `broadcastText` + `sharedEncoded` |
| WS 心跳保活 | ✅ | `onWheelTimeout`：Ping→waitingPong 超时关闭 |
| 扩展 RSV / permessage-deflate | ❌ | 本阶段不做 |
| 客户端掩码发出（服务端角色） | ❌ | 服务端发送永不 mask（RFC6455 §5.1） |
| WSS（TLS） | ❌ | 留给后续阶段 |
| 子协议协商 / 多路复用 | ❌ | |

---

## 七、验证方式

### 7.1 单元测试

```bash
cmake -S . -B build-tests -DBUILD_TESTING=ON
cmake --build build-tests -j
ctest --test-dir build-tests --output-on-failure
```

测试文件：`unit_tests.cpp` + `transport_tests.cpp` + `coroutine_lifecycle_tests.cpp`，覆盖 Accept Key（RFC6455 示例）、Version 13 门禁、Text 帧编解码往返、未掩码帧拒绝、半包 NeedMore、acceptWebSocket 标志、出站任务票据、协程生命周期。

### 7.2 黑盒集成

```bash
# 终端 1
./webserver
# 终端 2（websocat，点对点聊天）
websocat "ws://127.0.0.1:8080/ws?uid=1001"   # A
websocat "ws://127.0.0.1:8080/ws?uid=1002"   # B（另一终端）
# A 发送：{"type":"chat","to":1002,"content":"hello"} → B 收到 "hello"
# A 发送：@1002:hello                          → B 收到 "hello"
# 可靠路径：A 发送 {"v":1,"type":"chat","id":"c-1","to":1002,"content":"hello"}
# B 收到服务端 id 后发送 {"v":1,"type":"ack","replyTo":"ws-1001-boot7-1"}
# 无 type 匹配 → echo 回显
```

`http_blackbox.py` / `websocket_blackbox.py` 提供自动化黑盒用例。

---

## 八、后续改进登记表

| 序号 | 改进项 | 状态 | 预计带动路径 |
|------|--------|------|----------------|
| 1 | WS 写背压与读暂停策略细化（两级邮箱准入、结果回传、慢连接关闭） | 已完成（第八阶段） | `OutboundAdmission`、`TransportWriter`、`SubReactor` |
| 1.1 | 异步最终回执与 Writer 每轮公平预算 | 已完成（第九阶段） | `OutboundReceipt`、`TransportWriter`、`ChunkedBody` |
| 1.2 | WS 应用信封、ACK、幂等窗口与重试决策 | 已完成（第十阶段） | `WebSocketMessage`、`WebSocketCodec`、`WebSocketDeliveryTracker` |
| 1.3 | 周期驱动自动重发、最终写回执与失败通知 | 已完成（第十一阶段） | `ServerRuntime`、`WebSocketDeliveryService`、`OutboundReceipt` |
| 1.4 | 接收端有界幂等窗口、正确 ACK 顺序与重复包补 ACK | 已完成（第十二阶段） | `examples/reliable_websocket_consumer.py`、Python unit / WS black-box |
| 1.5 | 指数退避、确定性抖动与投递指标端点 | 已完成（第十三阶段） | `WebSocketDeliveryTracker`、`WebSocketDeliveryService::metrics`、`/delivery-metrics` |
| 1.6 | 投递持久化、延迟直方图与外部指标后端 | 待做 | durable store、Prometheus / OpenTelemetry |
| 2 | 子协议 / 扩展协商（permessage-deflate） | 待做 | `WebSocketHandshake`、`RequestContext` |
| 3 | WSS（TLS 终止或 openssl 接入） | 待做 | `ServerRuntime`、新 `TlsTransport` |
| 4 | WS 消息处理投递 Executor（连接内串行、跨连接并行） | 已完成，见 phase14 | `WebSocketSession`、`ExecuteAwaiter` |
| 4.1 | handler 协作式取消、5 秒 deadline、HTTP/WS 容量隔离 | 已完成，见 phase15 | `HandlerCancellation`、`ServerRuntime`、HTTP/WS Context |
| 5 | 指标：握手次数、帧数、关闭码分布、跨 Reactor 投递量 | 待做 | metrics / 日志 |
| 6 | SessionManager 分片锁（高在线量场景） | 待做 | `WebSocketSessionManager` |
| 7 | 广播批量编码优化（多 type 合并） | 待做 | `WebSocketSessionManager` |

---

## 九、改动路径速查（目录树）

```text
web-test6.1/
├── main.cpp                          # + /ws 路由 + wsDispatcher on("chat")/onDefault
├── CMakeLists.txt                    # webserver_core 分 4 组源文件 + 6 测试
├── server/
│   ├── transport/                    # 【整目录新增】出站任务体系
│   │   ├── OutboundTask.{h,cpp}      #   出站任务抽象（variant 三态）
│   │   ├── OutboundQueue.{h,cpp}     #   出站投递窗口（2.0 新增：enqueue/post/processPending）
│   │   ├── TransportWriter.{h,cpp}   #   writev 聚集 / sendfile / 背压
│   │   └── Connection.h              #   ConnTransport+ConnTimer+Connection
│   ├── session/Session/Session.h     # 【新增】协议抽象基类（唯一基类，run()+onTimeout/onClose）
│   ├── session/SessionFactory.h      # 【2.0 新增】协议升级装配台抽象接口
│   ├── websocket/                    # 【整目录新增】与 http/ 对称
│   │   ├── WebSocketTypes/{WebSocketTypes.h, UserId.h}
│   │   ├── WebSocketHandshake/WebSocketHandshake.{h,cpp}
│   │   ├── WebSocketParser/{WebSocketParser.{h,cpp}, FrameHeaderParser.h,
│   │   │                          ExtendedLengthParser.h, MaskKeyParser.h,
│   │   │                          PayloadParser.h, ParserUtils.h}
│   │   ├── WebSocketCodec/WebSocketCodec.{h,cpp}
│   │   ├── WebSocketMessage/WebSocketMessage.h
│   │   ├── WebSocketDelivery/WebSocketDeliveryTracker.{h,cpp}
│   │   ├── WebSocketDelivery/WebSocketDeliveryService.{h,cpp}
│   │   ├── WebSocketDispatcher/{WebSocketDispatcher.{h,cpp}, WsMessageContext.h}
│   │   ├── WebSocketSessionManager/WebSocketSessionManager.{h,cpp}
│   │   ├── WebSocketSessionFactory.{h,cpp}  # 【2.0 新增】SessionFactory 具体实现
│   │   └── WebSocketSession/WebSocketSession.{h,cpp}
│   ├── SubReactor/SubReactor.{h,cpp} # 改：router/transportWriter/outbound_(OutboundQueue)/sessionFactory_/writerLoop/postOutbound
│   ├── Reactor/ReactorGroup.{h,cpp}  # 改：3 参构造(Router,Executor,SessionFactory) + postOutbound
│   ├── Runtime/ServerRuntime.{h,cpp}# 改：wsDispatcher_/wsManager_/wsFactory_
│   ├── CoroutineScheduler/CoroutineSlot.h  # 【新增】CoroutineSlots
│   ├── common/Units.h                # 【新增】
│   ├── http/
│   │   ├── HttpSession/HttpSession.{h,cpp}  # 改：继承 Session + 升级 + OutboundTask 发送
│   │   ├── RequestContext/RequestContext.h   # 改：webSocketAccepted + acceptWebSocket
│   │   ├── HttpCodec/HttpCodec.h             # 改：HttpCodec(Router&)（无继承）
│   │   └── http.cpp                          # 改：buildHeader 101 分支
│   └── (其它目录重命名: buffer_pool/ response/ thread_pool/)
└── tests/                            # unit + transport + coroutine_lifecycle + blackbox
```

---

## 十、设计取舍说明

### 10.1 为何用 OutboundTask 取代旧版 HTTP 发送器

旧版 HTTP 发送器是 HTTP 专用的同步发送器。引入 WebSocket 后出站需求多样化（WS 帧、HTTP 响应、广播），且需要 writev 聚集与 sendfile 零拷贝。`OutboundTask` 用 `variant` 统一三种 payload，`OutboundQueue` 统一本线程/跨线程投递，`TransportWriter` 统一冲刷策略，`writerLoop` 串行化出站 I/O——一套体系同时服务 HTTP 与 WS，避免协议分叉。

### 10.2 为何 SessionManager 用 weak_ptr 而非 shared_ptr

Session 真实所有权在 `Connection::session`（shared_ptr）。Manager 若持 shared_ptr，会形成"Connection 持 Session，Session 经 Manager 间接持 Connection"的循环引用，导致内存泄漏。weak_ptr 只观察不拥有，Session 销毁时自动 expired，Manager 检测到后惰性 erase 死条目。

### 10.3 为何每连接单 writerLoop 协程

把出站 I/O 串行化到一个协程：避免多协程并发 `write` 导致帧字节交错破坏协议边界；与 Reader 协程并列但通过 `CoroutineSlots` 分槽互不干扰；读快写慢时由 `pendingWriteBytes` 背压自动节流读端。单协程模型也简化了票据推进（`completedTicket` 单调递增）。

### 10.4 为何点对点聊天走 SessionManager 而非直接投递

跨 Reactor 推送必须知道目标在哪条线程。`WebSocketSession` 只持本连接的 `reactor_`，不知道其它用户的位置。`SessionManager` 维护全局 `UserId → Locator(reactorIndex, fd, connId)` 目录，调用方只需 `sendText(uid, text)`，Manager 查表后调 `postOutbound` 投递，屏蔽"目标在哪个线程"的细节。

### 10.5 为何跨 Reactor 投递用 eventfd 而非直接加锁 enqueue

`conns` 表只允许本 Reactor 线程访问（无锁设计）。若跨线程直接 `enqueueOutbound` 操作目标 Connection，会与目标线程的 epoll/协程逻辑竞争。eventfd 把任务暂存 `pendingOutbound_` 队列，唤醒目标线程在 `processPendingOutbound` 里自己处理，所有 `conns` 操作集中在本线程，消除数据竞争。

### 10.6 为何用 ticket 票据保证 HTTP 响应顺序

HTTP/1.1 Keep-Alive 下多个请求可能乱序入队（流水线 + Executor 异步），但响应必须按请求顺序发出。`reserveOutboundTicket` 分配递增 ticket，`writerLoop` 按 `completedTicket` 推进，只有 ticket 匹配时才 flush，保证乱序入队后仍按序完成。WS 帧无此约束（ticket=0，FIFO）。
