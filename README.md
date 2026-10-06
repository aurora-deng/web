# web · C++20 高级 Web 服务器学习项目

这是一个面向 Linux 的 C++20 Web 服务器学习项目，从 `epoll + Multi-Reactor + 协程`
逐步演进到 HTTP/1.1、WebSocket、SSE、HTTP/2、TLS/ALPN、gRPC，以及带账号、好友、
聊天、PostgreSQL 持久化和 AI Provider 的完整应用层。

> **当前最新功能版本：[`Function1.0`](https://github.com/aurora-deng/web/tree/Function1.0)**
>
> `main` 用作仓库首页和版本导航。最新可运行代码、完整 Phase 11 文档与测试位于
> `Function1.0` 分支。

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey)
![Build](https://img.shields.io/badge/build-CMake-green)
![Protocols](https://img.shields.io/badge/protocols-HTTP%20%7C%20WebSocket%20%7C%20SSE%20%7C%20HTTP%2F2-orange)

## 最新版本能做什么

`Function1.0` 在高性能网络服务器上增加了真实业务闭环：

- 注册、登录、退出、Session 恢复、CSRF 防护与登录限流；
- PostgreSQL Repository、连接池、迁移、事务 Outbox 和消息持久化；
- 好友申请、好友列表、私聊、群聊、成员权限、历史消息与搜索；
- WebSocket 双向消息、SSE 通知、ACK、已读和离线消息恢复；
- Ollama API、进程内 ONNX 和独立 gRPC ONNX Worker 三类 AI Provider；
- 模型注册、版本、Adapter 校验、启用/回滚和训练数据授权骨架；
- 原生 HTML/CSS/JavaScript 响应式聊天界面，已与 HTTP、WebSocket、SSE 接口打通。

核心请求关系：

```text
Browser
  ├─ HTTP ─────→ Auth / Social / Chat API
  ├─ WebSocket → 实时消息与 ACK
  └─ SSE ←───── 通知与 AI Token
                     │
                     ↓
               Phase 11 业务层
          ┌──────────┼──────────┐
          ↓          ↓          ↓
     PostgreSQL   Outbox    AI Provider
                         ┌────┼─────┐
                         ↓    ↓     ↓
                      Ollama ONNX  gRPC Worker
```

## 主要版本

| 分支 | 阶段 | 主要内容 |
|---|---|---|
| [`Function1.0`](https://github.com/aurora-deng/web/tree/Function1.0) | Phase 11 | 账号、数据库、好友、私聊/群聊、离线消息、AI Provider 与完整网页端 |
| [`test9.1`](https://github.com/aurora-deng/web/tree/test9.1) | Phase 10 | HTTP/gRPC 热路径、生命周期与性能优化，是 Function1.0 的直接基线 |
| [`test9.0`](https://github.com/aurora-deng/web/tree/test9.0) | Phase 9 | gRPC Callback API、生产化护栏与故障测试 |
| [`test8.0`](https://github.com/aurora-deng/web/tree/test8.0) | Phase 6–7 | HTTP/2、TLS、ALPN、Stream 协程与教学版 HTTP/2 |
| [`test7.0`](https://github.com/aurora-deng/web/tree/test7.0) | Phase 5 | SSE 长响应、事件编码与会话生命周期 |
| [`test6.3`](https://github.com/aurora-deng/web/tree/test6.3) | WebSocket 阶段 | WebSocket 协议栈、会话管理和异步发送基础 |

完整差异见：

- [Function1.0 与 test9.1 对比](https://github.com/aurora-deng/web/blob/Function1.0/docs/architecture/function1.0-vs-test9.1.md)
- [Phase 11 总体架构与学习文档](https://github.com/aurora-deng/web/blob/Function1.0/docs/architecture/phase11.md)
- [项目目录分层](https://github.com/aurora-deng/web/blob/Function1.0/docs/architecture/project-layout.md)
- [PostgreSQL 表结构和业务关系](https://github.com/aurora-deng/web/blob/Function1.0/docs/database/README.md)
- [测试指南](https://github.com/aurora-deng/web/blob/Function1.0/docs/testing/TESTING.md)

## 获取最新功能版本

```bash
git clone https://github.com/aurora-deng/web.git
cd web
git switch Function1.0
```

项目面向 Linux 构建。Phase 11 的 PostgreSQL、libsodium、libcurl、gRPC、ONNX Runtime
等依赖均通过显式 CMake 开关接入，配置过程不会自动下载依赖。依赖版本、安装边界、构建参数和
启动方式请以 [Phase 11 文档](https://github.com/aurora-deng/web/blob/Function1.0/docs/architecture/phase11.md)
为准。

## 验证状态

- 2026-10-06，`Function1.0` 的 Linux Release 构建成功；
- 全量 CTest **93/93** 通过，PostgreSQL 契约测试使用独立 `phase11_test` 数据库；
- HTTP、WebSocket、SSE、TLS、gRPC、Phase 11 业务核心和 AI Provider 测试均通过；
- 服务重启后 `/phase11/` 页面可访问；
- 模型文件、数据库数据、口令、Token 密钥和本地账号清单不提交到 Git。

这些结果用于说明指定虚拟机和依赖组合下的验证状态，不代表无需容量评估、安全审计、监控、
备份恢复和多节点设计即可直接承载公网关键业务。

## 学习路线

建议按下面的顺序阅读和动手：

1. `epoll`、Reactor、连接生命周期与 C++20 协程；
2. HTTP/1.1 增量解析、路由、响应和统一出站队列；
3. WebSocket 握手、帧、消息重组、ACK 与会话管理；
4. SSE 长响应、断线重连和事件游标；
5. HTTP/2 Frame、HPACK、Stream、流控与调度；
6. TLS 握手、证书校验和 ALPN 协议选择；
7. gRPC Callback、流式 RPC、取消与生命周期；
8. Phase 11 的业务层、Repository、事务 Outbox、实时聊天和 AI 推理适配。

项目的定位是可运行、可测试、可逐层拆解的学习工程。阅读时先沿一次请求的完整路径理解职责，
再进入某个 Parser、Session、Service 或 Repository 的局部实现。
