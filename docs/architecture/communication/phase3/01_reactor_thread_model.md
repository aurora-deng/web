# 第三阶段重要知识点 · 01 Reactor 线程模型

> **标记**：第三阶段重要知识点  
> **对应代码**：`SubReactor`、`ReactorGroup`、`pendingFds`、`conns`

---

## 1. 你现在的拓扑

```text
              acceptor（ServerRuntime.acceptLoop）
                         |
                    ReactorGroup
          +--------------+--------------+
          |              |              |
     SubReactor0    SubReactor1    SubReactor2
     (独立线程)      (独立线程)      (独立线程)
          |
     epoll / conns / TimerWheel / CoroutineScheduler
```

关键词：**one thread one reactor**（一个线程独占一个 Reactor）。

---

## 2. 为什么必须这样？

每个 SubReactor 持有：

```text
unordered_map<int, unique_ptr<Connection>> conns;
```

以及对该 map 的：`epoll_ctl`、TimerWheel、协程调度。

若线程 A 拿着 `conns[fd]`，线程 B `erase(fd)` → **use-after-free**。

因此原则：

> **只有拥有者线程修改该 Reactor 的连接与 epoll 状态。**

---

## 3. 跨线程只能「投递消息」

主线程 accept 之后：

```text
accept(fd)
  → pendingFds.push(fd)
  → eventfd 通知目标 SubReactor
  → SubReactor 线程 processPendingFds()
  → 在本线程创建 Connection、加入 conns
```

这叫 **message passing**，不是「随便哪个线程 new Connection」。

`eventfd` ≈ Actor 的邮箱门铃。

---

## 4. 与业界的对应

| 本仓概念 | 常见叫法 |
|----------|----------|
| SubReactor 线程 | EventLoop / Reactor / Actor |
| pendingFds + eventfd | 跨线程任务投递 |
| conns 线程独占 | Thread affinity（连接亲和） |

同构思想可见于：Netty EventLoop、Tokio runtime（单线程执行器变体）、Seastar 等。

---

## 5. 必须记住的一句话

**Connection 只能属于一个 SubReactor；别人改它只能通过「投递给拥有者线程」。**
