# 文档导航

架构文档先按职责分为两大区域：`communication/` 讲 Reactor、HTTP、WebSocket、SSE、HTTP/2、TLS 和 gRPC；
`business/` 讲账号、社交聊天、文件、AI、RAG、训练与模型治理。`ARCHITECTURE.md` 把两块串成完整端到端流程。
测试、性能、数据库和运维文档继续放在各自专题目录。

| 主题 | 入口 |
|---|---|
| 当前系统和版本对比 | [项目 README](../README.md) |
| 架构总览 | [architecture/ARCHITECTURE.md](architecture/ARCHITECTURE.md) |
| 底层通信架构总入口 | [architecture/communication/README.md](architecture/communication/README.md) |
| 各协议逻辑与调用关系图 | [architecture/communication/protocol-call-flows.md](architecture/communication/protocol-call-flows.md) |
| 业务功能架构总入口 | [architecture/business/README.md](architecture/business/README.md) |
| 业务实现与调用关系图 | [architecture/business/business-call-flows.md](architecture/business/business-call-flows.md) |
| 业务接入服务器教学：六课、真实调用链与代码索引 | [architecture/business/server-business-integration-tutorial.md](architecture/business/server-business-integration-tutorial.md) |
| 历史分页、断线补齐、ACK 幂等重试与管理员撤回（Phase 27） | [architecture/business/phase27.md](architecture/business/phase27.md) |
| 项目目录分层与文件放置规则 | [architecture/project-layout.md](architecture/project-layout.md) |
| 一次请求的路径 | [architecture/communication/request_flow.md](architecture/communication/request_flow.md) |
| SSE 接入（Phase 5） | [architecture/communication/phase5.md](architecture/communication/phase5.md) |
| HTTP/2 接入与手写教学对照（Phase 6，test8.0） | [architecture/communication/phase6.md](architecture/communication/phase6.md) |
| TLS/ALPN 接入与教学版（Phase 7，test8.0） | [architecture/communication/phase7.md](architecture/communication/phase7.md) |
| gRPC C++ 接入与消息封装教学版（Phase 8，同步 API 基线） | [architecture/communication/phase8.md](architecture/communication/phase8.md) |
| 生产化护栏与 gRPC Callback Reactor（Phase 9，test9.0） | [architecture/communication/phase9.md](architecture/communication/phase9.md) |
| test9.0 → test9.1 版本差异 | [architecture/test9.1.md](architecture/test9.1.md) |
| test9.1 → Function1.0 完整差异 | [architecture/function1.0-vs-test9.1.md](architecture/function1.0-vs-test9.1.md) |
| Function1.0 → Function2 完整差异 | [architecture/function2-vs-function1.0.md](architecture/function2-vs-function1.0.md) |
| Function2 → Function2.1 差异与后续建议 | [architecture/function2.1-vs-function2.md](architecture/function2.1-vs-function2.md) |
| HTTP/gRPC 性能路径、架构拆分与 Linux 实测（Phase 10） | [architecture/communication/phase10.md](architecture/communication/phase10.md) |
| 账号、社交聊天、AI Provider 与模型治理地基（Phase 11，Function1.0） | [architecture/business/phase11.md](architecture/business/phase11.md) |
| 持久未读、送达/已读回执与消息撤回（Phase 12，Function2） | [architecture/business/phase12.md](architecture/business/phase12.md) |
| 群主、管理员、禁言、退出与软解散（Phase 13，Function2） | [architecture/business/phase13.md](architecture/business/phase13.md) |
| 好友备注、免打扰、黑名单与多设备 Presence（Phase 14，Function2） | [architecture/business/phase14.md](architecture/business/phase14.md) |
| HTTP/WS/SSE/PostgreSQL 真实业务性能基线（Phase 15，Function2） | [architecture/business/phase15.md](architecture/business/phase15.md) |
| 可观测性、Outbox 主动唤醒与运维闭环（Phase 16，Function2） | [architecture/business/phase16.md](architecture/business/phase16.md) |
| 头像、图片/文件消息、分片上传与权限下载（Phase 17，Function2） | [architecture/business/phase17.md](architecture/business/phase17.md) |
| AI 生成韧性、配额、期限、熔断与备用端点（Phase 18，Function2） | [architecture/business/phase18.md](architecture/business/phase18.md) |
| 模型变更审计与 AI 生成追踪（Phase 19，Function2） | [architecture/business/phase19.md](architecture/business/phase19.md) |
| 模型评测集与激活质量门禁（Phase 20，Function2） | [architecture/business/phase20.md](architecture/business/phase20.md) |
| RAG 知识检索、权限隔离与回答引用链（Phase 21，Function2） | [architecture/business/phase21.md](architecture/business/phase21.md) |
| 授权数据集与真实 LoRA 微调闭环（Phase 22，Function2） | [architecture/business/phase22.md](architecture/business/phase22.md) |
| 候选模型真实评测与人工发布（Phase 23，Function2） | [architecture/business/phase23.md](architecture/business/phase23.md) |
| 训练到候选模型不可变血缘（Phase 24，Function2） | [architecture/business/phase24.md](architecture/business/phase24.md) |
| 用户态进程监督与异常恢复（Phase 25，Function2） | [architecture/business/phase25.md](architecture/business/phase25.md) |
| 长连接容量、AI 并发与混合负载（Phase 26，待长稳后实测） | [architecture/business/phase26.md](architecture/business/phase26.md) |
| Phase 11 外部依赖审批、位置和卸载说明 | [architecture/business/phase11-dependency-gates.md](architecture/business/phase11-dependency-gates.md) |
| Phase 11 PostgreSQL 表结构、关系和业务写入流程 | [database/README.md](database/README.md) |
| WebSocket 课程演进 | [architecture/communication/websocket-lessons/README.md](architecture/communication/websocket-lessons/README.md) |
| Reactor 基础课程 | [architecture/communication/phase3/README.md](architecture/communication/phase3/README.md) |
| WebSocket 初次接入 | [architecture/communication/phase4_upgrade.md](architecture/communication/phase4_upgrade.md) |
| 测试说明 | [testing/TESTING.md](testing/TESTING.md) |
| 测试学习材料 | [testing/learning/测试学习综合讲义和笔记/README.md](testing/learning/测试学习综合讲义和笔记/README.md) |
| 性能实验方法、业务基线、A/B 与容量矩阵 | [performance/BENCHMARK.md](performance/BENCHMARK.md)、[architecture/business/phase15.md](architecture/business/phase15.md)、[architecture/business/phase16.md](architecture/business/phase16.md)、[architecture/business/phase26.md](architecture/business/phase26.md) |
| 调试 | [debugging/CHEATSHEET.md](debugging/CHEATSHEET.md) |
| 部署环境 | [ops/environment.md](ops/environment.md) |
| 停机与收尾 | [closeout/CLOSEOUT.md](closeout/CLOSEOUT.md) |
| 历史材料 | [archive/](archive/) |

学习顺序建议：先读总架构末尾的大图，再沿“底层通信架构 → 协议调用流程 → 业务架构 → 业务调用流程”
建立当前全貌；需要了解演进原因时，再进入对应 Phase 文档和 WebSocket/Reactor 课程。
