# 第三阶段重要知识点 · 02 ServerRuntime 生命周期

> **标记**：第三阶段重要知识点  
> **对应代码**：`ServerRuntime`、`main.cpp`

---

## 1. 从「上帝 main」到 Runtime

| 以前 | 现在 |
|------|------|
| main 里 socket / epoll / 线程 / router / parser 全创建 | `ServerRuntime` 统一持有与启停 |
| main 知道一切细节 | main 只注册路由并 `start()` |

```text
main
  → ServerRuntime
       ├─ ReactorGroup
       ├─ Executor（共享）
       ├─ Router / HttpCodec
       ├─ listen + accept loop
       └─ 信号 → requestStop → 释端口 → join
```

这是第三阶段标志：**服务器有了生命周期管理者（RAII / 显式 start-stop）**。

---

## 2. 启动顺序（思想）

```text
start()
  → setupListener（listen + SO_REUSEADDR + epoll）
  → createReactors / start SubReactor 线程
  → acceptLoop（直到 running_=false）
```

---

## 3. 停止顺序（定稿，与收尾一致）

```text
信号 / requestStop
  → running_=false + eventfd 唤醒 accept
  → releaseListener（尽早归还端口）
  → Reactor stop / join
  → 析构时 Executor drain（成员顺序保证）
```

原则：**先停接听，再收尾线程**。

---

## 4. 为何第四阶段也不拆掉 Runtime

HTTP2 / WebSocket / gRPC 仍需要：

- 统一启动停止  
- 共享线程池 / Reactor 组  
- 路由或服务注册入口  

变的是 Session/Codec 层，不是「不要 Runtime」。
