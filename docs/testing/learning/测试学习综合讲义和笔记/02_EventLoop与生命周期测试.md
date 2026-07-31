# 02 · EventLoop / Reactor / Connection 单元测试（详细讲义）

> 这一章是单元测试体系里的**分水岭**。  
> 前面 Buffer / Parser / Router：输入确定、输出确定 → **纯逻辑**。  
> 这里开始：fd、epoll、回调、线程、对象生死 → **事件驱动 + 资源生命周期**。  
> C++ 高性能服务器最严重的 Bug，十有八九出在这一层。

---

## 我对 EventLoop / Reactor 测试的理解（笔记）

前面单测像：

> 输入字符串 → 输出结构体，对就算过。

这里不一样。我的理解是：

> **测的是「事件来了会不会正确回调」以及「对象关了之后还会不会被误用」——重点是生命周期和线程边界，不只是返回值。**

所以：

- 要用 pipe / 假事件模拟「可读了」  
- `loop()` 必须能 `quit`，否则测试卡死  
- Connection 关掉以后，Timer / 回调 / 别的线程不能再当活人用  
- 单测只能覆盖一部分；万级并发下的 UAF / race 还要靠压测 + TSan  

一句话：

> **GoogleTest-4 是从「测函数」迈向「测事件驱动系统」的门槛。**

---

## 1. 先用白话回顾：Reactor 在干什么？

第三阶段你大概是这种形状：

```text
Main / Acceptor
      │
   新连接 accept
      │
  分给某个 SubReactor
      │
   EventLoop（while + epoll_wait）
      │
   可读/可写事件
      │
   Connection / Session
      │
   HTTP 处理
```

一个 EventLoop 线程里往往挂着很多 Channel（每个 fd 一个）：

```text
        EventLoop 线程
            │
    ┌───────┼───────┐
 Channel1  Channel2  Channel3
 (fd=10)   (fd=11)   (fd=12)
```

原则回忆（第三阶段）：**Connection 只属于一个 SubReactor 线程**，别人不能随便改 `conns` map。

---

## 2. 为什么「逻辑单测过了」还要测 Reactor？

因为很多 Bug **平时不爆，压力或时序一对就炸**：

### Bug1：连接关了，事件还在

```text
客户端关闭
  → 你 delete Connection
  → epoll 里还残留事件 / 回调还持有 this
  → 回调一跑 → 访问已释放对象 → 段错误
```

### Bug2：fd 编号被操作系统复用

```text
旧连接 fd=10 关闭
新连接又拿到 fd=10
但旧事件/旧回调还以为「还是那个 Connection」
→ 对「新 socket」做了「旧逻辑」
```

### Bug3：跨线程

```text
Reactor 线程：conn->send() / 读 buffer
Worker 线程：delete conn
→ data race 或 use-after-free
```

GoogleTest 能覆盖一部分「状态机/回调是否触发」；  
**真正大规模暴露**要靠后面的 TSan + 压测 + 故障断开。但单测必须先把「生命周期意识」练出来。

---

## 3. 测试策略变了：用 Fake Event

不能再只写：

```cpp
Buffer buf;
EXPECT_EQ(...);
```

EventLoop 需要「像真的一样」有可读事件，但又要可控、可退出。常用手段：

| 手段 | 作用 |
|------|------|
| `pipe()` / `socketpair` | 人造一对 fd，写一端 → 读一端变可读 |
| 手动 `handleEvent(EPOLLIN)` | 假装 epoll 已经告诉你「可读了」 |
| `runInLoop` + `quit` | 任务跑完必须退出，否则 `loop()` 永久卡住测试 |

---

## 4. 测 EventLoop：runInLoop（教学样例）

EventLoop 本质常是：

```text
while (!quit) {
  epoll_wait(...);
  处理事件;
  跑就绪任务 / 协程;
}
```

所以测试里**必须能 quit**，否则 CI 会挂死。

```cpp
TEST(EventLoopTest, RunTask) {
    EventLoop loop;
    bool called = false;

    loop.runInLoop([&]() {
        called = true;
        loop.quit();   // 关键：不 quit 就卡死
    });

    loop.loop();
    EXPECT_TRUE(called);
}
```

流程用人话讲：

```text
把任务丢进 loop
  → 启动 loop
  → 执行回调（called=true）
  → quit
  → loop 返回
  → 断言回调确实跑过
```

---

## 5. 测 Channel：用 pipe 模拟「可读」

真实世界：

```text
socket 收到数据 → epoll 返回 EPOLLIN → Channel 调 readCallback
```

测试里用 pipe 模拟「对端写了数据」：

```cpp
TEST(ChannelTest, ReadEvent) {
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    bool called = false;
    Channel channel(pipefd[0]);  // 读端
    channel.setReadCallback([&]() { called = true; });

    // 写端写入 → 读端变为可读
    ASSERT_EQ(write(pipefd[1], "hello", 5), 5);

    channel.handleEvent(EPOLLIN);  // 假装 epoll 通知了

    EXPECT_TRUE(called);

    close(pipefd[0]);
    close(pipefd[1]);
}
```

你在验证的不是 HTTP，而是：**事件到了，回调会不会被正确调用**——这是 Reactor 的地基。

---

## 6. 测 Connection 生命周期（重点思想）

Connection 不是「一个请求对象」，而是「一条 TCP 连接的管家」。

教学向断言示例：

```cpp
TEST(ConnectionTest, Close) {
    Connection conn(/* 测试 fd 或 mock */);

    EXPECT_FALSE(conn.closed());
    conn.close();
    EXPECT_TRUE(conn.closed());
}
```

更重要的是你脑子里要有这张图：

```text
谁拥有 Connection？     → 通常 SubReactor 的 unique_ptr map
谁拥有 Session/协程？   → 另有规则（scheduler / handle）
Timer 还握着指针吗？   → 关闭后回调必须失效或安全忽略
Worker 能直接 delete 吗？→ 一般不能，应回传到属主线程
```

C++ 服务器里 **80% 严重 Bug 不是算法，是生命周期**（`shared_ptr` 乱传、`raw this` 进回调、Timer 打僵尸连接）。  
本仓相关思想：`zombieWakes`、Completion 回传——见第三阶段所有权笔记。

---

## 7. 测 ThreadPool / Executor（靠近 TSan）

```cpp
TEST(ThreadPoolTest, Execute) {
    ThreadPool pool(4);
    std::atomic<int> count{0};

    for (int i = 0; i < 100; ++i) {
        pool.submit([&]() { count.fetch_add(1); });
    }
    pool.wait();  // 按你真实 API：join / drain

    EXPECT_EQ(count.load(), 100);
}
```

### 为什么这里强调 `atomic`？

若写成普通 `int count++`：多线程写同一变量 → **数据竞争**。  
测试可能偶现失败；TSan 会直接报。  
这已经在为后面「压测 + TSan」热身。

### 和本仓强相关的不变量

Worker 里 `sleep(5)`（如 `/slow`）**绝不能**堵死同一个 Reactor 上其它连接的 `/fast`。  
这个最终用黑盒证明更直观，但单测阶段就要建立「IO 线程 vs 业务线程」边界感。

---

## 8. 现在单元测试覆盖到哪了？

```text
tests/unit 心智覆盖：

Buffer / HttpParser / Router     ← 业务逻辑零件
EventLoop / Channel              ← 事件驱动
Connection                       ← 生命周期
ThreadPool                       ← 并发基础
```

---

## 9. GoogleTest 的极限（诚实说）

真正的地狱场景往往是：

```text
10000 连接 × 100 线程 × 随机断开 × 随机超时 × 响应乱序完成
```

单测模拟不全。所以下一站必须升级到：

> **黑盒（真 socket）→ Benchmark（真压力）→ Sanitizer（真仪器）**

---

## 10. 本章小结

1. Reactor 测试测的是**事件有没有正确触发、对象有没有正确生死**。  
2. 用 pipe/socketpair + quit 控制，避免测到一半卡死。  
3. 生命周期意识 > 再写一百个业务断言。  
4. 单测到头了就进黑盒，不要幻想靠 GoogleTest 证明万级并发安全。

下一章：[03_HTTP黑盒测试.md](03_HTTP黑盒测试.md)
