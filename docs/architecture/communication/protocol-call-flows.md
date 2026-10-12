# 底层协议逻辑与调用关系

本文按当前代码说明每个通信功能。图中的“业务 handler”是通信层边界，具体账号、聊天和 AI 逻辑在
[业务调用流程](../business/business-call-flows.md) 中展开。

## 1. Reactor、Connection 与协程

```mermaid
sequenceDiagram
    participant A as Acceptor
    participant G as ReactorGroup
    participant R as SubReactor
    participant E as epoll
    participant C as CoroutineScheduler
    participant S as Session

    A->>G: addFd(fd, listener kind)
    G->>R: pending fd + eventfd
    R->>E: EPOLL_CTL_ADD(ET + ONESHOT)
    R->>S: SessionFactory 创建初始会话
    R->>C: adopt(session.run(), owner)
    C->>S: resume
    S-->>C: co_await Read/Write/ExecuteAwaiter
    E-->>R: EPOLLIN / EPOLLOUT
    R->>C: schedule(handle)
    C->>S: resume
    S-->>C: final_suspend
    C->>C: reap，唯一销毁协程帧
```

关键点：

1. `fd` 像房间号，可能被系统复用；`ConnectionKey(fd, connId)` 才像“房间号 + 本次入住单号”。
2. 连接表只由所属 SubReactor 修改，跨线程任务必须通过邮箱和 `eventfd` 回到该线程。
3. 协程只是把状态机写得像顺序代码，不会把阻塞调用自动变成异步调用。
4. Reactor 关闭连接时请求协程协作退出，由 Scheduler 在 `final_suspend` 后统一销毁帧。

代码入口：[`ReactorGroup`](../../../server/Reactor/ReactorGroup.cpp)、
[`SubReactor`](../../../server/SubReactor/SubReactor.cpp)、
[`CoroutineScheduler`](../../../server/CoroutineScheduler/CoroutineScheduler.cpp)、
[`Connection`](../../../server/transport/Connection.h)。

## 2. HTTP/1.1

```mermaid
flowchart LR
    Bytes[readBuffer 字节] --> Parser[HttpParser 增量状态机]
    Parser -->|NEED_MORE| Await[ReadAwaiter]
    Parser -->|完整请求| Codec[HttpCodec]
    Codec --> Policy{Router execution policy}
    Policy -->|ReactorSafe| Router[Router]
    Policy -->|默认| Exec[HTTP Executor]
    Exec --> Router
    Router --> Resp[HttpResponse]
    Resp --> Task[OutboundTask::http]
    Task --> Queue[OutboundQueue]
    Queue --> Writer[TransportWriter]
```

- 解析器保留请求行、Header、定长 Body、chunked Body 的中间状态，应对拆包和粘包。
- pipeline 的剩余字节留在 Buffer 中，下一轮先解析现有数据，再等待新的 `EPOLLIN`。
- Router 冻结后走只读查找；普通业务进入有界 Executor，避免慢 handler 卡住 Reactor。
- 队列满、业务异常和 deadline 分别收口为过载、500 和 504 类结果。

代码入口：[`HttpParser`](../../../server/http/HttpParser/HttpParser.cpp)、
[`HttpSession`](../../../server/http/HttpSession/HttpSession.cpp)、
[`HttpCodec`](../../../server/http/HttpCodec/HttpCodec.h)、
[`Router`](../../../server/Route/Router.cpp)。

## 3. WebSocket

```mermaid
flowchart TD
    H1[HTTP Upgrade 请求] --> HS[WebSocketHandshake]
    HS --> Factory[ProtocolSessionFactory]
    Factory --> Session[WebSocketSession]
    Session --> Parser[WebSocketParser]
    Parser --> Validate[掩码/长度/opcode/UTF-8 校验]
    Validate --> Assemble[MessageAssembler<br/>分片重组]
    Assemble --> Exec[WebSocket Executor]
    Exec --> Dispatcher[WebSocketDispatcher]
    Dispatcher --> App[业务消息处理器]
    App --> Delivery[DeliveryService<br/>幂等/ACK/重试]
    Delivery --> Manager[SessionManager<br/>用户到多连接]
    Manager --> Queue[跨 Reactor OutboundQueue]
    Queue --> Codec[WebSocketCodec 编帧]
    Codec --> Writer[唯一 writerLoop]
```

- 握手只完成协议升级，之后 HTTP Session 退出，由 WebSocket Session 接管同一 Connection。
- 帧边界不是业务消息边界；Assembler 将 continuation 帧组合成一条完整消息。
- `ping/pong/close` 属于协议控制帧；`chat.send/chat.ack` 属于上层业务信封。
- 投递分为“邮箱接纳、写入内核、对端业务 ACK”三种不同完成程度。
- 接收方离线不等于业务发送失败：消息先持久化；实时推送只负责加速在线到达。

代码入口：[`WebSocketSession`](../../../server/websocket/WebSocketSession/WebSocketSession.cpp)、
[`WebSocketParser`](../../../server/websocket/WebSocketParser/WebSocketParser.cpp)、
[`WebSocketDispatcher`](../../../server/websocket/WebSocketDispatcher/WebSocketDispatcher.cpp)、
[`WebSocketDeliveryService`](../../../server/websocket/WebSocketDelivery/WebSocketDeliveryService.cpp)。

## 4. SSE

```mermaid
sequenceDiagram
    participant B as Browser EventSource
    participant H as HTTP handler
    participant M as SseSessionManager
    participant S as SseSession
    participant O as OutboundQueue

    B->>H: GET /events + session + Last-Event-ID
    H->>S: 发送 200 + text/event-stream
    H->>M: 注册 user + ConnectionKey
    M->>S: 回放游标之后的事件
    S->>O: SSE event 编码后再做 HTTP chunk 编码
    O-->>B: data/event/id + chunk framing
    loop 空闲期间
        S->>O: 注释心跳
    end
    B--xS: TCP 断开
    S->>M: 按 ConnectionKey 注销
```

SSE 是一条长时间不结束的 HTTP 响应。SSE 格式负责事件语义，HTTP chunked 负责传输边界；二者不能混为一层。
`SseReplayBuffer` 保存有界历史，浏览器重连时用 `Last-Event-ID` 补发。SSE 适合通知与 AI token，客户端命令仍走
HTTP 或 WebSocket。

代码入口：[`SseCodec`](../../../server/sse/SseCodec.cpp)、
[`SseSession`](../../../server/sse/SseSession.cpp)、
[`SseSessionManager`](../../../server/sse/SseSessionManager.cpp)、
[`SseReplayBuffer`](../../../server/sse/SseReplayBuffer.cpp)。

## 5. HTTP/2、TLS 与 ALPN

```mermaid
flowchart TD
    TCP[TCP accept] --> Kind{listener 类型}
    Kind -->|明文| Peek{Client Preface?}
    Peek -->|否| H1[HTTP/1.1]
    Peek -->|是| H2[Http2Session]
    Kind -->|TLS| Handshake[TlsSession / SSL_accept]
    Handshake --> ALPN{ALPN 结果}
    ALPN -->|http/1.1| H1
    ALPN -->|h2| H2
    H2 --> Preface[Client Preface]
    Preface --> Settings[SETTINGS / ACK]
    Settings --> Frames[nghttp2 帧解析 + HPACK]
    Frames --> Streams[多 Stream 状态]
    Streams --> Request[HttpRequest / Router]
    Request --> Response[HEADERS + DATA]
    Response --> Flow[流量控制 + 调度]
```

- 明文端口使用 prior knowledge 前言识别；TLS 端口使用 ALPN 明确选择 `h2` 或 `http/1.1`。
- 生产路径把帧、HPACK、流状态和流量控制交给 nghttp2；`server/http2/learn` 是可逐步阅读的简化教学版。
- TLS 位于 HTTP 下面。`TlsTransport` 把 OpenSSL 的 `WANT_READ/WANT_WRITE` 映射回协程等待。
- HTTP/2 的 Stream 是一条连接里的逻辑请求，不等于线程；Stream 协程仍受连接级和流级窗口约束。

代码入口：[`Http2Codec`](../../../server/http2/Http2Codec.cpp)、
[`Http2StreamCoroutine`](../../../server/http2/Http2StreamCoroutine.h)、
[`TlsSession`](../../../server/tls/TlsSession.cpp)、
[`TlsTransport`](../../../server/tls/TlsTransport.cpp)。

## 6. gRPC

```mermaid
flowchart LR
    Proto[web_learning.proto] --> Generated[Protobuf / Stub / Service]
    Client[gRPC Client] --> Listener[gRPC listener<br/>HTTP/2 + TLS 可选]
    Listener --> Callback[CallbackService]
    Callback --> Reactor[RPC Reactor]
    Reactor --> Method{RPC 形态}
    Method --> Unary[Unary]
    Method --> ServerStream[Server Streaming]
    Method --> ClientStream[Client Streaming]
    Method --> Bidi[Bidi Streaming]
    Unary --> Finish[Finish + OnDone]
    ServerStream --> Finish
    ClientStream --> Finish
    Bidi --> Finish
```

gRPC 使用 HTTP/2，但业务看到的是 Service/Method、Metadata、Deadline、Cancellation、Status 和 Streaming。
当前生产路径使用官方 gRPC C++ Callback API；`server/grpc/learn` 只演示 5 字节消息信封，不重复手写完整 HTTP/2。
Callback 对象的最终清理在 `OnDone`，避免异步 Alarm 或回调仍引用已经销毁的上下文。

代码入口：[`proto`](../../../server/grpc/proto/web_learning.proto)、
[`GrpcServer`](../../../server/grpc/GrpcServer.cpp)、
[`GrpcMain`](../../../server/grpc/GrpcMain.cpp)、
[`教学 Codec`](../../../server/grpc/learn/GrpcMessageCodec.cpp)。

## 7. 统一出站与背压

```mermaid
flowchart LR
    H[HTTP response] --> T[OutboundTask]
    W[WebSocket frame] --> T
    S[SSE chunk] --> T
    X[跨 Reactor 投递] --> Admission[OutboundAdmission]
    T --> Admission
    Admission -->|超过任务/字节上限| Reject[拒绝/过载结果]
    Admission -->|准入| Q[OutboundQueue]
    Q --> Loop[每连接唯一 writerLoop]
    Loop --> Writer[TransportWriter]
    Writer -->|明文| Sys[writev / sendfile]
    Writer -->|TLS| SSL[SSL_write_ex]
    Sys -->|EAGAIN| Wait[WriteAwaiter]
    SSL -->|WANT_WRITE| Wait
    Wait --> Loop
```

背压像仓库的限高门：它不是让卡车更快，而是在堆积失控之前停止继续收货。项目同时限制每连接任务数、
每连接字节、Reactor 邮箱任务/字节和单轮写预算。唯一 writerLoop 保证同一连接的字节顺序，
`OutboundReceipt` 只为需要追踪的任务记录终态。

代码入口：[`OutboundTask`](../../../server/transport/OutboundTask.h)、
[`OutboundAdmission`](../../../server/transport/OutboundAdmission.cpp)、
[`OutboundQueue`](../../../server/transport/OutboundQueue.cpp)、
[`TransportWriter`](../../../server/transport/TransportWriter.cpp)。
