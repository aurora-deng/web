# 业务功能实现与调用关系

## 1. 统一调用边界

```mermaid
flowchart LR
    Browser[浏览器] --> HTTP[Phase11*Http]
    Browser --> RT[Phase11Realtime]
    HTTP --> BI[BusinessInterfaces]
    RT --> BI
    BI --> Service[业务 Service]
    Service --> Tx[ITransaction]
    Service --> Repo[Repository ports]
    Repo --> Mem[InMemoryStore]
    Repo --> PG[PostgresStore]
    Service --> Outbox[IOutboxRepository]
    Outbox --> Pump[Phase11OutboxPump]
    Pump --> Sink[WS / SSE Event Sink]
```

HTTP、WebSocket 和 SSE 只负责翻译协议。权限、幂等、事务和状态机放在业务 Service，持久化细节放在 Repository。

## 2. 账号注册与登录

```mermaid
sequenceDiagram
    participant UI as 前端
    participant H as Phase11AuthHttp
    participant E as DatabaseExecutor
    participant A as AuthService
    participant C as CredentialCodec
    participant R as User/Session Repository

    UI->>H: POST /api/auth/register 或 /login
    H->>H: 解析 JSON、提取登录来源
    H->>E: 提交数据库任务
    alt 注册
        E->>A: registerUser
        A->>C: 生成 Argon2id 密码散列
        A->>R: 写 User
        A-->>H: User
        H-->>UI: 201 + User JSON，不创建登录会话
    else 登录
        E->>A: login
        A->>A: 登录失败限流检查
        A->>C: 验证密码
        A->>R: 写 Session token hash
        A-->>H: User + Session Token + CSRF Token
        H-->>UI: Cookie + User/CSRF JSON
    end
```

代码：[`Phase11AuthHttp`](../../../server/phase11/transport/http/Phase11AuthHttp/Phase11AuthHttp.cpp)、
[`AuthService`](../../../server/phase11/business/AuthService/AuthService.cpp)、
[`SodiumCredentialCodec`](../../../server/phase11/security/SodiumCredentialCodec/SodiumCredentialCodec.cpp)。

## 3. 好友与联系人

```mermaid
flowchart TD
    Search[选择联系人或输入目标用户 ID] --> Request[发送好友申请]
    Request --> Social[SocialService]
    Social --> Rules{重复/自己/黑名单检查}
    Rules -->|拒绝| Error[业务错误]
    Rules -->|通过| Tx[事务写 friend_request + outbox]
    Tx --> Notify[SSE friend.request]
    Notify --> Decision{对方处理}
    Decision -->|接受| Friend[建立双向好友关系]
    Decision -->|拒绝| Rejected[申请终态]
    Friend --> Contact[各自维护单向备注/免打扰]
    Contact --> Block{是否拉黑}
    Block -->|是| Revoke[阻断互动并按规则清理关系]
```

好友关系是双方确认后的关系；备注和免打扰是每个用户自己的单向设置。联系人离线不会阻止发送持久消息。

代码：[`SocialService`](../../../server/phase11/business/SocialService/SocialService.cpp)、
[`Phase11SocialChatHttp`](../../../server/phase11/transport/http/Phase11SocialChatHttp/Phase11SocialChatHttp.cpp)。

## 4. 消息发送与离线接收

```mermaid
sequenceDiagram
    participant U as 发送方
    participant W as Phase11Realtime
    participant C as ChatService
    participant T as Transaction
    participant M as MessageRepository
    participant O as OutboxRepository
    participant P as OutboxPump
    participant R as 接收方连接

    U->>W: chat.send(client_message_id, conversation, content)
    W->>C: 经 DatabaseExecutor 调用 sendText
    C->>T: begin
    C->>C: 会话成员/禁言/黑名单/大小校验
    C->>M: 按 client_message_id 幂等写入并分配 conversation_seq
    C->>O: 同事务写 message.created
    C->>T: commit
    C-->>W: 已提交的消息结果
    W-->>U: chat.accepted（inserted/duplicate）
    P->>O: pendingOutbox，扫描已提交事件
    alt 接收方在线
        P->>R: WebSocket 消息 + SSE 通知
    else 接收方离线
        Note over M,R: 实时通知可标记完成，消息由历史接口恢复
    end
    R->>W: chat.ack / chat.read
    W->>C: acknowledgeDelivery / markRead
    C->>M: 单调推进送达、已读和未读状态
```

“离线可发”的关键不是把 WebSocket 变成永久连接，而是**先提交数据库，再尝试实时投递**。对方重新上线后按
`conversation_seq` 拉取缺失历史，重复 `client_message_id` 不会再写一条消息。

同一条消息的后续操作仍经过业务层：

- `chat.ack` 和 `chat.read` 只能单调推进状态，迟到或重复回执不会让状态倒退；变化通过 `chat.receipt` 通知成员；
- `chat.recall` 校验发送者、会话权限和当前状态，再事务化写撤回结果和 Outbox；
- `/api/search/messages` 要求指定 `conversationId`，校验成员身份后搜索该会话，当前不是跨全部会话的全局搜索；
- 会话列表和历史查询从持久状态恢复，因此浏览器刷新或服务器重启不会只剩内存中的消息。

[Phase 27](phase27.md) 补齐浏览器侧的恢复编排：最新页用 `before=0`、旧页用排他 `before`、缺口用排他 `after` 逐页读取；重连也刷新可见旧消息的撤回状态。
文字命令 5 秒未确认时，先用 `messages/by-client-id` 查询当前发送者的原幂等键，再有限重试；前端按角色显示“管理撤回”，最终仍由 `ChatService` 授权。

代码：[`ChatService`](../../../server/phase11/business/ChatService/ChatService.cpp)、
[`Phase11Realtime`](../../../server/phase11/transport/realtime/Phase11Realtime/Phase11Realtime.cpp)、
[`OutboxDispatcher`](../../../server/phase11/business/OutboxDispatcher/OutboxDispatcher.cpp)。

## 5. 群聊治理

```mermaid
flowchart LR
    Owner[群主] --> Create[创建群]
    Create --> Members[成员与角色]
    Members --> Invite[邀请成员]
    Members --> Promote[设置管理员]
    Members --> Mute[禁言/解禁]
    Members --> Remove[移除成员]
    Members --> Leave[普通成员退出]
    Owner --> Transfer[转让群主后退出]
    Owner --> Dissolve[软解散]
    Invite --> Permission{角色和成员状态校验}
    Promote --> Permission
    Mute --> Permission
    Remove --> Permission
    Permission --> Tx[事务更新 + Outbox]
```

所有权限判断都在 `ChatService/SocialService` 和 Repository 事务内完成，前端隐藏按钮只改善体验，不构成安全边界。

## 6. 文件与头像

```mermaid
flowchart TD
    Start[POST /api/files/uploads] --> Meta[创建 Upload 元数据]
    Meta --> Chunks[PATCH 分片内容]
    Chunks --> Hash[大小/摘要校验]
    Hash --> Complete[原子封存正文]
    Complete --> Choice{用途}
    Choice -->|头像| Avatar[绑定用户 avatar_file_id]
    Choice -->|聊天文件| Message[创建 file message + outbox]
    Download[GET /api/files/:id] --> Auth{本人/会话成员/授权?}
    Auth -->|是| Stream[受控下载]
    Auth -->|否| Deny[拒绝]
```

元数据由 Repository 管理，文件正文由 `LocalFileStorage` 管理；临时分片只有完成摘要校验后才原子变成正式文件。

代码：[`FileService`](../../../server/phase11/business/FileService/FileService.cpp)、
[`Phase11FileHttp`](../../../server/phase11/transport/http/Phase11FileHttp/Phase11FileHttp.cpp)、
[`LocalFileStorage`](../../../server/phase11/storage/files/LocalFileStorage/LocalFileStorage.cpp)。

## 7. Outbox 与实时通知

```mermaid
flowchart LR
    Tx[业务事务] --> Data[业务表]
    Tx --> Event[outbox_events]
    Commit[事务提交] --> Wake[notifyCommitted]
    Wake --> Pump[Phase11OutboxPump]
    Pump --> Pending[pendingOutbox 有序批量查询]
    Pending --> Sink[Phase11RealtimeOutboxSink]
    Sink --> WS[WebSocket 在线消息]
    Sink --> SSE[SSE 通知]
    Sink --> Retry{投递结果}
    Retry -->|成功| Done[标记完成]
    Retry -->|暂时失败| Backoff[重试]
```

Outbox 解决“数据库已经成功，但进程在发通知前崩溃”的夹缝问题。业务数据与事件写在同一事务，推送可以稍后重试。

当前是单 Pump 顺序扫描，没有多消费者 claim/lease。Sink 的“成功”不是客户端送达回执；零在线连接也可以标记事件已发布，离线用户依靠消息历史恢复。AI 中间 token 由独立 SSE Event Sink 直接发布，不逐 token 写入 Outbox。详见 [教学讲义](server-business-integration-tutorial.md#10-outbox保证的是什么不保证什么)。

### 多设备 Presence

```mermaid
sequenceDiagram
    participant D1 as 设备 A
    participant D2 as 设备 B
    participant WS as WebSocketSessionManager
    participant DB as DatabaseExecutor
    participant S as SocialService/Repository
    participant O as Outbox/SSE

    D1->>WS: WebSocket 注册(user, connection key)
    WS->>WS: 当前进程连接计数变为 1
    WS->>DB: PresenceBridge 提交计数变化
    DB->>S: recordPresence，保存 last-active
    S->>O: presence.update
    D2->>WS: 同一用户第二个连接注册
    WS->>DB: 上报当前连接数 2
    D1--xWS: 设备 A 断开
    WS->>DB: 上报当前连接数 1
    DB->>S: recordPresence
    Note over S,O: 设备 B 仍在线，不发布用户离线
    D2--xWS: 最后设备断开
    WS->>DB: 上报连接数 0
    DB->>S: recordPresence
    S->>O: presence.update(offline) + last-active
```

Presence 是“用户是否至少有一个有效设备在线”，不能把某一个 fd 当成用户本身。连接注册和断开先回到所属
Reactor，再把持久化工作交给数据库执行器，避免占住网络线程。当前连接计数由进程内 SessionManager 维护，没有数据库设备租约表或跨实例 lease 协调。

## 8. AI 生成与 RAG

```mermaid
sequenceDiagram
    participant UI as 用户
    participant RT as Phase11Realtime
    participant AI as AiChatService
    participant RAG as RagService
    participant IDX as IKnowledgeIndex
    participant MR as ModelRouter
    participant RP as ResilientModelProvider
    participant P as Ollama/ONNX/gRPC Provider
    participant SSE as SSE Event Sink

    UI->>RT: ai.generate(conversation, ai_account, prompt)
    RT->>AI: generate + CancellationToken
    AI->>AI: 成员、AI 账号、并发配额和 deadline 校验
    AI->>RAG: buildContext(user, conversation)
    RAG->>RAG: 先计算允许访问的文档集合
    RAG->>IDX: 只在允许集合中检索
    IDX-->>RAG: chunks + scores
    RAG-->>AI: prompt context + citation candidates
    AI->>MR: 选择账号绑定的活动模型版本
    MR->>RP: generate
    RP->>P: 主端点；符合条件时同 Runtime 备用端点
    loop 每个 token
        P-->>AI: onToken
        AI->>SSE: ai.token
    end
    P-->>AI: complete/status
    AI->>AI: 保存生成记录、引用、延迟和终态
    AI->>SSE: ai.completed
```

Provider 统一接口屏蔽 Ollama HTTP、进程内 ONNX 和独立 gRPC Worker 的差异。RAG 先做权限集合，再做检索；
引用保存的是生成当时真正使用的切片，而不是事后重新搜索的结果。

代码：[`AiChatService`](../../../server/phase11/business/AiChatService/AiChatService.cpp)、
[`RagService`](../../../server/phase11/ai/RagService/RagService.cpp)、
[`ModelRegistryService`](../../../server/phase11/ai/ModelRegistryService/ModelRegistryService.cpp)、
[`ResilientModelProvider`](../../../server/phase11/ai/ResilientModelProvider/ResilientModelProvider.cpp)。

## 9. 训练、评测、发布与回滚

```mermaid
flowchart TD
    Msg[用户消息] --> Consent{用户明确授权?}
    Consent -->|否/已撤销| Stop[不得进入数据集]
    Consent -->|是| Candidate[Training Candidate]
    Candidate --> Review[脱敏/审核/去重]
    Review --> Dataset[不可变 Dataset Version<br/>内容摘要]
    Dataset --> Export[JsonlTrainingDataExporter]
    Export --> Train[train-qwen-lora.py]
    Train --> Adapter[LoRA Adapter]
    Adapter --> Convert[Olive / OGA ONNX 导出]
    Convert --> Manifest[制品摘要 + 训练血缘]
    Manifest --> Register[登记 Candidate Model Version]
    Register --> Evaluate[ModelEvaluationService<br/>真实逐题评测]
    Evaluate --> Gate{质量门禁通过?}
    Gate -->|否| Reject[拒绝激活，旧版本继续服务]
    Gate -->|是| Human[人工确认发布]
    Human --> Validate[运行时试装 + checksum/兼容性]
    Validate --> Activate[原子激活 + 审计]
    Activate --> Serve[ModelRouter 新请求选中新版本]
    Serve --> Rollback{异常或人工回滚}
    Rollback --> Previous[恢复上一个已验证版本 + 审计]
```

训练脚本不直接改线上活动模型。数据授权、数据集摘要、模型制品摘要、评测结果、人工发布和审计串成一条可追踪血缘。
任何一步失败都保留当前活动版本。

代码：[`TrainingDataService`](../../../server/phase11/training/TrainingDataService/TrainingDataService.cpp)、
[`JsonlTrainingDataExporter`](../../../server/phase11/training/JsonlTrainingDataExporter/JsonlTrainingDataExporter.cpp)、
[`ModelEvaluationService`](../../../server/phase11/ai/ModelEvaluationService/ModelEvaluationService.cpp)、
[`ModelRegistryService`](../../../server/phase11/ai/ModelRegistryService/ModelRegistryService.cpp)、
[`训练管理工具`](../../../tools/phase11_training_admin.cpp)、
[`模型管理工具`](../../../tools/phase11_model_admin.cpp)。

## 10. 数据与迁移对应关系

| 迁移 | 业务主题 |
|---|---|
| `001_phase11.sql` | 用户、Session、好友、会话、消息、Outbox、基础模型表 |
| `002_message_lifecycle.sql` | 送达、已读、撤回、会话摘要 |
| `003_group_governance.sql` | 群角色、禁言、退出与解散 |
| `004_contacts_presence.sql` | 联系人设置、黑名单、多设备在线状态 |
| `005_files.sql` | 文件元数据、上传、头像和文件消息 |
| `006_ai_governance.sql` | 模型审计和生成追踪 |
| `007_model_evaluation.sql` | 评测运行与结果 |
| `008_evaluation_suite_versioning.sql` | 不可变评测集版本 |
| `009_rag_knowledge_and_citations.sql` | 知识文档、切片、检索与引用 |
| `010_training_dataset_artifacts.sql` | 训练数据集和制品 |
| `011_training_candidate_deduplication.sql` | 授权候选去重 |
| `012_model_training_lineage.sql` | 训练到模型版本的不可变血缘 |

表字段和关系详见 [数据库文档](../../database/README.md)。

## 11. 健康检查、指标与进程恢复

```mermaid
flowchart LR
    Runtime[ServerRuntime] --> Live["/health/live<br/>进程存活"]
    Runtime --> Ready["/health/ready<br/>server.ready + store.healthy"]
    Runtime --> Metrics[/metrics<br/>HTTP/WS/SSE/DB/AI/Outbox 指标]
    PG[(PostgreSQL)] --> Ready
    Provider[AI Provider] --> Metrics
    Outbox[OutboxPump] --> Metrics
    LongConn[WS/SSE 长连接] --> Metrics
    Supervisor[用户态 Supervisor] --> Live
    Supervisor --> Ready
    Supervisor --> Decision{异常类型与重启预算}
    Decision -->|可恢复| Restart[有退避地重启服务]
    Decision -->|反复失败| Stop[停止重启并保留诊断]
    Benchmark[Phase 15/26 基准] --> Metrics
    Benchmark --> Capacity[QPS/P99/长连接/AI 并发容量证据]
```

- **liveness** 回答“进程是否仍在运行”，**readiness** 回答“现在是否适合接新请求”；两者不能共用一个结论。
- 当前 readiness 检查服务器状态与数据库健康，不探测每个 AI Provider 的生成能力；`/metrics` 输出 Prometheus 文本。
- Supervisor 使用 PID 与进程启动时刻共同确认身份，避免 PID 被复用后误杀另一个进程。
- Phase 15 测真实账号和聊天业务，Phase 26 测长连接与 AI 混合负载；测试数字必须和硬件、构建类型、配置及错误数一起记录。
- 指标用于定位容量瓶颈，不替代业务持久化，也不能把单机一次通过写成生产 SLA。

对应文档：[Phase 15](phase15.md)、[Phase 16](phase16.md)、[Phase 25](phase25.md)、[Phase 26](phase26.md)。
