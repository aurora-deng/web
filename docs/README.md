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
| Phase 11 外部依赖审批、位置和卸载说明 | [architecture/phase11-dependency-gates.md](architecture/phase11-dependency-gates.md) |
| Phase 11 PostgreSQL 表结构、关系和业务写入流程 | [database/README.md](database/README.md) |
| WebSocket 课程演进 | [architecture/websocket-lessons/README.md](architecture/websocket-lessons/README.md) |
| Reactor 基础课程 | [architecture/phase3/README.md](architecture/phase3/README.md) |
| WebSocket 初次接入 | [architecture/phase4_upgrade.md](architecture/phase4_upgrade.md) |
| 测试说明 | [testing/TESTING.md](testing/TESTING.md) |
| 测试学习材料 | [testing/learning/测试学习综合讲义和笔记/README.md](testing/learning/测试学习综合讲义和笔记/README.md) |
| 性能 | [performance/BENCHMARK.md](performance/BENCHMARK.md) |
| 调试 | [debugging/CHEATSHEET.md](debugging/CHEATSHEET.md) |
| 部署环境 | [ops/environment.md](ops/environment.md) |
| 停机与收尾 | [closeout/CLOSEOUT.md](closeout/CLOSEOUT.md) |
| 历史材料 | [archive/](archive/) |

学习顺序建议：先看项目 README 的整体图，再读架构总览；已掌握旧版的读者可直接进入 Phase 5，遇到连接代际、出站队列、协程交接时回查 WebSocket 课程。
