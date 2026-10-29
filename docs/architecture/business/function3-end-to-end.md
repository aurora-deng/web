# Function3 从协议到业务、模型与公告的整体架构

```mermaid
flowchart TB
 subgraph CLIENT[浏览器：原生 JavaScript]
  NAV[统一导航：桌面顶部 / 移动底部 / 公告声音]
  CHAT[聊天 / 联系人 / 历史与 ACK]
  TREE[三维苦情树 / 分区列表 / 媒体 / 回复]
  ADMIN[站点后台 / 模型 / 举报 / 队列 / 审计]
  PLAYER[全局公告播放器：主动开启 / 多标签去重]
 end
 subgraph NET[底层通信：原服务器]
  TCP[TCP accept]
  TLS[TLS Transport + ALPN]
  DETECT[明文协议识别 / h2 prior knowledge]
  H1[HTTP/1.1 Session / Parser / Codec]
  H2[HTTP/2 Session / Stream / HPACK / Flow Control]
  WS[WebSocket Session / Frame / Heartbeat]
  SSE[SSE 长响应 / Event ID]
  REACTOR[Multi-Reactor + epoll + 协程挂起恢复]
  ROUTER[Router / WebSocket Dispatcher]
  SEND[有界 sendQueue / 慢客户端背压]
 end
 subgraph APP[业务应用与有界任务池]
  AUTH[Auth / Session / CSRF / 登录限流]
  OLD[账号 / 好友 / 私聊群聊 / 消息状态 / 文件]
  TREEB[TreeService / TreeMediaService]
  MOD[ModerationService]
  ADM[AdminService / ModelAdminService]
  DBWORK[数据库执行器：一个任务内保持整个事务]
  MEDIA[文件 / 媒体执行器：真实解码 / 期限与内存上限]
  MODELWORK[独立模型管理执行器]
 end
 subgraph STORE[存储端口与驱动]
  PORTS[Repository / UnitOfWork / Provider 端口]
  PG[(PostgreSQL：业务事实、权限、序号、版本)]
  FILES[(私有原文件 / 权限校验后的派生媒体)]
  OUT[(旧事件索引 + Function3 Outbox)]
  HANDOFF[ChatOutboxWriter：原连接/原事务/提交后通知]
  READCACHE[(现有Redis6379：公共分类30s / 榜单2s)]
 end
 subgraph EVENT[异步事件与公告]
  PUB[独立发布器 / aggregate 顺序 / 重试 / 死信]
  MQ[RocketMQ：Chat/Tree FIFO，Speech NORMAL]
  LOCAL[LocalEventBroker：默认单进程持久投递]
  INBOX[(Inbox + 持久副作用事务)]
  DELIVERY[独立投递查询小池 / 连接等待指标]
  FAILURE[(消费失败登记 / 管理员审计重投)]
  RANK[数据库时间每30秒结算 / 七日热度 / 前十]
  ANN[(公告版本 / 文字 / 用户声音设置 / 播放权)]
  SPEECH[(男女语音任务：租约 / 有限重试)]
  TTS[独立 C++ sherpa-onnx Worker / Kokoro]
  CACHE[(音频 SHA 缓存 / 配额 / 过期)]
 end
 subgraph AI[既有 AI 与授权训练链路]
  AICALL[AiChatService：配额 / 取消 / Deadline / 熔断]
  RAG[RAG：权限检索 / 引用]
  PROVIDER[IModelProvider：Ollama API / 进程内 ONNX / gRPC Worker]
  TRACE[(Prompt / 模型版本 / 结果追踪)]
  CONSENT[用户明确授权 / 脱敏 / 审核 / 去重]
  DATASET[(数据集不可变版本)]
  TRAIN[受控训练工具 / LoRA / ONNX Adapter 导出]
  EVAL[注册候选版本 / 真实评估集 / 质量门禁]
  ACT[制品试装 / 权限复核 / Active 原子切换 / 回滚]
 end
 NAV --> CHAT
 NAV --> TREE
 NAV --> ADMIN
 CHAT --> TCP
 TREE --> TCP
 ADMIN --> TCP
 TCP --> TLS --> H1
 TLS --> H2
 TCP --> DETECT --> H1
 DETECT --> H2
 H1 --> WS
 H1 --> SSE
 H1 --> REACTOR
 H2 --> REACTOR
 WS --> REACTOR
 REACTOR --> ROUTER --> AUTH
 AUTH --> OLD
 AUTH --> TREEB
 AUTH -->|认证后公共读| READCACHE
 READCACHE -->|未命中或故障| PG
 AUTH --> ADM
 AUTH --> MOD
 OLD --> DBWORK
 TREEB --> DBWORK
 MOD --> DBWORK
 TREEB --> MEDIA
 ADM --> MODELWORK
 DBWORK --> PORTS --> PG
 PORTS --> HANDOFF --> PG
 MEDIA --> FILES
 PG --> OUT --> PUB --> MQ --> INBOX
 PUB -->|显式本地模式| LOCAL --> INBOX
 INBOX --> DELIVERY --> WS
 INBOX -. 事务失败后独立登记 .-> FAILURE
 INBOX --> SSE
 PG --> RANK --> ANN --> OUT
 INBOX --> SPEECH --> TTS --> CACHE
 CACHE --> ANN
 ANN --> SSE --> PLAYER
 ANN -->|HTTP游标补发| PLAYER
 WS --> SEND
 SSE --> SEND
 H1 --> SEND
 H2 --> SEND
 SEND --> CLIENT
 OLD --> AICALL --> RAG --> PROVIDER
 PROVIDER --> TRACE
 PROVIDER --> SSE
 TRACE -->|需要明确授权| CONSENT --> DATASET --> TRAIN --> EVAL
 MODELWORK --> EVAL --> ACT --> PG
 ACT --> PROVIDER
 TREEB -. 不自动进入训练 .-> CONSENT
```

1. **通信层决定如何收发字节**，不写分类、热度或站点权限判断。
2. **Transport 决定 DTO 如何转成业务调用**，不是业务规则的唯一防线。
3. **业务层决定能不能做和如何原子完成**，依赖端口，不包含数据库和 SDK 头文件。
4. **基础设施实现端口**，把业务决定落实到 PostgreSQL、RocketMQ、文件与模型引擎。
5. **装配根连接对象**，后台 Worker 的拥有者负责停机和资源回收。
6. **客户端 ACK、MQ ACK、送达与已读各有含义**；前端状态依据服务器事实收敛。

训练图表示复用原有受控训练流程，不表示用户上传内容会自动触发训练；本轮没有启动新的自动训练任务。历史验收与性能限制见 [Phase34](phase34.md)，当前原库、语音、Redis与界面增量见[Phase35](phase35.md)。

MQ 消费者和普通聊天提交使用独立数据库连接预算。两者仍访问同一业务事实库；这是连接资源隔离，不是复制数据或拆成两个不一致数据库。

默认日常入口使用原 `phase11 / 5432` 和18090，本地持久投递支持单进程公告语音；MQ路径只有显式配置Proxy时启用。Redis不是账号、权限或消息事实库，无法命中时回落PG。实际部署入口见[日常启动](../../ops/function3-runtime.md)。
