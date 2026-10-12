# 第三阶段重要知识点 · 03 Async Runtime（异步运行时）思想

> **标记**：第三阶段重要知识点  
> **对应代码**：`HttpSession` 协程、`ExecuteAwaiter`、`CoroutineScheduler`、`Executor`

---

## 1. 异步 ≠ 多线程

异步真正解决的是：

> **大量任务如何在少量线程上高效暂停与恢复。**

| 模型 | 问题 |
|------|------|
| 一连接一线程 | 线程爆炸 |
| 纯 Reactor + 同步 handler | handler 一 `sleep` 卡死该 Reactor 上所有连接 |
| Reactor + 协程 + Executor | IO 与阻塞/CPU 业务分离 |

---

## 2. 本仓落到的形态

```text
SubReactor 线程
  HttpSession 协程（状态机）
       |
       | co_await ExecuteAwaiter
       v
  Executor Worker（跑 handler，可 sleep/算）
       |
       | notifyExecuteComplete
       v
  completeQueue + eventfd
       |
       v
  同一 SubReactor 再 resume 协程 → 发响应
```

- **协程**：用户态暂停/恢复会话步骤（遇 `co_await` 让出）。  
- **Executor**：真正可能阻塞或吃 CPU 的业务；**不能替代**「主动 await」。  
- **Scheduler**：ready 队列上的微型 runtime（类似 EventLoop 跑就绪任务）。

---

## 3. 协程解决什么、不解决什么

| 能 | 不能 |
|----|------|
| 把「等 IO / 等业务完成」写成顺序代码 | 自动打断死循环 CPU 计算（没有 await 就不会切换） |

因此：**IO 路径用协程挂起；CPU/阻塞业务进 Executor。**

---

## 4. 与「有协程还要不要线程池」

要。协程不会在纯计算循环里自动让出；Worker 提供并行与隔离。  
Reactor 线程应保持「可调度」：尽量只做 IO、解析、调度、发送。

---

## 5. `/slow` 时序（必会）

```text
Reactor: submit → co_await EXECUTE ──────────────────→ resume → write
              │                                         ▲
Worker:       └── sleep(10) → notify(eventfd+queue) ────┘
```

黑盒 `/slow`+`/fast`：证明 Worker 阻塞不得拖死同 Reactor 其他连接。
