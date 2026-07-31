# 第三阶段重要知识点 · 08 Completion 回传模型

> **标记**：第三阶段重要知识点  
> **对应代码**：`Executor::submit`、`notifyExecuteComplete`、`processComplete`、`wakeExecuteCoroutine`

---

## 1. 问题

业务在 Worker 跑完后，**谁**继续发 socket？

错误：Worker 里 `write(fd)` 或 `handle.resume()` → 破坏 Reactor 独占。

正确：**完成通知回传拥有者线程**，由它 resume 协程并 Sender 发送。

---

## 2. 流程

```text
Reactor 线程                          Worker 线程
─────────────                        ───────────
submit(task)
co_await ExecuteAwaiter
  （登记 EXECUTE，不挂 epoll）
                                     跑 handler
                                     （异常 → 填 500，勿让线程池 terminate）
                                     notifyExecuteComplete(fd, connId)
                                       → completeQueue.push
                                       → eventfd write
epoll 醒来
processComplete()
  → 匹配 connId → wakeExecuteCoroutine
  → 或 zombieWakes
resume 会话协程
buildHeader + send
```

---

## 3. 关键结构

| 构件 | 作用 |
|------|------|
| `completeQueue` + mutex | 跨线程无锁队列投递 |
| `eventfd` | 打断 `epoll_wait` |
| `ExecuteAwaiter` | 会话侧「等业务完成」的 await 点 |
| `connId` / zombie | 关连接后仍安全收尾 |

---

## 4. 错误传播（与 Completion 一起记）

| 场景 | 期望 |
|------|------|
| handler `throw` | Worker 捕获 → 500 响应路径，**进程不崩** |
| `submit` 失败（队列满） | 503，通常不 keep-alive |
| 解析错误 / 超限 | 关连接（当前策略），会话 CLOSED |

---

## 5. 一句话

**Executor 只产结果；Reactor 只碰 socket 与协程恢复。** Completion 是两者之间的螺纹。
