# Function1.0 与 test9.1 对比

> 对比基线：`origin/test9.1`，提交 `92252c34a5058644705bb22390a177ca61f2ba23`
>
> 当前版本：`Function1.0`
> 目标：在 test9.1 的高性能网络与 gRPC 基础上，形成可登录、可持久化、可实时聊天并可接入 AI 的应用闭环。

## 1. 定位变化

test9.1 主要解决“服务器怎样更快、更稳定地处理协议流量”。Function1.0 在此基础上加入 Phase 11
业务层，开始解决“用户进入服务器之后可以完成什么”。可以把二者理解为：

- test9.1 建成了道路、红绿灯和物流调度系统；
- Function1.0 在道路之上建成账号中心、好友系统、聊天室、数据库和 AI 服务站。

原有 HTTP/1.1、WebSocket、SSE、HTTP/2、TLS/ALPN、gRPC Callback、Multi-Reactor、协程调度、
有界出站队列和性能快路径继续保留。

## 2. 功能与架构对比

| 维度 | test9.1 | Function1.0 |
|---|---|---|
| 核心定位 | 高性能多协议服务器与 gRPC 热路径优化 | 在服务器上形成账号、社交、聊天、AI 和网页端完整业务 |
| 用户身份 | 跨协议 HMAC 身份与安全边界 | PostgreSQL 用户、Argon2id 密码、Session Token Hash、Cookie、CSRF、限流与吊销 |
| 数据持久化 | 协议状态和运行指标为主 | PostgreSQL Repository、连接池、事务、迁移和 16 张表 |
| 社交关系 | 无完整好友业务 | 好友申请、接受/拒绝和双向好友关系 |
| 聊天业务 | WebSocket/SSE 协议能力 | 私聊、群聊、成员权限、历史、搜索、ACK、已读与离线恢复 |
| 消息可靠性 | 协议出站队列与连接背压 | `conversation_seq` 排序、`client_message_id` 幂等、消息与 Outbox 同事务提交 |
| 实时通道 | WebSocket 双向帧、SSE 长响应 | HTTP 管理查询、WebSocket 发消息、SSE 通知与 AI Token 分工 |
| AI 接入 | gRPC 教学与 Callback 服务 | `IModelProvider`、Ollama、进程内 ONNX、独立 gRPC ONNX Worker |
| 模型管理 | 无业务模型注册表 | 模型节点、版本、绑定、Adapter 校验、启用、回滚与训练授权骨架 |
| 浏览器端 | 通用静态资源与协议测试 | 原生 HTML/CSS/JS 响应式聊天工作台，已与真实 API/WS/SSE 打通 |
| 依赖策略 | OpenSSL、nghttp2、gRPC 等服务器依赖 | 新依赖全部使用显式 CMake 开关和用户指定前缀，配置过程不自动下载 |
| 文件组织 | 协议与运行时为主 | 新增 `business/domain/ports/storage/transport/ai/training` 边界，并按组件归类 |

## 3. Function1.0 请求链

```text
Browser
  ├─ HTTP  ──→ Phase11AuthHttp / Phase11SocialChatHttp
  ├─ WS    ──→ Phase11Realtime ──→ ChatService
  └─ SSE   ←─ OutboxDispatcher / AI token stream
                         │
                         ↓
                 BusinessInterfaces
                         │
       ┌─────────────────┼──────────────────┐
       ↓                 ↓                  ↓
 Auth/Social/Chat    AiChatService    ModelRegistryService
       │                 │                  │
       ↓                 ↓                  ↓
 Repository Ports    IModelProvider     Model Repository
       │          ┌──────┼────────┐          │
       ↓          ↓      ↓        ↓          ↓
 PostgreSQL    Ollama  ONNX   gRPC Worker  PostgreSQL
```

网络线程只负责连接和协议推进。数据库操作进入专用执行器，模型生成进入有界 Worker 或独立进程，
避免把 Reactor 当成执行慢查询和推理的工作线程。

## 4. 业务闭环

### 4.1 账号和会话

```text
注册 → Argon2id 散列 → users
登录 → 随机 Session Token → 数据库只保存 Token Hash
浏览器 ← HttpOnly / SameSite Cookie + CSRF Token
退出或过期 → Session 吊销 / 拒绝恢复
```

### 4.2 离线消息

```text
发送者 WebSocket 提交 chat.send
              ↓
校验成员权限和 client_message_id
              ↓
同一事务写入 messages + outbox_events
              ↓
发送者收到 ACK；在线成员实时收到消息
              ↓
离线成员下次登录按 conversation_seq 恢复
```

好友或群成员是否在线不影响消息入库。在线状态只决定能否立刻推送，不决定消息能否发送。

### 4.3 AI 消息

AI 账号与模型节点/版本绑定。用户 Prompt 仍进入普通消息体系，`ModelRouter` 再选择 Ollama、进程内
ONNX 或独立 gRPC Worker。增量 Token 通过 SSE 返回，完成后的 AI 消息与模型追踪信息持久化；
取消或模型故障不会阻塞普通聊天 Reactor。

## 5. 前端变化

`frontend/phase11/` 新增完整的登录和聊天页面：

- 注册、登录、退出、Session 恢复和个人资料；
- 好友列表、好友申请和角色资料；
- 私聊、群聊、群成员、历史消息和会话内搜索；
- WebSocket 消息、SSE 通知、AI 流式生成与取消；
- 离线投递说明、错误状态、重连状态和移动端布局。

演示数据脚本会通过正式业务 API 创建产品、前端、后端、测试、运维、设计、数据、安全等角色账号，
再完成双向好友关系和群聊初始化。密码只从仓库外文件读取，私密账号清单由 `.gitignore` 排除。

## 6. 目录和无用文件清理

Function1.0 将历史文件按职责迁移：

| 原位置 | 新位置/处理 |
|---|---|
| `static/phase11/` | `frontend/phase11/` |
| `fuzz/`、`fuzz-corpus/` | `tests/fuzz/` |
| `log/logger/` | `server/observability/logger/` |
| `benchmark-results/` | `artifacts/benchmarks/` |
| `test-output/` | `artifacts/test-results/` |
| 根目录 `flame.svg` | `artifacts/profiling/flame.svg` |
| `preview.cpp` | `docs/archive/legacy_subreactor_preview.cpp` |
| 根目录旧学习讲义 | `docs/testing/learning/legacy-guide/` |
| 空的协议/应用占位目录 | 删除 |
| 临时传输压缩包、`__pycache__`、`.pyc` | 删除并由 `.gitignore` 阻止再次提交 |

这些移动同时更新了 CMake、include、脚本和文档引用。测试报告和性能证据仍保留在 `artifacts/`，
因为它们用于说明历史验证结果，不属于无用缓存。

## 7. 构建开关

Function1.0 新增以下可选项：

```text
WEBSERVER_ENABLE_PHASE11
WEBSERVER_ENABLE_POSTGRES
WEBSERVER_ENABLE_SODIUM
WEBSERVER_ENABLE_CURL_CLIENT
WEBSERVER_ENABLE_ONNX_GENAI
```

缺少外部依赖时仍可构建内存 Repository 和业务核心；启用适配器时必须提供已经安装好的路径，
CMake 不会调用 `FetchContent` 或包管理器下载依赖。

## 8. 验证结果

- 2026-10-06 提交前 Linux Release 全量 CTest：**93/93**（PostgreSQL 契约测试使用隔离的
  `phase11_test` 数据库）；此前 Debug、ASan+LSan+UBSan 两套记录均为 **92/92**；
- PostgreSQL、密码凭据、好友/聊天、Outbox、Ollama、ONNX 和 gRPC Provider 测试通过；
- WebSocket → PostgreSQL → ONNX/gRPC Worker → SSE → 消息持久化整链通过；
- 当前前端实际显示角色好友，点击好友可创建私聊，浏览器控制台无错误；
- 演示数据脚本首次建立用户/好友/群聊，第二次运行新增数均为 0，幂等检查通过；
- Phase 10 `/fast` 性能快路径仍保留，完整 gRPC+ONNX Release 配置历史五轮中位数为
  **56,929.50 QPS**，相对 test9.1 记录值提升 15.10%，错误数为 0。

测试数字只适用于记录中的虚拟机、编译参数和依赖组合，不能直接推导为其他机器上的吞吐保证。

## 9. 当前边界

Function1.0 已适合继续进行功能学习、单机部署和预生产验证，但仍不能直接宣称可用于公网关键业务。
多实例 Presence、分布式事件总线、跨节点会话路由、备份恢复演练、正式密钥管理、监控告警、容量规划、
灾难恢复和完整安全审计仍需在部署阶段补齐。真实模型质量也需要授权数据、评估集和持续评测证明。

## 10. 代码与文档入口

1. [Phase 11 总体架构](phase11.md)
2. [项目目录分层](project-layout.md)
3. [数据库表结构](../database/README.md)
4. [依赖审批与安装边界](phase11-dependency-gates.md)
5. [测试指南](../testing/TESTING.md)
6. [性能指南](../performance/BENCHMARK.md)
