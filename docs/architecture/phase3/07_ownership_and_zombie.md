# 第三阶段重要知识点 · 07 Connection / Session / Coroutine 所有权

> **标记**：第三阶段重要知识点（进入第四阶段前的桥梁）  
> **对应代码**：`Connection`+`HttpSession`、`scheduler`、`fd_close`、`zombieWakes`

---

## 1. 三者关系

```text
Connection（fd、缓冲、归属 Reactor）
    └── HttpSession（HTTP 状态机，协程 this）
            └── Coroutine frame（由 CoroutineScheduler 调度/收割）
```

- Connection **拥有** Session（如 `unique_ptr`）。  
- 协程帧里的 `this` **引用** Session。  
- Scheduler **拥有/收养** handle，统一 resume/destroy，避免乱销毁。

避免：对象拥有协程、协程又强持对象形成无法拆的环（滥用 `shared_ptr` 互指）。

---

## 2. 为何不能 Worker 直接 `handle.resume()`

协程与 `conns` 属于 **SubReactor 线程**。Worker 是另一线程 → 直接 resume = 数据竞争 / 未定义行为。  
必须：**Worker 只投递完成消息 → 拥有者 Reactor 再 resume**（见 08）。

---

## 3. 关闭时的安全路径

`fd_close` 时：

- 标记 closed，移出 epoll，从 `conns` 删除。  
- 若协程正卡在 **EXECUTE**（Worker 还没完）：把 handle 记入 **`zombieWakes[connId]`**。  
- Worker 结束后 `processComplete`：conn 已不在 → 走 zombie 表调度协程 → 协程 `getConn()==nullptr` → **安静 co_return**。

这就是「僵尸协程」：**连接没了，但执行完成通知仍能安全收尾**。

---

## 4. `connId` 的作用

fd 会复用。完成通知带 **连接世代 id**，防止唤醒「同 fd 新连接」的会话。

---

## 5. 设计原则摘要

| 原则 | 做法 |
|------|------|
| 单线程改 conns | 仅所属 SubReactor |
| 跨线程只投递 | pendingFds / completeQueue + eventfd |
| 挂起后重验 | 每次 `co_await` 后 `getConn()` |
| 关连接不丢协程 | zombieWakes + connId |
| 少用 shared_ptr 环 | Scheduler 管柄，Session 不靠环保活乱套 |

第四阶段长连接（WS/HTTP2 stream）更依赖这一套，否则「连几小时」必踩生命周期坑。
