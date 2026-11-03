# Phase43–47端到端：通信核心固定，业务逐领域演进

```mermaid
flowchart TB
  subgraph Windows[Windows用户电脑]
    Local[Companion本机入口 / TLS代理]
    Private[(本地资料、词法索引、长期记忆、两个私人历史)]
    Agent[确认式Agent / 私人RAG]
    Local <--> Agent <--> Private
  end
  subgraph Browser[浏览器]
    UI[聊天 / 好友式AI / 全局助手 / 神树 / 主页 / 管理]
    Context[ChatContext + 9个控制器]
    View[虚拟列表、未读水位、偏好版本、资源清单]
    UI <--> Context <--> View
  end
  UI <--> Local
  subgraph Linux[Linux服务器]
    subgraph Frozen[backend/server：本轮冻结]
      TCP[TCP / TLS + ALPN]
      Reactor[Multi-Reactor / 连接协程 / HTTP1、H2、WS、SSE]
      Routes[Router / Dispatcher / 身份和完成状态]
      Outgoing[sendQueue / 背压 / Sender]
      TCP --> Reactor --> Routes
      Outgoing --> TCP
    end
    subgraph Business[backend/application：共享权威业务]
      HTTP[业务HTTP / WS / SSE适配器]
      Tasks[有界DB / 文件 / 知识任务]
      Service[账号、社交、聊天工作区、消息、文件、树、管理用例]
      Ports[领域规则和Repository Port]
      Drivers[PostgresStore兼容门面 → 领域驱动]
      PG[(原业务库 / 独立隔离测试库)]
      Outbox[事务内Outbox]
      Delivery[提交后唤醒 / 本地或MQ投递 / Inbox去重]
      HTTP --> Tasks --> Service --> Ports --> Drivers --> PG
      Drivers --> Outbox --> Delivery
    end
    subgraph AI[backend/ai：独立目标与Worker]
      Router[模型路由、预算、取消、切换与门禁]
      Inference[Ollama API / ONNX进程内 / 独立gRPC Worker]
      Ephemeral[私人推理限时RAM状态，不写聊天库/训练库/正文日志]
      Train[显式授权样本 → 脱敏/审核/数据集 → 转换/训练Worker]
      Eval[候选版本 → 评测 → 激活/回滚 → 审计]
      TTS[本地TTS / 已配置云端 / 无Key或失败回退]
      Router --> Inference
      Ephemeral --> Router
      Train --> Eval --> Router
    end
    Routes --> HTTP
    HTTP --> Ephemeral
    Service --> Router
    Service --> Train
    Delivery --> TTS
    Delivery --> Outgoing
    Inference --> Service
    Ephemeral --> HTTP
    HTTP --> Outgoing
    TTS --> Delivery
  end
  Local <--> TCP
  UI -. 直接访问公共网页 .-> TCP
  Agent -. 仅已授权片段，经TLS .-> Ephemeral
```

1. **权威状态在Linux**：账号、群成员、共享消息、公开故事和权限不能让浏览器或Windows自行决定。HTTP/WS只是不同入口，同一业务规则与事务不可分叉。
2. **私人资料在Windows**：好友式和侧栏历史独立，共享授权资料/长期记忆；仅必要授权片段送往私人推理路径。群AI不读取这些私人知识。网页助手走明确非保密确认，和本地私人入口区分。
3. **数据库任务有界**：一个事务持有一条任务作用域连接；领域驱动委托不会拆掉消息/偏好与Outbox的原子性。网络完成由既有连接/Stream身份和至多一次完成规则守护。
4. **输出有两种证明**：HTTP/PATCH结果或chat.accepted证明服务器已接受/落库；WS/SSE投递是后续通知；chat.ack和chat.read又是接收与已读水位。MQ确认不能代替它们。
5. **模型质量另算**：既有转换、Adapter、评测门禁与训练授权流程保留。本轮真实调用证明链路可通，不证明模型质量、语音自然度或生产容量。没有自动训练、自动发布或新云账号。
6. **部署另算**：本轮只升级隔离环境、构建客户端和验证定义；原业务库、日常端口/服务不因源码更新而自动替换。状态页区分运行二进制、前端资源和测试结论。

逐文件索引见 [本轮代码索引](../indexes/phase43-47-code-index.md)，一次业务如何接入见 [三次用户操作教学](../guides/phase43-47-teaching.md)。

## Phase48内部细化

网络、Windows私人存储与Linux权威业务的位置不变。业务HTTP细分为联系人/会话/消息历史/群治理适配器，Store只保留连接/事务及兼容委托；数据库驱动与内存驱动按领域配对。前端事件与控制器通过显式资源/命令端口协作，成功Outbox前缀采用批量确认。详图和事务边界见 [Phase48](../engineering/phase48.md)。
