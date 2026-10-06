# Phase 11：账号、社交聊天与 AI 推理平台

> 当前分支：本地 `phase11`，基线为 `test9.1` 的 `92252c3`。本阶段不向远程仓库推送。

## 1. 当前完成状态

Phase 11 已完成账号与文字聊天的第一条可运行纵向链路，并完成 PostgreSQL、libsodium、
libcurl、ONNX Runtime 与 ONNX Runtime GenAI 的真实 Linux 接入：

- 账号、Session、好友、会话、消息、Outbox、模型和训练数据领域对象；
- 数据库、事务和全部 Repository 的运行时多态接口；
- 可以回滚事务的线程安全内存 Repository；
- 内存版与 PostgreSQL 版共用的 Repository 契约测试入口；
- 注册、登录、CSRF、退出、好友申请、私聊、群聊、权限、消息幂等和历史搜索服务；
- 账号与来源地址双维度的登录失败限流；
- 独立于 Reactor、带队列上限和优雅关闭的数据库执行器；
- HTTP/WebSocket/SSE 共用的传输 DTO 和事件名称；
- 按顺序重试、至少一次投递的 Outbox Dispatcher；
- 模型 Provider 接口、模型版本注册、父子无环约束、AI 账号绑定和 Provider 路由；
- 异步 `OllamaModelProvider`、增量 NDJSON 解码、取消、超时与有界并发；
- `InProcessOnnxModelProvider`、有界推理 Worker、模型/Adapter 缓存、目录摘要与逐 Token 回调；
- `GrpcOnnxModelProvider`、独立 `webserver-onnx-worker`、gRPC server-streaming、跨进程取消和有界背压；
- `AIChatService`、`ai.generate/ai.cancel`、SSE token/完成事件与 AI 回复持久化；
- 会话成员接口返回显示名与 `aiAccount`，前端进入 AI 会话后自动选择 AI 账号；
- 明确授权、脱敏、审核、数据集版本和导出的训练数据服务；
- PostgreSQL 18.6 用户目录运行环境、第一版 Schema、libpq 连接池与完整 Repository；
- libsodium 1.0.22、Argon2id 密码散列、安全随机 Session Token 和带密钥 Token Hash；
- 内存版与 PostgreSQL 版共用的契约测试，以及 PostgreSQL 专属扩展契约；
- 注册、登录、退出、恢复会话和资料修改的真实 HTTP API；
- 好友申请、私聊、群聊、成员管理、历史和搜索的真实 HTTP API；
- Phase 11 Session 身份进入 WebSocket/SSE，客户端不能用 `uid` 冒充其他用户；
- `chat.send/chat.ack/chat.read` WebSocket 适配器和后台 Outbox Pump；
- 消息提交后向会话成员发送 WebSocket 消息与 SSE 通知；
- 原生 HTML/CSS/JavaScript 登录、好友、私聊、群聊、AI 流式生成和断线恢复界面；
- 普通 HTTP 虚拟机地址使用兼容型客户端消息 ID，避免非安全上下文缺少
  `crypto.randomUUID()` 时发送按钮在 WebSocket 提交前中断；
- 可复用的 HTTP + WebSocket + SSE + PostgreSQL + Ollama API 真实黑盒测试；
- 不联网、不自动下载依赖的 CMake 功能开关；
- 覆盖内存、PostgreSQL、密码、Ollama、进程内 ONNX 和远程 ONNX 的 Phase 11 专项测试矩阵。

当前版本已经是可实际运行的单机文字聊天、Ollama API、ONNX 进程内 Provider 和独立 ONNX 推理
进程学习版，但还不是完整生产聊天与 AI 平台。用户已在 Windows 安装 Ollama，按要求不在 Linux
重复安装。Linux 的
Phi-3 Mini 4K CPU INT4 真实模型已通过单 Provider smoke 和包含 WebSocket、SSE、PostgreSQL 的
整链黑盒，所以现在已证明“真实模型能加载、生成 Token 并持久化”。Presence 生命周期、附件/内容审核、跨实例广播、共享限流、
Outbox 多消费者租约和完整运维告警仍需后续阶段完善。PostgreSQL 只绑定虚拟机回环地址，不会与
现有 MySQL/Redis 抢端口。`TestOnlyCredentialCodec` 只用于依赖无关的内存测试，真实服务器使用
`SodiumCredentialCodec`。

### 1.1 完整架构总览

可以把整个项目看成一座分层工厂：Reactor 是只负责收发货的装卸区，协议 Session 是拆箱员，
Transport 是前台，Application Service 是业务主管，Repository 是仓库接口，PostgreSQL 和模型
Provider 才是实际干重活的后厨。最重要的规则是：**装卸区不等待数据库，也不执行模型推理**。

```mermaid
flowchart TB
    Browser[浏览器 / API 客户端]
    Ollama[Windows Ollama API]
    PG[(PostgreSQL 18.6)]
    ModelFiles[(ONNX 模型与 Adapter)]

    subgraph ChatProcess[webserver 聊天主进程]
        Acceptor[TCP accept / TLS accept]
        Reactor[Multi-Reactor + epoll + 协程]
        Protocol["HTTP/1.1 · HTTP/2 · TLS/ALPN<br/>WebSocket · SSE · gRPC"]
        Transport[Router + Phase11 HTTP/WS/SSE Adapter]
        Business["IAuthBusiness · ISocialBusiness<br/>IChatBusiness · IAiChatBusiness"]
        App["Auth · Social · Chat · AIChat<br/>ModelRegistry · TrainingData"]
        Ports[Repository / Credential / Provider Ports]
        DbExecutor["DatabaseExecutor<br/>固定 Worker + 有界队列"]
        Outbox[Outbox Pump]
        ModelRouter[ModelRouter]
        OllamaProvider[OllamaModelProvider]
        InprocProvider[InProcessOnnxModelProvider]
        RemoteProvider[GrpcOnnxModelProvider]

        Acceptor --> Reactor --> Protocol --> Transport --> Business --> App --> Ports
        App --> DbExecutor --> Ports
        App --> ModelRouter
        ModelRouter --> OllamaProvider
        ModelRouter --> InprocProvider
        ModelRouter --> RemoteProvider
        Ports --> Outbox --> Transport
    end

    subgraph InferenceProcess[webserver-onnx-worker 独立推理进程]
        GrpcService[OnnxInferenceService]
        WorkerProvider["InProcessOnnxModelProvider<br/>有界推理 Worker"]
        GrpcService --> WorkerProvider --> ModelFiles
    end

    Browser -->|HTTP 查询和管理| Acceptor
    Browser <-->|WebSocket 实时命令和消息| Protocol
    Browser <--|SSE 通知与 AI Token| Protocol
    Ports <--> PG
    OllamaProvider -->|libcurl multi HTTP| Ollama
    InprocProvider --> ModelFiles
    RemoteProvider <-->|gRPC TLS + server streaming| GrpcService
```

#### 1.1.1 七层职责

| 层 | 主要职责 | 当前实现 | 不应该承担的工作 |
|---|---|---|---|
| 网络运行时 | accept、epoll、连接上限、定时器、协程恢复和优雅停机 | `ServerRuntime`、`ReactorGroup`、`SubReactor` | SQL、密码散列、模型生成 |
| 协议 Session/Codec | 管理协议生命周期，把字节与协议对象互转 | HTTP/1.1、HTTP/2、TLS、WebSocket、SSE、gRPC Core | 好友关系和模型选择 |
| 传输适配层 | Cookie/CSRF、JSON、状态码、WS 命令、SSE 事件 | `Phase11AuthHttp`、`Phase11SocialChatHttp`、`Phase11Realtime` | 直接拼 SQL、保存业务状态 |
| 应用层 | 编排一个用例的权限、事务、幂等、取消和完成顺序 | `AuthService`、`SocialService`、`ChatService`、`AiChatService` | 依赖 HTTP fd 或 PostgreSQL 类型 |
| 领域与端口 | 定义 User、Message、ModelVersion 等规则以及抽象接口 | `Domain.h`、`Ports.h`、`ModelProvider.h` | 知道部署路径和线程池实现 |
| 基础设施适配层 | 把端口落实为数据库、密码库、HTTP 客户端或 ONNX Runtime | `PostgresStore`、`SodiumCredentialCodec`、三个 Model Provider | 决定业务权限 |
| 外部资源 | 持久化、模型执行和浏览器展示 | PostgreSQL、Ollama、ONNX/Adapter、原生 JS 页面 | 反向控制核心业务对象 |

依赖方向始终从外向内：传输层依赖应用层，应用层依赖端口，基础设施实现端口。业务层不会包含
`PGconn`、`CURL*`、`grpc::ClientContext` 或 `OgaModel`，因此存储和推理方式可以替换，而注册、聊天
和模型绑定规则不用重写。

#### 1.1.2 进程与线程模型

| 执行单元 | 做什么 | 阻塞规则与保护 |
|---|---|---|
| 主线程 | 装配对象、注册路由、启动监听、等待关闭 | 不处理具体请求 |
| SubReactor | 处理 socket readiness、协议解析、连接状态和协程恢复 | 只做短操作，不能等 SQL 或模型 |
| HTTP Worker | 执行普通或可能阻塞的 HTTP Handler | `/fast` 等明确短路由才允许 `ReactorSafe` |
| WebSocket Worker | 执行 `chat.*`、`ai.*` 命令 | 数据库调用转交 `DatabaseExecutor`；不会执行模型循环 |
| DatabaseExecutor | 执行 Repository、事务和 libpq 调用 | 固定 Worker、有界队列；满载返回忙，不无限堆积 |
| Outbox Pump | 顺序读取并发布已提交事件 | 单独后台线程；失败事件保留，下轮重试 |
| Ollama Provider | libcurl multi 驱动外部 HTTP 流 | 限制并发、排队、响应字节和总超时 |
| 进程内 ONNX Worker | 模型加载、摘要、Tokenizer、Adapter 和逐 Token 生成 | 固定 Worker、有界任务队列与模型缓存 |
| gRPC ONNX 客户端 Worker | 等待远端 `ClientReader` 流 | 独立有界队列；统一取消观察器调用 `TryCancel()` |
| 独立 ONNX 进程 | gRPC 认证、Token 缓冲和实际 OGA/ORT 推理 | 与聊天进程隔离；双重 Token/字节上限传播背压 |

协程解决的是“网络等待时把线程让给其他连接”，不能把同步 SQL 或 CPU 推理自动变成非阻塞任务。
因此架构同时使用协程和专用 Worker：前者管理大量空闲连接，后者隔离真正会阻塞的工作。

#### 1.1.3 普通消息的完整生命周期

```text
浏览器 chat.send
  → WebSocketCodec 解帧
  → WebSocketDispatcher
  → Phase11Realtime 从已认证连接取得 userId
  → IChatBusiness（协议无关业务接口）
  → DatabaseExecutor
  → ChatService 检查成员权限
  → PostgreSQL 事务
       ├─ client_message_id 幂等检查
       ├─ 分配 conversation_seq
       ├─ INSERT message
       └─ INSERT outbox_event
  → COMMIT
  → Outbox Pump 读取已提交事件
  → WebSocket chat.message + SSE message.notification
  → 成功后标记 published
```

这里的关键是 Message 和 Outbox 同事务。它像“货物入库”和“生成发货单”盖在同一张单据上：数据库
提交后即使实时连接断开，消息仍然存在；如果发布成功但标记失败，下次可能重复发送，所以客户端用
事件 ID 和会话序号去重。接收方不需要保持 WebSocket 或 SSE 在线：只要发送方仍能连接服务器并且是
会话成员，私聊与群聊消息都会正常提交；离线成员上线后从 history 按 sequence 恢复。

#### 1.1.4 AI 消息的完整生命周期

```text
浏览器 ai.generate
  → Phase11Realtime 校验登录身份和请求字段
  → AiChatService 在 DatabaseExecutor 中保存用户 Prompt
  → ModelRouter 查询 AI 账号绑定和 Active ModelVersion
  → 按 runtime 选择 Provider
       ├─ ollama        → libcurl → Windows Ollama /api/chat
       ├─ onnx-inprocess→ 本进程 ONNX Worker
       └─ onnx-grpc     → gRPC TLS → 独立 webserver-onnx-worker
  → TokenSink → Phase11AiEventSink → SSE ai.token
  → 唯一 CompletionSink
       ├─ 成功：AI 回复和模型/版本/Adapter 追踪落库
       ├─ 取消：停止生成，不伪造完整回复
       └─ 失败：返回脱敏错误，普通聊天继续
  → SSE ai.completed
```

`AiChatService` 是总导演，但不亲自推理。它只保证 Prompt 先落库、Provider 只完成一次、取消属于原
请求用户、成功回复带模型追踪。远程 Worker 崩溃相当于“后厨停工”，不会把“餐厅前台”一起带走。

#### 1.1.5 状态分别归谁管理

| 状态 | 所有者 | 为什么放这里 |
|---|---|---|
| fd、连接代次、协议解析状态 | `Connection` / 各协议 Session | fd 会复用，连接身份必须跟随生命周期 |
| 在线 WebSocket/SSE 会话 | `WebSocketSessionManager` / `SseSessionManager` | 只表示当前在线连接，不作为业务真相 |
| 用户、Session、好友、消息、游标 | PostgreSQL Repository | 重启后仍存在，支持事务和查询 |
| 未发布实时事件 | `outbox_events` | 保证消息提交和待推送记录不发生双写裂缝 |
| 在途 AI 生成与取消句柄 | `AiChatService::State` | 只在生成期间存在，关闭时统一取消并等待收尾 |
| 模型节点、版本和绑定 | `IModelRepository` / PostgreSQL | 能审计哪一个 AI 账号使用哪个版本 |
| 已加载模型和 Adapter | ONNX Provider 缓存 | 与 Runtime 对象生命周期一致，不放数据库 |
| 登录失败窗口 | `InMemoryLoginRateLimiter` | 当前单实例有效；多实例时应换共享实现 |

#### 1.1.6 背压、故障与安全边界

- **连接背压**：每个连接有出站队列上限，慢客户端不能无限吃内存；HTTP/2 另有 Stream/Connection
  flow control 和调度器。
- **任务背压**：HTTP、WebSocket、数据库、Ollama、进程内 ONNX 和 gRPC 客户端都有独立容量，
  一个队列满只拒绝对应工作，不把压力扩散成全局 OOM。
- **身份边界**：HTTP Session Cookie、WebSocket 和 SSE 都从服务器恢复的 Session 得到用户身份，
  不相信客户端提交的 `uid`；写操作还要求 CSRF 或已认证 WS 上下文。
- **数据边界**：密码用 Argon2id；数据库只保存 Session Token Hash；模型和 Adapter 只能使用配置根
  目录内的相对路径，并在启用前核对 `sha256-tree-v1`。
- **进程边界**：远程 ONNX 使用 Bearer Token；生产模式强制 TLS、至少 32 字节 Token、CA 和证书；
  gRPC/HTTP2 的帧、HPACK、流控由 gRPC Core 负责。
- **关闭边界**：先停止接收新连接和 Outbox 搬运，再取消 AI 生成、排空 Worker，最后销毁 Provider、
  数据库执行器和连接池，避免回调访问已析构对象。

#### 1.1.7 核心代码导航

| 想学习的部分 | 先读文件 |
|---|---|
| 总装配与对象生命周期 | [main.cpp](../../main.cpp) |
| 领域对象与抽象端口 | [Domain.h](../../server/phase11/domain/Domain/Domain.h)、[Ports.h](../../server/phase11/ports/Ports/Ports.h) |
| 账号与权限 | [AuthService.cpp](../../server/phase11/business/AuthService/AuthService.cpp)、[Phase11AuthHttp.cpp](../../server/phase11/transport/http/Phase11AuthHttp/Phase11AuthHttp.cpp) |
| 好友、群聊与消息事务 | [SocialService.cpp](../../server/phase11/business/SocialService/SocialService.cpp)、[ChatService.cpp](../../server/phase11/business/ChatService/ChatService.cpp) |
| WebSocket/SSE/Outbox | [Phase11Realtime.cpp](../../server/phase11/transport/realtime/Phase11Realtime/Phase11Realtime.cpp)、[OutboxDispatcher.cpp](../../server/phase11/business/OutboxDispatcher/OutboxDispatcher.cpp) |
| PostgreSQL 与连接池 | [PostgresStore.cpp](../../server/phase11/storage/postgres/PostgresStore/PostgresStore.cpp)、[PostgresConnectionPool.cpp](../../server/phase11/storage/postgres/PostgresConnectionPool/PostgresConnectionPool.cpp) |
| AI 编排和模型选择 | [AiChatService.cpp](../../server/phase11/business/AiChatService/AiChatService.cpp)、[ModelRegistryService.cpp](../../server/phase11/ai/ModelRegistryService/ModelRegistryService.cpp) |
| Ollama 流式 HTTP | [OllamaModelProvider.cpp](../../server/phase11/ai/OllamaModelProvider/OllamaModelProvider.cpp)、[OllamaStreamDecoder.cpp](../../server/phase11/ai/OllamaStreamDecoder/OllamaStreamDecoder.cpp) |
| 进程内 ONNX | [InProcessOnnxModelProvider.cpp](../../server/phase11/ai/InProcessOnnxModelProvider/InProcessOnnxModelProvider.cpp) |
| 独立 ONNX Worker | [GrpcOnnxModelProvider.cpp](../../server/phase11/ai/GrpcOnnxModelProvider/GrpcOnnxModelProvider.cpp)、[GrpcOnnxWorkerService.cpp](../../server/phase11/ai/GrpcOnnxWorker/GrpcOnnxWorkerService.cpp)、[GrpcOnnxWorkerMain.cpp](../../server/phase11/ai/GrpcOnnxWorker/GrpcOnnxWorkerMain.cpp) |
| 训练数据授权与导出 | [TrainingDataService.cpp](../../server/phase11/training/TrainingDataService/TrainingDataService.cpp) |
| 数据库 Schema | [001_phase11.sql](../../server/phase11/storage/postgres/migrations/001_phase11.sql)；[16 张表的关系与业务写入流程](../database/README.md) |
| 前端入口 | [index.html](../../frontend/phase11/index.html)、[app.js](../../frontend/phase11/app.js) |

当前最适合作为**单机学习、局域网演示和继续工程化的基线**。它已经验证关键功能和故障隔离，仍缺少
跨实例事件总线、Outbox 多消费者租约、共享限流、附件与内容审核、正式可观测平台、备份恢复演练、
真实业务数据训练评估以及独立压测机上的长时间容量验证，所以文档不会把它描述成可直接承载公网
关键业务的最终生产系统。

## 2. 为什么先写接口与内存实现

可以把 Repository 接口看成标准插座：

```text
AuthService / ChatService / ModelRegistryService
                     │
                     ↓
        IUserRepository / IMessageRepository
                     │
          ┌──────────┴──────────┐
          ↓                     ↓
  InMemoryStore          PostgresRepository
  测试用仓库               生产用仓库
```

业务服务只知道“保存用户、追加消息、开始事务”，不知道底下使用 SQL、内存还是以后新增的
MySQL。这里使用虚函数完成运行时多态；模板只适合在数据库驱动内部复用实体映射，不作为
整个业务层的公共接口。

代码入口：

- [Domain.h](../../server/phase11/domain/Domain/Domain.h)：领域对象；
- [Ports.h](../../server/phase11/ports/Ports/Ports.h)：数据库、Repository 与密码学端口；
- [InMemoryStore.h](../../server/phase11/storage/InMemoryStore/InMemoryStore.h)：无外部依赖的参考实现；
- [PostgresConnectionPool.h](../../server/phase11/storage/postgres/PostgresConnectionPool/PostgresConnectionPool.h)：
  有界等待、独占 `PGconn` 的 libpq 连接池；
- [PostgresStore.h](../../server/phase11/storage/postgres/PostgresStore/PostgresStore.h)：全部 Repository 的
  PostgreSQL 适配器；
- [RepositoryContract.h](../../tests/phase11/unit/RepositoryContract.h)：所有数据库驱动共用的契约测试；
- [phase11_postgres_contract.cpp](../../tests/phase11/unit/postgres_contract.cpp)：真实 PostgreSQL
  扩展契约与连接池并发检查；
- [001_phase11.sql](../../server/phase11/storage/postgres/migrations/001_phase11.sql)：PostgreSQL Schema。

### 2.1 Repository 契约测试

“实现了同一个 C++ 接口”只说明函数长得一样，不能证明行为一样。因此
`runRepositoryContract` 像统一驾照考试，会对每种存储实现检查：

- 未提交事务离开作用域后是否回滚；
- Session 是否能按 Token Hash 查询；
- 好友关系是否正确落库；
- 相同 `client_message_id` 是否返回同一条消息；
- 会话序号是否保持连续；
- Message 与 Outbox 是否在同一个事务语义中；
- Outbox 确认后是否不再被当成待发送事件。

内存实现和 PostgreSQL 实现都已经通过同一份契约。PostgreSQL 测试还额外检查了会话吊销、
好友待处理队列、会话列表、消息搜索、ACK/已读游标、模型树、跨节点错误版本启用、AI 绑定、
训练数据集和 12 个并发连接池健康请求。它验证的是业务语义，而不只是“数据库能连上”。

## 3. 内存事务如何工作

`InMemoryStore::Transaction` 开始时锁住仓库并复制一份状态：

```text
beginTransaction
      ↓
保存 State 快照
      ↓
业务进行多次 Repository 写入
      ↓
commit   → 丢弃快照
rollback → 用快照恢复
```

这是学习和契约测试实现，不适合保存大量生产数据。`PostgresStore::Transaction` 已把相同接口
映射为 `BEGIN / COMMIT / ROLLBACK`，并把借出的连接绑定到当前数据库 Worker，事务完成才归还。

连接池像四把数据库窗口钥匙：每个 Worker 借走一把钥匙后独占一个 `PGconn`，用完先检查事务
是否回到 idle，再把钥匙归还。钥匙借完时只等待配置的超时时间，不让请求无限挂起。

消息序号分配与幂等检查使用事务级 advisory lock，把同一会话的并发写入排成短队；不同会话
仍可并行。模型版本切换先锁定模型节点，只有目标版本属于该节点、校验和非空且未被拒绝时，
才退役旧版本并启用新版本。无效目标不会破坏当前 active 版本。

## 4. 账号与 Session

[AuthService.cpp](../../server/phase11/business/AuthService/AuthService.cpp) 已实现：

1. 用户名统一转小写并校验字符；
2. 密码交给 `ICredentialCodec`，业务层不接触 Argon2id 细节；
3. 登录生成 Session Token 和 CSRF Token；
4. Repository 只保存两个 Token 的 Hash；
5. Session 过期、吊销或用户禁用后，认证立即失败；
6. 不存在的账号也执行一次虚拟密码校验，减少账号枚举的明显时间差。
7. [InMemoryLoginRateLimiter.cpp](../../server/phase11/security/InMemoryLoginRateLimiter/InMemoryLoginRateLimiter.cpp)
   同时记录账号和来源地址失败次数；到达阈值后返回 `RateLimited`，协议层以后映射为 429。
8. Session Token 只允许进入 Secure/HttpOnly/SameSite Cookie；JSON 响应使用不含
   `passwordHash` 的 `PublicUser`，只把 CSRF Token 交给同源页面。

登录限流像两道门：账号门防止一个账号被连续猜测，来源门防止同一个地址轮流猜很多账号。
成功登录只清除账号桶，不清除来源桶，避免攻击者用一个已知账号穿插成功请求来洗掉来源记录。
当前实现只在单进程内共享；多实例部署时需要用共享数据库或 Redis 实现同一个端口。

真实实现位于 [SodiumCredentialCodec.cpp](../../server/phase11/security/SodiumCredentialCodec/SodiumCredentialCodec.cpp)：
密码使用 Argon2id 字符串格式，Session Token 由安全随机数生成，数据库只保存带服务端密钥的
Token Hash。libsodium 初始化或密钥文件权限不符合要求时，服务器会在监听端口前失败关闭。

HTTP 入口位于 [Phase11AuthHttp.cpp](../../server/phase11/transport/http/Phase11AuthHttp/Phase11AuthHttp.cpp)：

```text
POST /api/auth/register   POST /api/auth/login
POST /api/auth/logout     GET/PATCH /api/me
```

登录成功后浏览器只得到 `HttpOnly + SameSite=Strict` Cookie；JavaScript 能读取 CSRF Token，
却读不到 Session Token。服务器重启后 Session 仍可从 PostgreSQL 恢复，退出登录会立即吊销。

## 5. 聊天写入流程

```text
chat.send
   ↓
检查发送者是否属于会话
   ↓
beginTransaction
   ↓
按 (conversation, sender, clientMessageId) 去重
   ↓
分配 conversation sequence
   ↓
写 Message
   ↓
写 OutboxEvent
   ↓
commit
```

`clientMessageId` 像用户给快递贴的“本次寄件单号”。客户端超时重试时仍使用相同单号，
服务器就返回原消息，不会再写一条。如果相同单号却携带不同正文，服务返回冲突，防止错误
复用 ID 悄悄覆盖业务含义。

`conversation_seq` 是一个会话内部的连续号码，用于排序、ACK、已读游标和断线恢复。

Outbox 解决的是双写问题：数据库消息和“待推送事件”在同一事务内写入。即使 WebSocket
暂时发送失败，Dispatcher 以后仍能读取未发布 Outbox，用户重连也能从消息表恢复。

[OutboxDispatcher.cpp](../../server/phase11/business/OutboxDispatcher/OutboxDispatcher.cpp) 已实现顺序搬运：

```text
读取 pending Outbox（按 id 排序）
             ↓
       推送事件给 Sink
       ├─ 失败 → 保留事件并停止本批
       └─ 成功 → 标记 published
```

如果“推送已成功、数据库标记失败”，该事件下次会再次推送。这是至少一次投递，接收方必须用
`OutboxEvent.id` 去重。它不会谎称网络和数据库能组成一个跨系统原子事务。

[Phase11Realtime.cpp](../../server/phase11/transport/realtime/Phase11Realtime/Phase11Realtime.cpp) 把这套语义接入
真实连接：`chat.send` 先通过数据库执行器提交 Message 与 Outbox，后台 Pump 再把已提交事件
翻译成 WebSocket `chat.message` 和 SSE `message.notification`。Pump 像仓库传送带，每 250 ms
检查一次待发件；失败不盖“已发送”章，下一轮重试。零在线连接不算业务失败，因为消息已经
落库，客户端重连后会按会话 sequence 查询历史。

代码入口：

- [ChatService.cpp](../../server/phase11/business/ChatService/ChatService.cpp)：会话、权限、幂等和 Outbox；
- [OutboxDispatcher.cpp](../../server/phase11/business/OutboxDispatcher/OutboxDispatcher.cpp)：推送、确认和失败重试；
- [SocialService.cpp](../../server/phase11/business/SocialService/SocialService.cpp)：好友申请；
- [Phase11SocialChatHttp.cpp](../../server/phase11/transport/http/Phase11SocialChatHttp/Phase11SocialChatHttp.cpp)：
  好友、会话、历史和搜索 HTTP 入口；
- [Phase11Realtime.cpp](../../server/phase11/transport/realtime/Phase11Realtime/Phase11Realtime.cpp)：
  WebSocket 命令、Outbox 到 WS/SSE 的翻译和后台 Pump；
- [phase11_core.cpp](../../tests/phase11/unit/core.cpp)：完整业务流程测试。

### 5.1 为什么数据库有独立执行器

[BoundedExecutor.cpp](../../server/phase11/runtime/BoundedExecutor/BoundedExecutor.cpp) 是 Phase 11 的数据库后厨：

```text
Reactor 收到请求
      ↓ 快速投递
DatabaseExecutor（固定 Worker + 有界队列）
      ↓
Repository / PostgreSQL
      ↓
完成事件回到所属 Reactor
```

队列满时 `submit` 返回 `false`，协议适配层应返回 503 或稍后重试。Worker 会截住越过线程入口
的异常，队列在关闭时先排空再退出。数据库、普通 HTTP 业务和模型推理不会共用这条队列，
因此慢 SQL 不会直接占用 Reactor，也不会把模型生成积压传染给普通聊天。

## 6. AI Provider 和模型路由

```text
WebSocket ai.generate
              ↓
          AIChatService
              ↓
          ModelRouter
              ↓
       active ModelVersion
              ↓ runtime 字段
 ┌────────────┼─────────────────┐
 ↓            ↓                 ↓
ollama   onnx-inprocess      onnx-grpc
```

[ModelProvider.h](../../server/phase11/ai/ModelProvider/ModelProvider.h) 定义统一的生成请求、token 回调、
完成回调和取消令牌。依赖未安装时使用 `UnavailableModelProvider` 明确返回不可用，不伪造回答。

[ModelRegistryService.cpp](../../server/phase11/ai/ModelRegistryService/ModelRegistryService.cpp) 管理：

- Root/Specialist 模型节点；
- Candidate/Active/Retired/Rejected 模型版本；
- AI 账号与模型绑定；
- 父模型控制子模型的有向无环关系；
- 同级模型之间的拓扑关系；
- 激活新版本和保留旧版本以便回滚。

`ModelRouter` 根据 AI 账号绑定和 active version 选择 Provider，并把模型节点、版本和 Adapter
写入完成结果。它还把当前版本的 `modelArtifact` 和 `adapterArtifact` 放入请求，Provider 无需
反向访问 Repository。以后每条 AI 回复都能回答“由哪个模型版本生成”。

### 6.1 为什么已有 HTTP/WebSocket/SSE，还要 libcurl

项目原有协议栈是 **服务端入口**：浏览器主动连接本服务器，本服务器接收 HTTP 请求并维护
WebSocket/SSE 长连接。调用 Ollama 时连接方向反过来了——本服务器需要充当 **HTTP 客户端**，
主动连接另一个进程的 `/api/chat`。可以把前者理解成餐厅前台，libcurl 是把订单送到模型厨房、
再把每一份菜端回来的内部送单员；两者没有互相替换。

手写 HTTP Client 还需要重新解决 DNS、非阻塞 connect、HTTPS 校验、超时、取消、连接复用和
响应传输编码。这里把经过生产验证的网络细节交给 libcurl，项目自己保留模型路由、有界队列、
NDJSON 增量解析、会话权限、消息事务、SSE 推送和取消语义。浏览器侧仍完整使用本项目原有的
WebSocket 与 SSE。

### 6.2 Ollama Provider 与聊天纵向流程

```text
WebSocket Worker 收到 ai.generate
              ↓ generate() 只登记并入数据库队列
DatabaseExecutor：验证成员和 AI 账号、持久化用户问题、读取最近上下文
              ↓
ModelRouter：AI 账号 → Active ModelVersion → runtime=ollama
              ↓ generate() 只入模型队列
OllamaModelProvider 有界队列
              ↓
专用 libcurl multi Worker
              ↓ HTTP POST /api/chat
Ollama NDJSON 字节流
              ↓
OllamaStreamDecoder（恢复完整行并解 JSON 字符串）
              ↓
TokenSink / CompletionSink
       ┌──────┴────────────────────┐
       ↓                           ↓
Phase11AiEventSink             DatabaseExecutor
       ↓ ai.token                 ↓ 保存 AI 回复和模型追踪
SseSessionManager            Outbox + ai.completed
       ↓ 所属 Reactor              ↓
浏览器 EventSource            WebSocket/SSE
```

可以把 `libcurl multi` 看成一名同时照看多口锅的厨师：模型连接在等待网络数据时，厨师会去
照看其他连接，不会让每个生成请求永久占住一个 Reactor 或一条线程。`generate()` 只完成参数
校验和有界入队；DNS、连接、HTTP 传输和 NDJSON 解析都在专用 Worker 上进行。

[OllamaStreamDecoder.cpp](../../server/phase11/ai/OllamaStreamDecoder/OllamaStreamDecoder.cpp) 处理一个常被忽略的边界：
TCP/libcurl 回调的分块不等于 JSON 行边界。一行可能分三次到达，三行也可能一次到达。Decoder
先恢复换行分隔的完整对象，再提取 `message.content`、`done`、`error`，并支持 JSON 转义和
Unicode surrogate pair。单行和请求体都有大小上限，终止事件后继续发数据会被拒绝。

[OllamaModelProvider.cpp](../../server/phase11/ai/OllamaModelProvider/OllamaModelProvider.cpp) 还实现了：

- `maxConcurrent + maxQueued` 总接纳边界，满载时立即失败，避免模型积压耗尽内存；
- connect timeout、总请求 timeout、HTTP/HTTPS 协议白名单和禁止重定向；
- `CancellationToken` 通过 libcurl progress callback 中止传输；
- Provider 析构时取消排队和在途请求，等待 Worker 退出后才释放 multi handle；
- 远端 JSON `error`、非 2xx、畸形流、缺少 `done` 与回调异常分别返回明确错误；
- token 与完成回调最多执行一条终态路径。

回调在 Provider Worker 上触发，但不会直接改 Session 或 socket。`Phase11AiEventSink` 调用线程安全
的 `SseSessionManager::publish()`，由 Manager 把任务投递到连接所属 Reactor；最终回答重新进入
DatabaseExecutor，落库成功后才发 `ai.completed`。因此模型网络、数据库和 Reactor 各自在自己的
车道运行。

`AIChatService` 还负责：

- 校验发起者和目标 AI 都存在、未禁用且同属该会话；
- 用户问题先按 `requestId` 幂等落库，再把最近 64 条（最多读取 500 条）整理成 prompt；
- AI 回复使用 `ai:<requestId>` 幂等落库，并保存模型节点、版本和 Adapter 追踪；
- 单条最终回复限制 16 KiB，模型队列和数据库队列都有上限；
- `ai.cancel` 只能由原发起者取消，关闭服务时会取消并等待在途生成收尾；
- 深层异步错误也通过 `ai.completed` 返回给发起者，普通聊天不会被模型不可用拖住。

Linux 没有安装 Ollama。真实协议黑盒使用固定响应的伪服务验证 API 适配，不把它当成真实推理。
用户现有的 Windows Ollama 可作为实际后端，只需让虚拟机能访问该服务并设置
`WEB_PHASE11_OLLAMA_URL`。

代码与验证入口：

- [OllamaModelProvider.h](../../server/phase11/ai/OllamaModelProvider/OllamaModelProvider.h)：容量、超时与 URL 配置；
- [OllamaModelProvider.cpp](../../server/phase11/ai/OllamaModelProvider/OllamaModelProvider.cpp)：libcurl multi 生命周期；
- [OllamaStreamDecoder.h](../../server/phase11/ai/OllamaStreamDecoder/OllamaStreamDecoder.h)：增量事件接口；
- [phase11_ollama_provider.cpp](../../tests/phase11/unit/ollama_provider.cpp)：本地伪服务端到端测试。
- [AiChatService.cpp](../../server/phase11/business/AiChatService/AiChatService.cpp)：权限、上下文、取消和落库编排；
- [Phase11Realtime.cpp](../../server/phase11/transport/realtime/Phase11Realtime/Phase11Realtime.cpp)：WS 命令和 SSE 事件适配；
- [phase11_ai_chat.cpp](../../tests/phase11/unit/ai_chat.cpp)：完整业务链路、失败和取消测试；
- [phase11_runtime_blackbox.py](../../tests/phase11/integration/runtime_blackbox.py)：真实 socket 纵向黑盒。

### 6.3 ONNX 进程内 Provider

Ollama 路径像“打电话给外部模型厨房”，ONNX 进程内路径则像“在服务器后厨放一台模型机器”。
两者共用 `IModelProvider`、`ModelRouter`、取消令牌和 token/完成回调，差别只在模型实际运行的位置：

```text
WebSocket ai.generate
        ↓
AIChatService（权限、上下文、幂等问题落库）
        ↓
ModelRouter（AI 账号 → active version → runtime=onnx-inprocess）
        ↓
InProcessOnnxModelProvider::generate（校验参数并有界入队）
        ↓
独立 ONNX Worker
        ↓
相对路径限制 + sha256-tree-v1 校验
        ↓
模型缓存 → Chat Template → Tokenizer
        ↓
可选 .onnx_adapter → OgaGenerator
        ↓ GenerateNextToken
TokenSink → SSE ai.token
        ↓
CompletionSink → AI 回复落库 → ai.completed
```

`generate()` 不加载模型，也不在 Reactor 上执行推理。它只像前台发号码牌一样，把任务放入固定
Worker 数量和固定队列长度的执行器；队列满时立即返回失败。模型加载、文件摘要、Tokenizer 和
每次 `GenerateNextToken()` 都在 Worker 中执行，因此慢模型不会占住 socket 所属 Reactor。

模型目录不是数据库给出什么路径就读什么路径：

- `modelArtifact` 必须是 `WEB_PHASE11_ONNX_MODEL_ROOT` 下的相对目录；
- 规范化后的目录必须仍在模型根目录内，并包含 `genai_config.json`；
- 模型树拒绝符号链接和特殊文件，避免摘要时跳到根目录外；
- `ModelRouter` 把 active version 的 `checksum` 放入生成请求；Provider 加载前重新计算
  `sha256-tree-v1`，不匹配就拒绝；
- `adapterArtifact` 只能指向当前模型目录内的普通文件。

`sha256-tree-v1` 不只哈希权重内容，还按顺序写入相对路径长度、相对路径和文件大小。这样交换
两个同大小文件的名字也会改变摘要。部署模型前使用：

```bash
python3 scripts/phase11/model_checksum.py /home/pikachu/phase11-models/<模型目录>
```

把输出完整写入 `model_versions.checksum`。Python 工具与 C++ Runtime 已用固定二进制夹具交叉
验证；文本文件的 LF/CRLF 不同会得到不同摘要，因为模型校验比较的是实际字节。

缓存按“模型规范路径 + 校验和”区分版本。达到模型数量上限时，只淘汰当前没有生成任务持有的
最久未使用模型；所有模型都在使用时返回“缓存已满”，不会释放正在推理的对象。每个模型的
Adapter 数也有独立上限。关闭 Provider 时先取消所有任务、排空 Worker，再销毁 Adapter、
Tokenizer 和 Model，最后才允许全局 GenAI Runtime 退出。

当前已真实加载 ONNX Runtime GenAI 0.17.0 与 ONNX Runtime 1.28.0 动态库，同时验证损坏模型的
失败路径、Phi-3 Mini 4K CPU INT4 的普通生成，以及 Qwen2.5-0.5B INT4 的真实 Adapter 加载和
Token 路径。这证明当前固定版本组合可用，仍不代表任意 ONNX/LoRA 都兼容 GenAI，也不是真实
模型并发吞吐或生成质量基准。

代码与验证入口：

- [InProcessOnnxModelProvider.h](../../server/phase11/ai/InProcessOnnxModelProvider/InProcessOnnxModelProvider.h)：容量和模型根配置；
- [InProcessOnnxModelProvider.cpp](../../server/phase11/ai/InProcessOnnxModelProvider/InProcessOnnxModelProvider.cpp)：
  路径、摘要、缓存、Tokenizer、Adapter、生成循环和关闭顺序；
- [model_checksum.py](../../scripts/phase11/model_checksum.py)：部署前模型树摘要工具；
- [phase11_onnx_provider.cpp](../../tests/phase11/unit/onnx_provider.cpp)：动态库、摘要、取消、路径逃逸、
  损坏模型和 Sanitizer 测试；
- [phase11_onnx_model_smoke.cpp](../../tests/phase11/smoke/onnx_model_smoke.cpp)：真实模型获批后显式运行的
  逐 Token C++ smoke 工具；它先执行启用前试装，也可显式传入 `.onnx_adapter`，然后才生成 Token。
  它参与三种构建，但在没有模型时不伪装成自动 CTest。
- [olive-oga017-compat.py](../../scripts/phase11/olive-oga017-compat.py)：只在进程内桥接 Olive 0.13 与
  OGA 0.17 已移动的模块和导出入口，不修改 `site-packages`；
- [create-learning-lora-adapter.py](../../scripts/phase11/create-learning-lora-adapter.py)：生成固定种子、
  非零但未训练的 PEFT 教学 Adapter；
- [create-zero-lora-reference.py](../../scripts/phase11/create-zero-lora-reference.py) 与
  [compare-onnx-adapters.py](../../scripts/phase11/compare-onnx-adapters.py)：在同一张图上隔离并测量
  Adapter 对 logits 的真实影响。

### 6.4 独立 gRPC ONNX 推理进程

进程内 Provider 像“把厨房设在餐厅里”：调用短、部署简单，但模型崩溃或内存峰值会直接影响聊天
主进程。独立模式像“把厨房搬到隔壁楼”：聊天进程只下单和接收菜品，模型由另一个进程加载。

```text
浏览器 WebSocket ai.generate
        ↓
聊天进程 AIChatService → ModelRouter(runtime=onnx-grpc)
        ↓
GrpcOnnxModelProvider（有界客户端 Worker，不占 Reactor）
        ↓  gRPC Generate server-streaming / Validate unary
webserver-onnx-worker
        ↓
InProcessOnnxModelProvider → OGA/ORT → 模型与 Adapter
        ↓
Token 流 → gRPC/HTTP2 背压 → SSE ai.token
        ↓
完成/失败 → AI 消息落库；普通聊天继续运行
```

这里没有重新实现 gRPC 的 HTTP/2、HPACK 或流控；这些由已接入并经过测试的 gRPC Core 管理。项目
新增的是 AI 领域协议和两端适配：

- `Generate` 使用 server-streaming，逐条发送 Token 和唯一终态；`Validate` 在模型版本切换前让
  Worker 真实试装模型/Adapter；
- 聊天进程中的阻塞 `ClientReader` 只运行在固定数量的 `BoundedExecutor` Worker 上；统一取消观察器
  调用 `ClientContext::TryCancel()`，不会为每个请求新建取消线程；
- Worker 端 Token 队列同时限制条数和总字节。客户端读取变慢时，队列填满会暂停模型生成，把背压
  沿 gRPC/HTTP2 传回 OGA，而不是无限堆积内存；
- Bearer Token 使用常量时间比较。生产模式要求至少 32 字节 Token、服务端 TLS 证书/私钥和客户端
  CA；本机学习模式可以在回环地址使用明文；
- Worker 断开只让当前生成得到脱敏错误，例如 `remote ONNX RPC failed (gRPC code 14)`。聊天进程、
  普通 WebSocket 消息、SSE、PostgreSQL 和 Outbox 都继续工作。

代码与验证入口：

- [web_learning.proto](../../server/grpc/proto/web_learning.proto)：远程生成、Token/终态和版本校验消息；
- [GrpcOnnxModelProvider.cpp](../../server/phase11/ai/GrpcOnnxModelProvider/GrpcOnnxModelProvider.cpp)：聊天进程客户端、
  有界排队、deadline、取消和错误脱敏；
- [GrpcOnnxWorkerService.cpp](../../server/phase11/ai/GrpcOnnxWorker/GrpcOnnxWorkerService.cpp)：认证、请求转换、
  有界 Token 队列和 server-streaming；
- [GrpcOnnxWorkerMain.cpp](../../server/phase11/ai/GrpcOnnxWorker/GrpcOnnxWorkerMain.cpp)：独立进程启动、TLS、信号和
  限制项；
- [phase11_grpc_onnx_provider.cpp](../../tests/phase11/unit/grpc_onnx_provider.cpp)：真实 gRPC socket 上的
  Token 顺序、Validate、错误 Token 和取消测试。

### 6.5 Adapter 启用、切换与回滚

`.onnx_adapter` 可以类比为一张可更换的“技能卡”，基础模型则是读卡器。文件存在并不等于能够
使用：技能卡的格式、参数名和形状都必须与读卡器预留的 Adapter 输入一致。因此 Phase 11 不再
把“数据库里写了一个文件名”当作可部署证明，而是采用下面的顺序：

```text
Candidate ModelVersion
        ↓
根据 runtime 查找 IModelArtifactValidator
        ↓
规范化模型和 Adapter 相对路径
        ↓
验证 sha256-tree-v1
        ↓
OgaModel::Create
        ↓
可选 OgaAdapters::LoadAdapter（解析 + 与基础模型试装）
        ├── 失败：Candidate 保持原状，旧 Active 继续服务
        └── 成功：Repository 在锁/事务内移动 Active 指针
                         ↓
                    旧版本 Retired
```

这里刻意把“体检”和“换岗”分成两段。`IModelArtifactValidator` 是体检接口，ONNX 实现直接复用
真实的 Runtime 和模型缓存，不写一套容易与生产行为分叉的假解析器；`IModelRepository` 只负责
最后一次很短的状态切换。体检可能首次加载数 GB 模型，所以管理接口必须把它放进专用 Worker，
不能在 epoll Reactor 中直接运行。

回滚不是简单改一列状态。`rollback(node, retiredVersion)` 只接受同节点的 `Retired` 版本，并会
重新执行路径、摘要、基础模型和 Adapter 兼容性检查。这样旧文件已经损坏或被替换时，回滚会
失败并保持当前 Active，不会把历史上“曾经可用”误当成“现在仍可用”。

Adapter 接入时在三套 **90/90** 基线上新增了真实证据；独立 gRPC Worker 接入后的最终全量结果已
增长到 **92/92**。Qwen Adapter-ready 图有 818 条输入
声明、434 个唯一名称；其中 384 个唯一名称属于 LoRA 权重或量化 scale。Olive 为这 384 个输入各
生成了两条声明，OGA Runtime 能加载，但生产导出器升级时还应继续检查这一重复现象。独立
`adapter_weights.onnx_adapter` 为 1,241,136 字节。
项目 Release C++ Provider 已通过 `LoadAdapter → SetActiveAdapter → GenerateNextToken`。同一图、同一
Prompt 下，零 Adapter 与非零教学 Adapter 的 151,936 个下一 Token logits 全部发生变化，最大绝对
差为 `1.047957420349121`，因此不是把随机采样误当成 Adapter 生效。

注册阶段还会阻止两类明显配置错误：

- Ollama 版本不能填写 `.onnx_adapter`；Ollama 的模型标签本身代表服务端已经组装好的模型；
- ONNX Adapter 必须是模型目录内的相对路径，并以 `.onnx_adapter` 结尾；最终的路径逃逸、
  普通文件和兼容性检查由 ONNX Validator 完成。

代码索引：

- [ModelProvider.h](../../server/phase11/ai/ModelProvider/ModelProvider.h)：`IModelArtifactValidator` 与校验结果；
- [ModelRegistryService.cpp](../../server/phase11/ai/ModelRegistryService/ModelRegistryService.cpp)：注册约束、先校验后切换、回滚；
- [InProcessOnnxModelProvider.cpp](../../server/phase11/ai/InProcessOnnxModelProvider/InProcessOnnxModelProvider.cpp)：真实 OGA 试装；
- [phase11_core.cpp](../../tests/phase11/unit/core.cpp)：不兼容版本不替换旧版本、成功切换与回滚；
- [phase11_onnx_provider.cpp](../../tests/phase11/unit/onnx_provider.cpp)：损坏模型试装失败且不泄露根路径。

当前边界也必须说清楚：Phi-3 CPU INT4 仍是普通生成模型，它的图中没有 Adapter 输入；只有本轮
重新导出的 Qwen 图能使用与其参数名、形状和 INT4 格式匹配的 Adapter。教学 Adapter 是固定随机
权重，用来验证工程机制，没有经过数据训练，不能宣称获得了新知识或更好的回答质量。

官方参考：

- [ONNX Runtime GenAI Adapter 格式](https://onnxruntime.ai/docs/genai/reference/adapter.html)
- [ONNX Runtime GenAI LoRA 教程](https://onnxruntime.ai/docs/genai/tutorials/finetune.html)
- [Olive PEFT Adapter 流程](https://microsoft.github.io/Olive/features/peft-adapters.html)

## 7. 训练数据为什么需要单独流程

聊天记录不能直接等同于训练数据。Phase 11 的流程是：

```text
用户明确授权
   ↓
确认用户有权访问来源消息
   ↓
提交已脱敏 prompt/response
   ↓
审核
   ↓
不可变 DatasetVersion
   ↓
Exporter
   ↓
 LoRA/Olive/PEFT（工程转换链已验证；有质量的训练仍需数据与 GPU 的独立审批）
```

已经导出的数据集保持不可变；用户撤销授权时记录 `Revoked`，后续训练通过新数据集版本排除，
而不是偷偷修改旧数据集，确保数据血缘可以审计。

代码入口：[TrainingDataService.cpp](../../server/phase11/training/TrainingDataService/TrainingDataService.cpp)。

## 8. 前端应用与协议闭环

[frontend/phase11](../../frontend/phase11/) 包含：

- 自适应登录页、会话侧栏、消息面板、空状态、加载状态和 Toast 反馈；
- HTTP `fetch` 封装和 CSRF 首部；
- 页面刷新后通过 `/api/me` 和 HttpOnly Cookie 恢复会话；
- WebSocket `chat.send/chat.ack/chat.read`、断线重连和按消息 ID 去重；
- EventSource 处理 `message.notification`、`conversation.invited`、`friend.request`、
  `ai.token` 和 `ai.completed`；
- AI 模式、逐 Token 显示、生成取消与失败状态；
- 好友列表、申请发送/接受/拒绝，以及从好友直接创建私聊；
- 私聊、群聊、群成员邀请、服务端消息搜索；
- 个人资料查看与修改；
- 桌面和移动布局，以及 WebSocket/SSE 两条连接的独立状态。
- 参考产品稿重做的三栏桌面工作台：左侧账号与会话、中间消息流、右侧成员/连接/离线投递详情；
- 真实成员查询 `GET /api/conversations/{id}/members`，群成员管理按钮只对创建者显示；
- 会话名称筛选、私聊/群聊/含 AI 分类、独立通道状态弹窗和移动端列表/会话切换。

浏览器端不是另一套业务系统。它像“总服务台”，把不同操作交给正确通道：账号、好友、历史和
搜索走短请求 HTTP；实时聊天走双向 WebSocket；邀请、好友申请和 AI Token 走单向 SSE。消息仍然
先由服务端事务写入 PostgreSQL 与 Outbox，再实时推送，页面刷新后能从持久化历史恢复。

```text
Browser
├─ HTTP fetch + CSRF ─────────→ Auth / Social / History / Search
├─ WebSocket ─────────────────→ chat.send / ack / read / AI control
└─ EventSource ───────────────→ notification / invitation / AI token
                                      ↓
                         Business Interfaces
                                      ↓
                              Application Service
                                      ↓
                              PostgreSQL + Outbox
```

服务器已注册 `/phase11` 及其静态资源路由。只有同时配置数据库连接和 Token 密钥文件时，
Phase 11 API 才启用；否则页面会准确显示 HTTP 错误，不会降级成不安全的演示登录。
CMake 的 `phase11_frontend_assets` 目标会把前端同步到构建目录，避免从 build 目录启动时找不到
CSS/JavaScript。Phase 11 的 HTML、CSS 和 JavaScript 路由显式使用 `Cache-Control: no-store`；
底层 `HttpResponse::sendfile` 只在路由没有指定缓存策略时补 `public,max-age=3600`，不会再覆盖
业务路由。入口脚本还带版本参数，避免已打开的旧页面继续运行修复前的 ES Module。
仓库完整分类与文件移动说明见 [project-layout.md](project-layout.md)。

[Contracts.h](../../server/phase11/transport/Contracts/Contracts.h) 固定了 `chat.send`、`chat.ack`、
`chat.read`、Presence 与六种 SSE 通知的 DTO/事件名。它不负责解析 JSON：未来协议适配器只做
“HTTP/WS/SSE 包裹 ↔ DTO”的转换，业务服务不会直接依赖 `HttpRequest` 或 WebSocket 帧。

[BusinessInterfaces.h](../../server/phase11/business/BusinessInterfaces/BusinessInterfaces.h) 是新增的业务门面边界。
`Phase11AuthHttp`、`Phase11SocialChatHttp` 和 `Phase11Realtime` 只依赖 `IAuthBusiness`、
`ISocialBusiness`、`IChatBusiness`、`IAiChatBusiness`；当前实现位于 `business/<Service>/` 下的
四个 Service。可以把它理解成“前台只认统一业务窗口，窗口后面才决定由哪个部门办理”，因此传输协议不会
直接操作 Repository，也不会把相同好友、权限和消息规则在 HTTP/WS 中各写一遍。

## 9. CMake 依赖开关

```text
WEBSERVER_ENABLE_PHASE11      默认 ON，只有标准 C++ 依赖
WEBSERVER_ENABLE_POSTGRES     默认 OFF
WEBSERVER_ENABLE_SODIUM       默认 OFF
WEBSERVER_ENABLE_CURL_CLIENT  默认 OFF
WEBSERVER_ENABLE_ONNX_GENAI   默认 OFF
WEBSERVER_ENABLE_GRPC         默认 OFF；与 ONNX 同时启用时生成独立推理进程
```

四个外部适配器不会自动下载。libsodium、libcurl 与 ONNX 要求显式提供用户批准的安装前缀；找不到时
CMake 立即说明缺失项和配置变量。独立 ONNX 进程复用项目已有的 Protobuf、gRPC、OpenSSL 和 ONNX
安装，不增加新的下载或审批项。

Linux 虚拟机上的完整已批准依赖构建示例：

```bash
cmake -S . -B build-postgres \
  -DWEBSERVER_ENABLE_PHASE11=ON \
  -DWEBSERVER_ENABLE_POSTGRES=ON \
  -DWEBSERVER_POSTGRES_ROOT=/home/pikachu/phase11-deps/postgresql/18.6 \
  -DWEBSERVER_ENABLE_SODIUM=ON \
  -DWEBSERVER_SODIUM_ROOT=/home/pikachu/phase11-deps/libsodium/1.0.22 \
  -DWEBSERVER_ENABLE_CURL_CLIENT=ON \
  -DWEBSERVER_CURL_ROOT=/home/pikachu/phase11-deps/curl/8.22.0 \
  -DWEBSERVER_ENABLE_GRPC=ON \
  -DWEBSERVER_ENABLE_ONNX_GENAI=ON \
  -DWEBSERVER_ONNX_RUNTIME_ROOT=/home/pikachu/phase11-deps/onnxruntime/1.28.0 \
  -DWEBSERVER_ONNX_GENAI_ROOT=/home/pikachu/phase11-deps/onnxruntime-genai/0.17.0
```

五个 `*_ROOT` 只告诉 CMake 去哪里找已经安装好的头文件和库，不会下载依赖或改全局 `PATH`。
ONNX Runtime 版本文件不是 1.28.0 时配置会失败，避免把没有验证过的核心 Runtime 与 GenAI
0.17.0 静默混用。

### 9.1 启动真实 Phase 11 服务

迁移完成后，运行时通过环境变量注入连接串和 Token Hash 密钥，仓库中不保存口令或密钥：

```bash
export PGPASSFILE=/home/pikachu/phase11-data/secrets/phase11.pgpass
export WEB_PHASE11_DATABASE='host=127.0.0.1 port=5432 dbname=phase11 user=phase11_app connect_timeout=3'
export WEB_PHASE11_TOKEN_KEY_FILE=/home/pikachu/phase11-data/secrets/phase11-token-hash.key
export WEB_PHASE11_OLLAMA_URL='http://<Windows主机可达IP>:11434'
# 使用进程内 ONNX 时才设置；目录必须已经由用户单独批准并准备好。
export WEB_PHASE11_ONNX_MODEL_ROOT=/home/pikachu/phase11-models
export WEB_SERVER_PORT=18082
./build/webserver
```

当前虚拟机可直接使用用户态启停脚本。它会保留数据库、密钥、端口与进程内 ONNX 所需变量，
避免手工重启时漏掉 `WEB_PHASE11_ONNX_MODEL_ROOT`，从而出现“聊天正常但 AI 没有 Provider”的情况：

```bash
cd /home/pikachu/phase11-full-src
chmod +x scripts/phase11/webserver-user-service.sh
scripts/phase11/webserver-user-service.sh start
scripts/phase11/webserver-user-service.sh status
# 查看日志
scripts/phase11/webserver-user-service.sh logs
```

密钥文件在 Linux 必须由当前用户持有且不能向 group/other 开放写读权限。两个 Phase 11 变量要么
同时设置，要么同时省略；只设置一个会在监听前失败。启动后浏览器访问 `/phase11`。

`WEB_PHASE11_OLLAMA_URL` 省略时不会偷偷连接 localhost；`WEB_PHASE11_ONNX_MODEL_ROOT` 省略时也
不会扫描或加载任何模型。Provider 可以同时启用，`ModelRouter` 依据 active version 的 `runtime`
选择 `ollama`、`onnx-inprocess` 或 `onnx-grpc`。所有 Provider 都未就绪时，普通聊天照常工作，AI
请求准确返回 Provider 不可用。

连接 Windows Ollama 时，数据库 `model_versions.model_artifact` 必须与 Windows 上的模型标签一致；
Windows Ollama 还必须显式监听一个虚拟机可达的接口，并只在可信私有网络中开放对应防火墙规则。
这属于 Windows Ollama 的运行配置，不需要在 Linux 安装第二份 Ollama。

可选的 Provider 边界可通过环境变量调整：

| 变量 | 默认值 | 含义 |
|---|---:|---|
| `WEB_PHASE11_OLLAMA_CONCURRENT` | 4 | 同时在途的模型 HTTP 请求 |
| `WEB_PHASE11_OLLAMA_QUEUE` | 64 | 等待模型的最大请求数 |
| `WEB_PHASE11_OLLAMA_CONNECT_MS` | 2000 | 建连超时 |
| `WEB_PHASE11_OLLAMA_REQUEST_MS` | 120000 | 单次生成总超时 |
| `WEB_PHASE11_ONNX_WORKERS` | 1 | 进程内推理 Worker 数；CPU 模型先从 1 开始测量 |
| `WEB_PHASE11_ONNX_QUEUE` | 8 | 等待推理的最大任务数 |
| `WEB_PHASE11_ONNX_MODELS` | 2 | 同时缓存的模型版本数 |
| `WEB_PHASE11_ONNX_ADAPTERS` | 8 | 每个模型缓存的 Adapter 数 |
| `WEB_PHASE11_ONNX_PROMPT_BYTES` | 1048576 | 应用 Chat Template 前后的 Prompt 字节上限 |
| `WEB_PHASE11_ONNX_GRPC_TARGET` | 未设置 | 聊天进程连接的 Worker 地址；设置后注册 `onnx-grpc` |
| `WEB_PHASE11_ONNX_RPC_TOKEN` | 空 | 两个进程共用的内部 Bearer Token；生产模式至少 32 字节 |
| `WEB_PHASE11_ONNX_GRPC_TLS_CA` | 空 | 聊天进程信任的 PEM CA 文件；生产模式必需 |
| `WEB_PHASE11_ONNX_GRPC_TLS_NAME` | 空 | 可选的 TLS 服务名覆盖 |
| `WEB_PHASE11_ONNX_GRPC_WORKERS` | 2 | 等待远程流的固定客户端 Worker 数 |
| `WEB_PHASE11_ONNX_GRPC_QUEUE` | 32 | 客户端等待 RPC 的最大任务数 |
| `WEB_PHASE11_ONNX_GRPC_STREAM_BYTES` | 1048576 | 单次远程 Token 流累计字节上限 |
| `WEB_PHASE11_ONNX_GRPC_RPC_MS` | 300000 | Generate 总 deadline |
| `WEB_PHASE11_ONNX_GRPC_VALIDATE_MS` | 120000 | Validate deadline |

### 9.2 启动独立 ONNX Worker

本机学习模式在两个终端使用同一个只读 Token 文件。Worker 独占模型目录，聊天进程不再设置
`WEB_PHASE11_ONNX_MODEL_ROOT`：

```bash
# 终端 A：模型进程
read -r WEB_PHASE11_ONNX_RPC_TOKEN < /home/pikachu/phase11-data/secrets/phase11-onnx-rpc.token
export WEB_PHASE11_ONNX_RPC_TOKEN
export WEB_PHASE11_ONNX_MODEL_ROOT=/home/pikachu/phase11-models
export WEB_PHASE11_ONNX_WORKER_ADDRESS=127.0.0.1:50061
./build/webserver-onnx-worker

# 终端 B：聊天进程（保留 9.1 节的数据库与 Token Hash 配置）
read -r WEB_PHASE11_ONNX_RPC_TOKEN < /home/pikachu/phase11-data/secrets/phase11-onnx-rpc.token
export WEB_PHASE11_ONNX_RPC_TOKEN
export WEB_PHASE11_ONNX_GRPC_TARGET=127.0.0.1:50061
./build/webserver
```

Worker 侧可用 `WEB_PHASE11_ONNX_RPC_BUFFER_TOKENS`（默认 256）和
`WEB_PHASE11_ONNX_RPC_BUFFER_BYTES`（默认 256 KiB）限制单次流缓冲，并用
`WEB_PHASE11_ONNX_RPC_RECEIVE_BYTES/WEB_PHASE11_ONNX_RPC_SEND_BYTES`（默认各 1 MiB）限制 gRPC
消息。生产模式设置 `WEB_PRODUCTION_MODE=1` 后，Worker 还要求
`WEB_PHASE11_ONNX_WORKER_TLS_CERT/WEB_PHASE11_ONNX_WORKER_TLS_KEY`；聊天进程要求
`WEB_PHASE11_ONNX_GRPC_TLS_CA`。跨主机部署时还应限制网络入口、轮换 Token，并由进程管理器负责
重启和健康检查。

### 9.3 已接入的 API

| 方向 | 接口 |
|---|---|
| 账号 | `POST /api/auth/register`、`POST /api/auth/login`、`POST /api/auth/logout` |
| 资料 | `GET /api/me`、`PATCH /api/me` |
| 好友 | `GET/POST /api/friends`、`GET/POST /api/friend-requests`、`POST /api/friend-requests/{id}` |
| 会话 | `GET/POST /api/conversations`、`POST /api/conversations/{id}/members` |
| 消息 | `GET /api/conversations/{id}/messages`、`GET /api/search/messages` |
| 实时聊天 | WebSocket `chat.send/chat.ack/chat.read`，SSE `message.notification` 等通知 |
| AI | WebSocket `ai.generate/ai.cancel`，SSE `ai.token/ai.completed` |

### 9.4 页面中使用 AI 模式

1. 登录后点击“新建私聊”，把可用 AI 账号的用户 ID 作为对方账号；当前虚拟机中进程内 ONNX
   学习账号为 `1`（Phi-3）和 `9`（Qwen Adapter）。
2. 打开刚创建的会话。`GET /api/conversations/{id}/members` 会返回成员显示名和
   `aiAccount`，页面会自动填入 AI 账号，不需要再死记或重复输入。
3. 打开输入框旁的“AI 模式”，输入问题并发送。WebSocket 只负责提交 `ai.generate`，模型 token
   通过 SSE 的 `ai.token` 连续到达，终态通过 `ai.completed` 返回，最终答案再作为普通消息持久化。
4. 首次调用要加载模型，CPU 环境可能等待几十秒；后续请求通常更快。生成期间可点“停止生成”。

当前账号 `6` 绑定 Ollama，但 Windows Ollama 只监听回环地址时 Linux 无法访问；账号 `16` 绑定
独立 gRPC Worker，Worker 未启动时也不可用。页面收到 Provider 错误后会直接显示 SSE 返回的原因。

2026-10-06 在虚拟机端到端验证：WebSocket 接收生成请求、进程内 ONNX 生成 242 个 SSE token、
`ai.completed.success=true`，提示词与 1104 字节回复均写入 PostgreSQL。测试脚本同时消费
WebSocket 心跳和 SSE，避免长时间模型加载期间把测试连接误判为失效。

当前 `FlatJson` 是教学阶段的小型、严格、扁平对象解析器，不是通用 JSON 库。因此请求里的 ID
使用 JSON 字符串，群成员暂用英文逗号分隔字符串。它减少第三方依赖，代价是不能表达嵌套对象
和真正数组；进入生产 API 前应换成经过充分测试的 JSON 库并保持 DTO/业务接口不变。

## 10. 当前验证

Windows 本地完成 JavaScript 语法检查、Python 黑盒脚本编译检查和 `git diff --check`。完整的
Linux 验证在 CentOS Stream 9 虚拟机中完成：

- PostgreSQL 18.6 官方源码 SHA-256 校验与官方核心回归 **231/231**；
- libsodium 1.0.22 安装在用户目录，官方测试 **101/101**；
- curl/libcurl 8.22.0 官方源码 SHA-256 与 OpenPGP 签名验证通过；
- curl 官方 unit test：适用项 **70/70**，9 项按未启用能力正常跳过；
- ONNX Runtime 1.28.0 与 ONNX Runtime GenAI 0.17.0 官方 Linux x64 CPU 归档 SHA-256 通过，
  解压后的动态库由目标 RPATH 精确定位；
- GCC 11.5、C++20、`-Wall -Wextra -Wpedantic -Werror` 全量 Debug 构建通过；
- 2026-10-05 参考 UI、业务接口层和离线消息测试接入后，Debug CTest **92/92**，30.10 秒，
  覆盖旧 HTTP/TLS/WebSocket/SSE/HTTP2、Phase 11 与远程 ONNX；
- 同一版本 Release 全量 CTest **92/92**，30.23 秒；
- 同一版本 ASan+LSan+UBSan 全量 CTest **92/92**，36.34 秒，无 Sanitizer 报告；
- PostgreSQL 契约测试连接独立的 `phase11_test` 数据库，Release、Debug 和 Sanitizer 三套构建均
  实际执行该测试，不复用开发数据库；
- 新增独立推理进程后，再用完整 gRPC+ONNX Release 二进制按 test9.1 相同的 2 Reactor、
  `wrk -t2 -c64 -d5s` 配置连续执行 5 轮；QPS 为 54609.73、56902.50、56929.50、56963.47、
  57038.60，中位数 **56929.50**，相对 test9.1 的 49459.19 提升 **15.10%**，错误数为 0；
- 性能原始输出和机器可读摘要保留在虚拟机
  `/home/pikachu/phase11-benchmark/20261004T145758Z-grpc-onnx`；这是一组短时同机回归门槛，不能替代
  独立压测机上的长稳态容量测试；
- Ollama Provider 测试通过真实 loopback HTTP 连接验证分片 token、请求转义、远端错误、取消、
  队列满、非法协议、请求/响应上限和缺少终止事件；
- ONNX Provider 专项在三种构建中均通过，真实加载两个 `.so`，验证 C++/Python 固定摘要、
  取消、路径逃逸拒绝、损坏模型异步失败、错误脱敏和关闭生命周期；
- 真实模型 smoke 可执行程序在 Debug、Release、Sanitizer 三种配置中编译通过；Release 版
  实际加载 Phi-3 Mini 4K CPU INT4，按请求生成 `Phase 11 onnx ready.` 并成功终止；
- [phase11_runtime_blackbox.py](../../tests/phase11/integration/runtime_blackbox.py) 通过真实 socket
  验证注册登录、好友、私聊、Cookie 身份、WebSocket 广播、SSE 通知、ACK、幂等与历史；
- 同一黑盒在未安装 Ollama 的 Linux 上连接固定响应伪服务，完整验证
  `ai.generate → PostgreSQL → ModelRouter → libcurl /api/chat → SSE token → AI 消息落库 → Outbox`，
  收到 2 个 token、成功终态以及用户/AI 两条持久化消息；
- 黑盒绑定 `runtime=onnx-inprocess` 的真实 AI 账号后，完整验证
  `ai.generate → PostgreSQL → ModelRouter → ONNX Worker → SSE → AI 消息落库 → Outbox`；
  实际接收 242 个 token、1104 字节，并验证长推理期间的 WebSocket Ping/Pong；
- 2026-10-06 从 `http://192.168.239.135:18090` 的真实浏览器页面点击发送完成回归：该普通
  HTTP 环境同时缺少 `crypto.randomUUID` 和 `crypto.getRandomValues`，时间戳随机兜底成功生成
  幂等 ID；普通离线消息保存为会话 26 的序号 2，AI Prompt/回复保存为会话 27 的序号 3/4，
  WebSocket 与 SSE 均保持在线，浏览器控制台无错误；
- 黑盒进一步绑定 Qwen Adapter 版本后重复相同纵向链路：接收 512 个 token、3315 字节；数据库中的
  AI 回复记录了实际 `model_node_id`、`model_version_id` 和 `adapter_weights.onnx_adapter`，相关
  Outbox 事件待发布数为 0；测试服务器推理后 RSS 约 714 MiB；
- 独立 Worker 真实纵向链路使用同一 Qwen Adapter：聊天进程只注册 `onnx-grpc`，通过
  `127.0.0.1:50061` 的 server-streaming 收到 512 个 token、2424 字节并完成持久化；停止 Worker 后
  再运行黑盒，AI 请求得到受控 gRPC code 14 错误、只保存用户 Prompt，注册登录、好友、普通聊天、
  ACK、SSE 和历史查询仍全部通过；
- TLS 纵向链路也使用真实模型通过：Worker 在 `WEB_PRODUCTION_MODE=1` 下用临时证书监听 50062，
  聊天进程通过 CA 和 `phase11-worker.test` 服务名校验建立 gRPC TLS 流，收到 241 个 token、1334
  字节并完成持久化；日志保存在
  `/home/pikachu/phase11-validation/20261004T150729Z-onnx-grpc-tls`，临时私钥在测试后删除；
- 服务器重启后原 Session、CSRF Token 和资料仍可恢复，退出后 Session 立即失效；
- 共享虚拟机的 nginx 与现有服务未被停止，新增真实模型测试使用独立的
  18083–18087 端口，结束后优雅停止；独立 Worker 也已收到 SIGTERM 并完成 drain；
- 测得真实模型整链运行时服务器 RSS 约 3.56 GiB。这是 6 GiB 虚拟机上的一次功能观测，
  不是并发吞吐、延迟或生成质量基准。

## 11. 真实 ONNX 模型与 Adapter 工具链状态

Ollama 的 API 适配和浏览器纵向链路已经完成，按用户要求 **不在 Linux 安装 Ollama**。真实模型
联调只需要让虚拟机访问 Windows 上现有的 Ollama，并在数据库中给 AI 账号绑定与 Windows 模型名
一致的 active version。

已批准并准备 Microsoft `Phi-3-mini-4k-instruct-onnx` 的
`cpu-int4-rtn-block-32-acc-level-4` 目录，固定 revision 为
`5f5f794c1c23c9d5ee142af85df02a6cc52d6945`，10 个官方模型文件共 2,725,547,235 字节。
模型位于 `/home/pikachu/phase11-models/phi3-mini-4k-instruct-cpu-int4`，当前目录指纹为：

```text
sha256-tree-v1:dd0a9ea96525ac10d1875f889548c4d051a330b301bdd1cf6b9f4b5eaa220094
```

虚拟机对 `huggingface.co` 的 DNS/连接异常，因此使用 Hugging Face 镜像代理传输；下载仍固定
官方 Microsoft 仓库 commit，并按该 revision 的 LFS SHA-256 / Git blob SHA-1 逐文件校验。
完整清单保存在模型目录的 `DOWNLOAD_MANIFEST.json`。这个文件也进入项目的目录指纹，
所以部署时必须使用上面的实际指纹。

模型获批并准备完成后的显式验证入口为：

```bash
./build-phase11-onnx-release/phase11_onnx_model_smoke \
  /home/pikachu/phase11-models <相对模型目录> 32
```

它会先按实际文件计算摘要，再走与服务器相同的 `InProcessOnnxModelProvider`，逐 Token 打印输出并
检查成功终态。该命令已实际通过。模型不会被 CTest 自动下载或运行，避免 CI 意外消耗数 GB
空间和推理时间。整链数据库绑定使用
[seed_phase11_onnx.sql](../../tests/phase11/integration/seed_onnx.sql)，通用黑盒在真实模型模式下不限定固定文本，
但仍强制校验非空 token、成功终态和用户/AI 两条消息。

Adapter 训练与转换工具已经按审批安装在独立 Python 3.12 venv 中。环境固定 Olive 0.13.0、
CPU PyTorch 2.10.0、Transformers 5.3.0、PEFT 0.21.2、Accelerate 1.15.0、SciPy 1.18.1 和
ONNX Runtime GenAI 0.17.0；`pip check`、八个主要模块导入、CPU-only 包审计以及 Olive 的
Adapter 命令入口均已通过。完整环境被记录为 71 包锁文件：
[lora-requirements.lock.txt](../../scripts/phase11/lora-requirements.lock.txt)。重建入口是
[install-lora-toolchain.sh](../../scripts/phase11/install-lora-toolchain.sh)，它不会下载模型或训练数据。

Qwen `Qwen2.5-0.5B-Instruct` 已按固定 revision
`7ae557604adf67be50417f59c2c2f167def9a775` 下载并逐文件校验。10 个官方文件共
999,604,126 字节，`model.safetensors` SHA-256 为
`fdf756fa7fcbe7404d5c60e26bff1a0c8b8aa1f72ced49e7dd0210fe288fb7fe`。源目录中的
`DOWNLOAD_MANIFEST.json` 保存了官方 Git/LFS 元数据和实际摘要。

本轮没有伪造训练效果，而是建立了两个形状完全相同的 PEFT 夹具：固定随机种子的非零教学 Adapter
和全零对照 Adapter，各有 192 个张量、540,672 个参数。Olive ModelBuilder 先产生带 LoRA 的 INT4
图，`ExtractAdapters` 再把权重从图中抽成独立文件。最终部署目录为：

```text
/home/pikachu/phase11-models/adapter-learning/qwen2.5-0.5b-instruct/
└── olive-int4-adapter-deploy/
    ├── model.onnx                         # 402,911,762 bytes
    ├── adapter_weights.onnx_adapter       # 1,241,136 bytes
    ├── genai_config.json / model_config.json
    └── tokenizer.json / tokenizer_config.json / ...
```

部署目录摘要为
`sha256-tree-v1:68d72d86fe15b45f69f497ab41c43c6722466b89ee76a3ed4c6a392d8bae1c41`。
图共有 818 条输入声明和 434 个唯一输入名称：50 个普通生成/KV Cache 输入、384 个唯一 LoRA
权重/scale 输入；Olive 对每个 Adapter 输入产生了两条声明。零 Adapter 与非零 Adapter 抽取后的
`model.onnx` SHA-256 完全相同，证明比较时基础图没有变化。

Olive 0.13 与 OGA 0.17 有两处发布期 API 漂移：旧的
`onnxruntime_genai.models.quantized_model` 被拆进 `models.loaders`，且 OGA 把 Hugging Face 参数准备
从 `create_model()` 分离到了 `check_extra_options()`。项目用
[olive-oga017-compat.py](../../scripts/phase11/olive-oga017-compat.py) 在当前 Python 进程内补上旧入口，
没有修改已安装包。FP32 图虽然能导出，但 `ExtractAdapters` 峰值约 6.8 GiB 并被 OOM 杀死；改用
只含 ModelBuilder 的 INT4 流程后，导出峰值约 4.9 GiB、抽取峰值约 2.1 GiB，并成功完成。

验证分为四层：

1. [inspect-onnx-adapter-inputs.py](../../scripts/phase11/inspect-onnx-adapter-inputs.py) 确认 Adapter 输入存在；
2. [compare-onnx-adapters.py](../../scripts/phase11/compare-onnx-adapters.py) 关闭采样，在同图同 Prompt 下
   得到 151,936 个变化的 logits，最大绝对差约 1.048；
3. `phase11_onnx_model_smoke` 通过项目 C++ Provider 试装并生成，证明 `.onnx_adapter` 可由实际
   OGA C++ Runtime 读取；
4. PostgreSQL 中注册带 `adapter_artifact` 的 Active 版本后，真实 socket 黑盒完整走通 WebSocket、
   ModelRouter、ONNX Worker、SSE、消息落库和 Outbox。收到 512 个 token、3315 字节，Outbox 无积压。

需要保留的边界是：这套随机权重只证明“插槽、技能卡和换卡机构”能工作。要让 Adapter 真正学到
领域知识，还需要经过授权的数据集、训练评估和 GPU 资源；它们不在本轮许可范围内。

## 12. 目录整理与前端纵向验收（2026-10-04）

本轮把前端、Phase 11 测试、fuzz、日志实现、历史报告与性能产物移入各自职责目录。完整树、
移动表和后续放置规则见 [project-layout.md](project-layout.md)。CMake、include、脚本和文档引用均已
同步；`phase11_frontend_assets` 会在每次构建时把页面复制到 build 目录，所以从源码根目录或
build 目录启动都能正确加载 CSS 和 JavaScript。

前端从功能草图升级为可实际操作的响应式聊天界面，并保持原生 HTML/CSS/JavaScript：

- 登录页展示 HTTP、WebSocket、SSE 三条职责不同的链路；
- 登录后显示 WebSocket 和 SSE 的独立在线/重连状态；
- 会话列表、持久化历史、好友与申请、资料修改、私聊/群聊、群成员和消息搜索都有独立界面；
- Composer 支持普通消息、AI 模式、逐 Token 气泡和取消生成；
- 空状态、加载骨架、错误状态、Toast 和桌面/移动断点替代浏览器默认控件堆叠。

真实 Linux 环境从 `build-phase11-grpc-onnx-release` 目录启动服务后完成浏览器验收：

1. `/phase11/` 与 `/phase11/styles.css` 均返回 200，类型分别为 `text/html` 与 `text/css`；
2. 使用真实 PostgreSQL 账号登录成功，Cookie Session 恢复正常；
3. 页面同时显示 WebSocket 与 SSE 为“在线”；
4. 两个持久化会话及历史消息成功显示；
5. 联系人接口返回一位真实好友，待处理申请为零；
6. 服务端搜索 `hello` 返回一条持久化消息；
7. 浏览器控制台无 warning/error，1280×720 视口无水平溢出。

回归结果：Release、Debug、ASan+LSan+UBSan 三套构建均成功，三套 CTest 均为 **92/92**。
PostgreSQL 契约测试使用隔离的 `phase11_test` 数据库；真实页面使用 `phase11` 数据库。测试服务和
临时 SSH 转发在验收后关闭，虚拟机原有服务与 nginx 未改动。

## 13. 参考 UI 重构与业务接口层（2026-10-05）

本轮依据 Nebula Chat 产品参考稿重新组织前端，同时只呈现项目已经具备的功能：

- 登录页、三栏聊天工作台、好友与申请、创建私聊/群聊、历史搜索、资料和模型通道状态统一为
  深色星云视觉；没有加入语音、文件、群管理员等尚未实现的假功能；
- 右侧详情通过真实成员接口显示成员、角色和已读游标；HTTP、WebSocket、SSE 分别显示状态；
- Composer 明确提示“接收方无需在线”，但发送者与服务器断开时仍停止发送，避免把未提交数据
  错误显示成已经成功；
- 新增 `BusinessInterfaces.h`，协议适配器改为依赖四个业务接口，现有 Application Service 成为
  接口实现；Repository、PostgreSQL、模型 Provider 继续隐藏在业务层之后；
- 单元测试增加无实时连接时的群消息写入和历史恢复断言，明确验证离线接收语义。

## 14. Phase 11 文件归类整理（2026-10-05）

Phase 11 原来按层分目录，但一个目录中平铺了较多 `.h/.cpp`。本轮进一步采用 HTTP 和 WebSocket
已有的组织规则：**一个可独立理解的组件使用一个同名目录**。例如：

```text
server/http/HttpParser/HttpParser.{h,cpp}
server/websocket/WebSocketSession/WebSocketSession.{h,cpp}
server/phase11/business/ChatService/ChatService.{h,cpp}
server/phase11/ai/OllamaModelProvider/OllamaModelProvider.{h,cpp}
```

业务层也完成了物理归并：原 `application/` 中的账号、社交、聊天、AI 编排和 Outbox 实现移动到
`business/<Component>/`，业务门面保存在 `business/BusinessInterfaces/`。协议适配器仍位于
`transport/`，因此依赖方向保持为：

```text
HTTP / WebSocket / SSE
          ↓
BusinessInterfaces
          ↓
Auth / Social / Chat / AI / Outbox Service
          ↓
Ports → Storage / Security / Model Provider
```

其余分类如下：

- `ai/<Component>/`：模型抽象、Ollama、进程内 ONNX 和 gRPC Worker；
- `domain/Domain/` 与 `ports/Ports/`：领域对象和抽象端口；
- `security/<Component>/`：登录限流和 libsodium 凭据实现；
- `storage/<Component>/`：内存实现、PostgreSQL 连接池和 Repository；
- `transport/Contracts/`、`FlatJson/`、`http/`、`realtime/`：DTO、JSON 与协议适配；
- `runtime/`、`training/`：有界执行器与训练数据授权流程。

同时删除了 7 个确认无文件、无构建引用的占位目录：

```text
server/http/ResponseSender/
server/protocol/
server/protocol/ProtocolCodec/
server/protocol/Sender/
server/websocket/WebSocketAwaiter/
server/websocket/WebSocketSender/
server/phase11/application/
```

CMake 源文件列表、全部 `#include`、测试和本文档代码链接已同步到新路径。目录调整不改变命名空间、
公开业务接口、HTTP 路由、WebSocket/SSE 消息格式或数据库 Schema。

整理后在 Linux 重新执行 CMake 配置和全量构建：Release、Debug、ASan+LSan+UBSan 均构建成功；
三套 CTest 分别以 **92/92（29.32 秒）**、**92/92（29.00 秒）**、**92/92（34.00 秒）**通过，
Sanitizer 没有报告内存泄漏或未定义行为。

## 15. 演示用户网络与侧栏好友列表（2026-10-06）

本轮为联调环境补充了覆盖产品、前端、后端、测试、运维、设计、数据、安全和普通成员的演示账号。
账号初始化由 `scripts/phase11/seed-demo-users.py` 完成，整个过程只调用已有 HTTP 业务接口：

```text
注册 / 登录
    ↓
PATCH /api/me 设置角色资料
    ↓
POST /api/friend-requests 发出申请
    ↓
POST /api/friend-requests/{id} 接受申请
    ↓
POST /api/conversations 创建协作群
```

脚本没有直接写数据库。这样一来，密码散列、CSRF、好友去重、事务和 Outbox 仍由正式业务代码处理，
演示数据本身也成为一条端到端检查链。脚本可重复运行：已有账号会登录复用，已有好友关系与同名群聊
会被跳过。密码从仓库外的权限受控文件读取，不进入源码和公开文档。

前端侧栏新增常驻“我的好友”区域。它直接读取 `GET /api/friends` 的真实结果，展示显示名称、用户名、
用户 ID 和简介；点击任意好友会调用 `POST /api/conversations` 获取或创建私聊。好友在线与否不会影响
创建会话和发送消息：消息先提交到 PostgreSQL，对方下次登录后仍能读取。页面没有伪造在线状态，
因为当前 Phase 11 尚未实现完整的 Presence 生命周期。

相关文件：

- `frontend/phase11/index.html`：好友区语义结构；
- `frontend/phase11/styles.css`：可滚动的紧凑好友卡片；
- `frontend/phase11/app.js`：加载、渲染和点击私聊逻辑；
- `scripts/phase11/seed-demo-users.py`：幂等演示数据初始化脚本。
