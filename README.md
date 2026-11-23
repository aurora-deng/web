# NEBULA · C++20 高级 Web 服务器与实时应用

**最新发布 [Function3.6](https://github.com/aurora-deng/web/releases/tag/Function3.6)**。提供 C++ 后端 DAG、gRPC 车队、Agent 审批与 Observer 回放，以及可校验的 Linux ZIP 和离线 Web/Fleet Docker 镜像。 [源码分支](https://github.com/aurora-deng/web/tree/Function3.6) · [部署与回退](https://github.com/aurora-deng/web/blob/Function3.6/docs/ops/runtime/function3.6-deployment.md) · [与 Function3.5 的区别](https://github.com/aurora-deng/web/blob/Function3.6/docs/releases/function3.6.md)

[各业务说明与学习](https://github.com/aurora-deng/web/blob/Function3.6/docs/business/README.md) · [业务上线台账](https://github.com/aurora-deng/web/blob/Function3.6/docs/business/release-matrix.md) · [参与维护](CONTRIBUTING.md)

NEBULA 在 Linux 使用 epoll、Multi-Reactor 和 C++20 协程，支持 HTTP、WebSocket、SSE、HTTP/2、TLS/ALPN 和 gRPC。缘续-聊天、缘生-广场、研途和小研提供可复放的业务场景，Observer 用指标、结构化日志和调用链帮助定位故障。Windows Companion 提供本机入口与私人资料管理。

- 缘续-聊天与缘生-广场共用账号和导航。会话检查期间保留加载状态，广场滚轮换页加入视觉过渡。研途用同一个 NEBULA 账号打开对应学习工作区，客户端可以切号后返回原模块。[Function3.3 变更与验收](https://github.com/aurora-deng/web/blob/Function3.3/docs/releases/function3.3.md)
- 通信核心、共享业务、研途和 Observer 各有明确边界。项目可选择 Linux 通信档、PostgreSQL 业务档与可选 Windows 客户端。[目录总图](https://github.com/aurora-deng/web/blob/Function3.3/docs/architecture/overview/project-layout.md) · [复现指南](https://github.com/aurora-deng/web/blob/Function3.3/docs/ops/validation/reproduce.md)
- 历史隔离演练覆盖 PostgreSQL 断连、ACK 后进程强杀、历史恢复和同键去重。混合负载的 ACK 长尾仍需复测和优化。[慢 ACK 排查讲义](https://github.com/aurora-deng/web/blob/Function3.3/docs/observability/ack-diagnosis.md) · [后续路线](https://github.com/aurora-deng/web/blob/Function3.3/docs/roadmap/next-steps.md)

## 获取并运行

```bash
git clone --branch Function3.6 --single-branch https://github.com/aurora-deng/web.git
cd web
```

从发布页下载镜像或 ZIP 并核对 `SHA256SUMS`。Docker 归档使用 Linux x86-64 主机现有兼容库，网页业务连接外部 PostgreSQL。源码构建可选择通信档、PostgreSQL 业务档和独立车队验证。 [文档首页](https://github.com/aurora-deng/web/blob/Function3.6/docs/README.md) · [验收](https://github.com/aurora-deng/web/blob/Function3.6/docs/ops/validation/m00-m07.md) · [研途运行](https://github.com/aurora-deng/web/blob/Function3.6/docs/ops/runtime/nebula-yantu-community.md)

## 主要版本

| 分支 | 阶段 | 主要内容 |
|---|---|---|
| [Function3.6](https://github.com/aurora-deng/web/tree/Function3.6) | C++ 调度和可验证交付 | 后端 DAG、gRPC 车队、Agent 审批、Docker 与 ZIP |
| [Function3.5](https://github.com/aurora-deng/web/tree/Function3.5) | 个性化与诊断 | 小研偏好、会话压缩、Envoy 路由和 Observer 增量刷新 |
| [Function3.3](https://github.com/aurora-deng/web/tree/Function3.3) | 账号工作区与诊断 | 研途账号同步、Observer、小研、界面切换和 Envoy 实验 |
| [Function3.2](https://github.com/aurora-deng/web/tree/Function3.2) | 复现与诊断 | 社区模块拆分、隔离复现、连接池恢复、ACK 分段测量 |
| [Function3.1](https://github.com/aurora-deng/web/tree/Function3.1) | 界面与治理 | 统一水岸主题、三分钟撤回、语音、AI 与社区体验 |
| [Function2.1](https://github.com/aurora-deng/web/tree/Function2.1) | 聊天闭环 | 历史分页、断线补发、幂等重试与管理员撤回 |
| [Function1.0](https://github.com/aurora-deng/web/tree/Function1.0) | Phase 11 | 账号、数据库、好友、聊天、离线消息与 AI Provider |
| [test9.1](https://github.com/aurora-deng/web/tree/test9.1) | Phase 10 | HTTP/gRPC 热路径、生命周期与性能优化 |
| [test9.0](https://github.com/aurora-deng/web/tree/test9.0) | Phase 9 | gRPC Callback API、运行保护和故障测试 |
| [test8.0](https://github.com/aurora-deng/web/tree/test8.0) | Phase 6–7 | HTTP/2、TLS、ALPN 和 Stream 协程 |
| [test7.0](https://github.com/aurora-deng/web/tree/test7.0) | Phase 5 | SSE 长响应、事件编码和会话生命周期 |
| [test6.3](https://github.com/aurora-deng/web/tree/test6.3) | WebSocket 阶段 | WebSocket 协议栈、会话管理和异步发送 |
