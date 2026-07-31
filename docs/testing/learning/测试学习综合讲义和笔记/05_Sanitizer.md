# 05 · Sanitizer（ASan / UBSan / TSan）（详细讲义）

> 功能测试和压测解决的是「对不对」「快不快」。  
> C++ 高性能服务器还有致命问题：**低压力正常，高并发下偶发崩溃或静默损坏**。  
> Sanitizer = 编译器给你的程序**加装运行时监控器**，把隐蔽错误变成明确报告。

---

## 我对 Sanitizer 的理解（笔记）

**核心定义（自己的话）：**

> **Sanitizer = 编译时给程序装上监控器，跑的时候一旦出现越界、野指针、UAF、数据竞争等，立刻明确报错（带位置）。**

和「普通跑崩了再猜」不同：

```text
普通：偶尔崩、难复现
ASan：直接说 heap-use-after-free 在哪一行
TSan：直接说两个线程谁在抢同一块内存
```

**三类（自己记）：**

| 工具 | 我理解成 |
|------|----------|
| ASan | 查地址/内存用错 |
| UBSan | 查未定义行为（如危险溢出） |
| TSan | 查多线程数据竞争 |

**纪律：** ASan 和 TSan 一般分开构建，不要混开。  
**和压测关系：** 压力把坏时序逼出来，Sanitizer 负责把 Bug 钉死。

---

## 1. 用白话理解 Sanitizer

### 普通运行

```text
你的代码里：delete 了指针又用
  → 有时崩，有时「碰巧」没崩
  → 极难复现
```

### 开了 Address Sanitizer（ASan）

```text
一跑到那行就报：
ERROR: AddressSanitizer: heap-use-after-free
  以及文件名:行号
```

像给内存访问装了「电子围栏」。

---

## 2. 为什么压测之后更需要它？

低压力：执行顺序「温和」，坏时序不容易撞上。  
高压力：`wrk` 上万连接，线程交错，更容易出现：

```text
线程 A：closeConnection → delete conn
线程 B：还在 onRead → conn->parse()
→ Use After Free
```

所以工程顺序常常是：

```text
功能测试 → 压测（制造压力）→ Sanitizer（在压力下抓隐藏 Bug）→ 修复 → 再压测
```

---

## 3. 三类 Sanitizer，各管什么？

### 3.1 ASan —— Address Sanitizer（地址）

查：

| 问题 | 例子 |
|------|------|
| 越界 | `arr[20]` 但只有 10 个元素 |
| Use After Free | `delete p; *p;` |
| Double Free | `free` 两次 |

对你这种 Connection / Buffer 项目，**UAF 是头号敌人**。

### 3.2 UBSan —— Undefined Behavior Sanitizer

查「C++ 没定义该怎么做」的行为，例如：

- `INT_MAX + 1` 有符号溢出  
- 非法类型转换  
- 空指针解引用（部分情况）  

### 3.3 TSan —— Thread Sanitizer（线程）

查**数据竞争**：两个线程同时访问同一内存，至少一个在写，且没有同步（锁/原子）。

```text
线程1：conn->state = 1;
线程2：if (conn->state) ...
没有 mutex / atomic → TSan: data race
```

你的 MainReactor / SubReactor / Worker / Timer **正好是 TSan 主战场**。

---

## 4. 关键纪律：ASan 与 TSan 不要混开

```text
❌  -fsanitize=address,thread   （一般不这么干，运行时冲突）
✅  build-asan/  只开 ASan
✅  build-tsan/  只开 TSan
```

---

## 5. CMake 教学参考

```cmake
option(ENABLE_ASAN "Enable Address Sanitizer" OFF)

if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()
```

`-fno-omit-frame-pointer`：保留栈帧，报错时才能看到清晰调用栈，否则可能一堆 `??`。

TSan / UBSan 同理换 sanitize 名字。  
本仓具体开关名以 CMake / `run_tests.sh` 为准（例如 `WEBSERVER_ENABLE_ASAN`）。

建议目录隔离：

```text
build/          普通
build-asan/
build-tsan/
```

---

## 6. 最小「故意犯错」实验（建立直觉）

```cpp
int* p = new int(10);
delete p;
std::cout << *p;  // UAF
```

ASan 构建运行后，应看到 `heap-use-after-free`。  
先在玩具程序确认工具真的工作，再拿去砸 WebServer。

---

## 7. 对你的 WebServer：高概率爆点（对照自查）

### （1）Connection + 回调捕获 `this`

```cpp
channel->setCallback([this]{ handleRead(); });
```

若 Connection 已销毁、Channel/事件仍在 → 经典 UAF。

### （2）Buffer 与 `string_view`

view 指向 buffer 内部 → buffer 扩容搬家 → view 悬空。

### （3）Timer 握着裸 `Connection*`

连接关了，定时器还到期 → 打到僵尸对象。  
（本仓有 zombie 相关设计思想，单测/ASan 用来验证「关了不会炸」。）

### （4）ThreadPool / Executor 任务

`pool.submit([this]{ process(); })`，对象先毁、任务后跑。

### （5）Worker 直接碰 socket / 直接 `resume` 协程

协程与 fd 属于 Reactor 线程 → 必须 Completion 回传再在属主线程恢复（第三阶段 Completion 模型）。

---

## 8. 推荐实战姿势

```text
① 普通 Debug：GoogleTest + 黑盒
② ASan 构建：再跑黑盒 + 中等 wrk（制造压力）
③ TSan 构建：并发黑盒 / 中等压力（先别直接万级）
④ 修干净 → Release 再 Benchmark 对比基线
```

修改 Connection 池、唤醒路径后，把上面四步当成清单。

---

## 9. 本章小结

1. Sanitizer 把「偶发玄学崩溃」变成「带行号的报告」。  
2. ASan 管地址，TSan 管竞争，UBSan 管未定义行为；**分构建**。  
3. 和压测组合才容易打出生命周期/并发 Bug。  
4. 你的项目优先盯：Connection 回调、Buffer view、Timer、Executor 跨线程。

下一章：[06_故障注入.md](06_故障注入.md)
