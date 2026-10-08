# 文档导航

文档分为「当前架构」「课程演进」「验证与运维」。`phase5.md` 是本次 SSE 接入，旧 WebSocket 课程的阶段 5–15 统一改名为 `lesson05`–`lesson15`，放在 `websocket-lessons/`，避免与后续接入阶段撞名。

| 主题 | 入口 |
|---|---|
| 当前系统和版本对比 | [项目 README](../README.md) |
| 架构总览 | [architecture/ARCHITECTURE.md](architecture/ARCHITECTURE.md) |
| 项目目录分层与文件放置规则 | [architecture/project-layout.md](architecture/project-layout.md) |
| 一次请求的路径 | [architecture/request_flow.md](architecture/request_flow.md) |
| SSE 接入（Phase 5） | [architecture/phase5.md](architecture/phase5.md) |
| HTTP/2 接入与手写教学对照（Phase 6，test8.0） | [architecture/phase6.md](architecture/phase6.md) |
| TLS/ALPN 接入与教学版（Phase 7，test8.0） | [architecture/phase7.md](architecture/phase7.md) |
| gRPC C++ 接入与消息封装教学版（Phase 8，同步 API 基线） | [architecture/phase8.md](architecture/phase8.md) |
| 生产化护栏与 gRPC Callback Reactor（Phase 9，test9.0） | [architecture/phase9.md](architecture/phase9.md) |
| test9.0 → test9.1 版本差异 | [architecture/test9.1.md](architecture/test9.1.md) |
| test9.1 → Function1.0 完整差异 | [architecture/function1.0-vs-test9.1.md](architecture/function1.0-vs-test9.1.md) |
| HTTP/gRPC 性能路径、架构拆分与 Linux 实测（Phase 10） | [architecture/phase10.md](architecture/phase10.md) |
| 账号、社交聊天、AI Provider 与模型治理地基（Phase 11，Function1.0） | [architecture/phase11.md](architecture/phase11.md) |
| 持久未读、送达/已读回执与消息撤回（Phase 12，本地分支） | [architecture/phase12.md](architecture/phase12.md) |
| 群主、管理员、禁言、退出与软解散（Phase 13，本地分支） | [architecture/phase13.md](architecture/phase13.md) |
| 好友备注、免打扰、黑名单与多设备 Presence（Phase 14，本地分支） | [architecture/phase14.md](architecture/phase14.md) |
| HTTP/WS/SSE/PostgreSQL 真实业务性能基线（Phase 15，本地分支） | [architecture/phase15.md](architecture/phase15.md) |
| 可观测性、Outbox 主动唤醒与运维闭环（Phase 16，本地分支） | [architecture/phase16.md](architecture/phase16.md) |
| 头像、图片/文件消息、分片上传与权限下载（Phase 17，本地分支） | [architecture/phase17.md](architecture/phase17.md) |
| AI 生成韧性、配额、期限、熔断与备用端点（Phase 18，本地分支） | [architecture/phase18.md](architecture/phase18.md) |
| 模型变更审计与 AI 生成追踪（Phase 19，本地分支） | [architecture/phase19.md](architecture/phase19.md) |
| 模型评测集与激活质量门禁（Phase 20，本地分支） | [architecture/phase20.md](architecture/phase20.md) |
| RAG 知识检索、权限隔离与回答引用链（Phase 21，本地分支） | [architecture/phase21.md](architecture/phase21.md) |
| 授权数据集与真实 LoRA 微调闭环（Phase 22，本地分支） | [architecture/phase22.md](architecture/phase22.md) |
| 候选模型真实评测与人工发布（Phase 23，本地分支） | [architecture/phase23.md](architecture/phase23.md) |
| 训练到候选模型不可变血缘（Phase 24，本地分支） | [architecture/phase24.md](architecture/phase24.md) |
| 用户态进程监督与异常恢复（Phase 25，本地分支） | [architecture/phase25.md](architecture/phase25.md) |
| 长连接容量、AI 并发与混合负载（Phase 26，待长稳后实测） | [architecture/phase26.md](architecture/phase26.md) |
| Phase 11 外部依赖审批、位置和卸载说明 | [architecture/phase11-dependency-gates.md](architecture/phase11-dependency-gates.md) |
| Phase 11 PostgreSQL 表结构、关系和业务写入流程 | [database/README.md](database/README.md) |
| WebSocket 课程演进 | [architecture/websocket-lessons/README.md](architecture/websocket-lessons/README.md) |
| Reactor 基础课程 | [architecture/phase3/README.md](architecture/phase3/README.md) |
| WebSocket 初次接入 | [architecture/phase4_upgrade.md](architecture/phase4_upgrade.md) |
| 测试说明 | [testing/TESTING.md](testing/TESTING.md) |
| 测试学习材料 | [testing/learning/测试学习综合讲义和笔记/README.md](testing/learning/测试学习综合讲义和笔记/README.md) |
| 性能实验方法、业务基线、A/B 与容量矩阵 | [performance/BENCHMARK.md](performance/BENCHMARK.md)、[architecture/phase15.md](architecture/phase15.md)、[architecture/phase16.md](architecture/phase16.md)、[architecture/phase26.md](architecture/phase26.md) |
| 调试 | [debugging/CHEATSHEET.md](debugging/CHEATSHEET.md) |
| 部署环境 | [ops/environment.md](ops/environment.md) |
| 停机与收尾 | [closeout/CLOSEOUT.md](closeout/CLOSEOUT.md) |
| 历史材料 | [archive/](archive/) |

学习顺序建议：先看项目 README 的整体图，再读架构总览；已掌握旧版的读者可直接进入 Phase 5，遇到连接代际、出站队列、协程交接时回查 WebSocket 课程。
