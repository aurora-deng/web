# 第三阶段重要知识点 · 目录

> **标记**：第三阶段重要知识点（架构能力补齐，非第四阶段协议扩张）  
> **定位**：代码对象化约 70%～80% 已落地；本目录补齐「为什么这样拆、控制流是什么、如何验收」。  
> **纪律**：先吃透这里，**不要急着** WebSocket / HTTP2 / gRPC。

## 与总架构文档关系

| 文档 | 用途 |
|------|------|
| [../ARCHITECTURE.md](../ARCHITECTURE.md) | 当前代码行为总览 |
| [../phase3_architecture_upgrade.md](../phase3_architecture_upgrade.md) | 第三阶段升级过程记录 |
| [../request_flow.md](../request_flow.md) | 请求穿过系统的路径 |
| **本目录** | 第三阶段**必须掌握的思想与验收** |

## 阅读顺序（建议）

1. [01_reactor_thread_model.md](01_reactor_thread_model.md) — one-thread-one-reactor / Actor  
2. [02_runtime_lifecycle.md](02_runtime_lifecycle.md) — ServerRuntime 生命周期  
3. [03_async_runtime.md](03_async_runtime.md) — 协程 + Executor = Async Runtime  
4. [04_session_lifecycle.md](04_session_lifecycle.md) — Session 状态机与 Keep-Alive  
5. [05_pipeline_response_order.md](05_pipeline_response_order.md) — Pipeline 与响应顺序  
6. [06_backpressure.md](06_backpressure.md) — 背压分层  
7. [07_ownership_and_zombie.md](07_ownership_and_zombie.md) — Connection / Session / 协程所有权  
8. [08_completion_model.md](08_completion_model.md) — Completion 回传（跨线程唤醒）  
9. [09_acceptance_checklist.md](09_acceptance_checklist.md) — 第三阶段验收  
10. [10_bridge_to_phase4.md](10_bridge_to_phase4.md) — 过渡到第四阶段（只讲桥，不实现）

## 代码 vs 设计目标（读笔记时对照）

| 主题 | 本仓库现状（约） | 笔记中的「完整体」 |
|------|------------------|-------------------|
| Reactor 独占 conns | ✅ | 同左 |
| pendingFds + eventfd | ✅ | 同左 |
| Session 状态机 + ExecuteAwaiter | ✅ | 同左 |
| pauseByMemory 读背压 | ✅ | 完整体还可含写积压/队列水位 |
| 多请求 RequestQueue / ResponseQueue | △ 当前多为**单连接串行 Session 协程**处理 | 笔记讲清 HTTP/1.1 顺序约束与演进点 |
| zombieWakes | ✅ | 同左 |
| `/slow`+`/fast` 黑盒 | ✅ | 验收项 |

**原则**：思想必须齐；队列模型等按验收与演进需要再动代码，**不为凑名词改协议栈**。
