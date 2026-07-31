# 第三阶段重要知识点 · 10 通向到第四阶段（只讲桥）

> **标记**：第三阶段重要知识点 · 过渡  
> **纪律**：本文只解释「为何不能直接拿 HTTP/1.1 Session 硬套 WS/HTTP2」；**本仓库约定仍停止实现** WS/SSE/HTTP2/gRPC。

---

## 1. 第三阶段交给你的积木

```text
Connection（TCP + 缓冲 + Reactor 亲和）
  └── Session 状态机（读/执行/写循环）
        ├── Codec / Parser
        ├── Executor + Completion
        └── Sender
```

第四阶段换的是 **会话语义与帧模型**，不是推倒 Runtime/Reactor。

---

## 2. 为何 HTTP/1.1 Session 不能直接当 WebSocket

| HTTP/1.1 Session | WebSocket |
|------------------|-----------|
| 请求/响应轮次清晰 | 握手后变成**双向消息流** |
| 协程常按「读满一请求→执行→写完响应」 | 需要长期读事件 + 主动推送，控制权从「短轮次」变为「连接占用」 |
| 可用 Keep-Alive 多请求 | 同一连接上是帧，不是多个独立 HTTP 事务 |

要点：WS 往往要**夺取**连接上的读循环控制权，而不是套一层 `GET` handler。

---

## 3. SSE

仍偏 HTTP 长响应（chunked/流式写），依赖长连接与写侧不堵死；与「短请求-短响应」Session 节奏不同，但比 WS/HTTP2 更接近现有 Sender 流式路径。

---

## 4. 为何 HTTP/2 需要 Stream 层

| HTTP/1.1 | HTTP/2 |
|----------|--------|
| 一连接上事务多靠串行/pipeline | **多路复用**：一连接多 Stream |
| 顺序响应约束强 | 帧交错；流级优先级/窗口 |
| Connection≈一条事务管道 | Connection + **Stream 状态机** + 帧 Codec |

没有「Connection 所有权 + 背压 + 完成回传」的底子，上 Stream 极易生命周期与线程安全失控。

---

## 5. gRPC

建立在 HTTP/2（或类 HTTP/2）帧与流之上，再加消息边界与桩代码。依赖：帧层、流生命周期、取消与背压——正是 03/06/07/08 的延伸。

---

## 6. 过渡期你该做什么

1. 用 [09_acceptance_checklist.md](09_acceptance_checklist.md) 自检第三阶段。  
2. 继续测试学习（不变量），而不是开协议实现。  
3. 收口文档：[../../closeout/CLOSEOUT_NARRATIVE.md](../../closeout/CLOSEOUT_NARRATIVE.md) 停扩约定仍有效。  
4. 若转向机器人：把 Reactor/背压/完成回传思想迁移到 ROS2/DDS，而非在本仓堆 HTTP2。
