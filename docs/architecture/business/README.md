# 业务功能分类

业务服务判断“能不能做、写什么、通知谁”；通信层负责安全传递请求和结果。按领域查正文，按Phase时间线查演进过程。

|领域/用途|入口与内容|
|---|---|
|[账号与聊天社交](accounts-chat/README.md)|认证、好友、私聊群聊、文件、未读/ACK/撤回、恢复与搜索|
|[AI与模型治理](ai/README.md)|Provider、RAG、评测、授权训练、血缘、三种助手与语音|
|[社区与管理](community/README.md)|苦情树、动态、媒体、榜单、公告、个人主页、站点权限|
|[客户端与前端](client-ui/README.md)|Windows本地资料、浏览器布局、交互与模块化|
|[MQ与事件投递](messaging/README.md)|Outbox、RocketMQ、Inbox、重试和人工重放|
|[业务运行与性能](runtime/README.md)|业务执行器、运行维护、指标、性能与容量|
|[业务分层与工程重整](engineering/README.md)|基线、检查机制、领域拆分、装配和接口对齐|
|[教学讲义](guides/README.md)|一次用户操作怎样走过接口、服务、事务和实时通知|
|[架构与调用图](flows/README.md)|完整链路与各阶段总图|
|[实现索引](indexes/README.md)|组件职责和源码文件位置|

当前总图见 [完整链路](flows/current-end-to-end.md)，下一阶段见 [建议路线](../../roadmap/next-steps.md)；两者分别描述已实现和待实施内容。

当前优先阅读：[Phase49改动与限制](engineering/phase49.md) → [接口教学](guides/phase49-teaching.md) → [数据库分层](../../database/domain-layering.md)。

[阶段时间线](../../roadmap/stage-index.md) · [进度台账](../../roadmap/function3-progress.md) · [阶段验收](../../reports/stages/README.md) · [回到文档分类](../../README.md)

## 固定边界

```text
前端页面 → 领域API契约 → HTTP/WS适配器 → Application Service
                                         ↓
                          领域规则 / Repository Port
                                         ↓
                       PostgreSQL / 文件 / 模型等实现
                                         ↓
                          同事务Outbox → WS/SSE/MQ
```

Linux保存共享业务的权威状态；Windows保存账号隔离的私人助手资料；浏览器负责交互。领域之间通过明确接口和事件协作，不靠直接访问对方表来复制规则。目录拆分不等于性能提升，历史未达标结果继续保留。
