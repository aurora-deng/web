# NEBULA · C++20 高级 Web 服务器与实时应用

**最新开发快照：[Function3.2](https://github.com/aurora-deng/web/tree/Function3.2)**。`main` 是版本导航；源码、构建入口、测试和文档在功能分支。

NEBULA 从 Linux 的 epoll、Multi-Reactor、C++20 协程出发，支持 HTTP、WebSocket、SSE、HTTP/2、TLS/ALPN 和 gRPC。应用层包括账号与好友、私聊群聊、苦情树社区、个人主页、AI 助手和语音通知；可选 Windows 客户端保存私人历史、资料和检索索引。共享业务与 PostgreSQL 在 Linux，浏览器负责界面和实时交互。

- [版本差异与故障、性能实测](https://github.com/aurora-deng/web/blob/Function3.2/docs/releases/function3.2.md)
- [干净 Linux 环境构建与验证](https://github.com/aurora-deng/web/blob/Function3.2/docs/ops/validation/reproduce.md)
- [架构、文件分层与文档分类](https://github.com/aurora-deng/web/blob/Function3.2/docs/README.md)
- [Obsidian 学习计划：12 课、实验清单与助教提示词](https://github.com/aurora-deng/web/blob/Function3.2/docs/learning/nebula-performance-and-reliability.md)

## 获取并复现

```bash
git clone --branch Function3.2 --single-branch https://github.com/aurora-deng/web.git
cd web
```

使用已安装的 Linux 工具链，按复现文档选择通信档或 PostgreSQL 业务档。业务档应用迁移 001–024 到脚本自建的临时库。配置不会自动下载依赖。

本轮隔离业务档 112 项 CTest 通过，1 项可选 Redis 契约跳过；数据库连接中断后能恢复，ACK 后进程重启同键重试未重复写入。混合负载下 ACK 尾延迟仍需优化；隔离结果不代表正式容量或生产级可用性。

## 主要版本

| 分支 | 阶段 | 主要内容 |
|---|---|---|
| [Function3.2](https://github.com/aurora-deng/web/tree/Function3.2) | 复现与诊断 | 社区模块拆分、隔离复现、连接池恢复、ACK 分段测量 |
| [Function3.1](https://github.com/aurora-deng/web/tree/Function3.1) | 界面与治理 | 统一水岸主题、三分钟撤回、语音、AI 与社区体验 |
| [Function2.1](https://github.com/aurora-deng/web/tree/Function2.1) | 聊天闭环 | 历史分页、断线补发、幂等重试与管理员撤回 |
| [`Function1.0`](https://github.com/aurora-deng/web/tree/Function1.0) | Phase 11 | 账号、数据库、好友、私聊/群聊、离线消息、AI Provider 与完整网页端 |
| [`test9.1`](https://github.com/aurora-deng/web/tree/test9.1) | Phase 10 | HTTP/gRPC 热路径、生命周期与性能优化，是 Function1.0 的直接基线 |
| [`test9.0`](https://github.com/aurora-deng/web/tree/test9.0) | Phase 9 | gRPC Callback API、生产化护栏与故障测试 |
| [`test8.0`](https://github.com/aurora-deng/web/tree/test8.0) | Phase 6–7 | HTTP/2、TLS、ALPN、Stream 协程与教学版 HTTP/2 |
| [`test7.0`](https://github.com/aurora-deng/web/tree/test7.0) | Phase 5 | SSE 长响应、事件编码与会话生命周期 |
| [`test6.3`](https://github.com/aurora-deng/web/tree/test6.3) | WebSocket 阶段 | WebSocket 协议栈、会话管理和异步发送基础 |
