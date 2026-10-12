# 第三阶段重要知识点 · 04 Session 生命周期与 Keep-Alive

> **标记**：第三阶段重要知识点  
> **对应代码**：`HttpSession::run`、`SessionState`

---

## 1. Connection ≠ Request

| 对象 | 含义 |
|------|------|
| **Connection** | 一条 TCP（fd、缓冲、归属哪个 Reactor） |
| **HttpSession** | 该连接上的 HTTP 会话状态机（可多轮请求） |
| **一次 Request/Response** | 一轮 HTTP 交换 |

Keep-Alive 下：**一个 Connection，多轮 Session 循环**（同一协程 `while(true)` 读→执行→写→再读）。

---

## 2. 状态机

```text
READING →（完整请求）→ EXECUTING →（Worker 完成）→ WRITING →（发完）→ READING …
                ↘ 错误/对端关/超限 → CLOSED → co_return
```

不是「来一个请求处理完就关连接」；是 **循环**，除非短连接或错误。

---

## 3. 创建链（本仓大致）

```text
TCP accept → Connection 入 conns
          → HttpSession 绑在 Connection 上
          → session->run() 协程帧交给 CoroutineScheduler
```

协程帧里的 `this` 指向 Session；关闭时必须保证：**先让协程安全退出或登记僵尸唤醒，再拆 Connection**（见 07）。

---

## 4. 每轮请求要 reset 的上下文

Keep-Alive 共用 Session 时，必须清空上一轮：`request` / `response` / `params` / `handled` 等，避免串请求。

---

## 5. 与 Pipeline 的关系

Keep-Alive 允许多请求同连接；**Pipeline** 是「请求可粘在一起到达」。  
当前实现以 **单连接串行协程** 为主（同一时刻处理一个请求的执行段）；读侧可保留缓冲区中的后续请求字节（Parser 流水线用例）。完整 RequestQueue/ResponseQueue 见 05。
