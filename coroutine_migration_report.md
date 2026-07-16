# C++20 Coroutine Reactor Server 迁移审计报告

> 审计范围：`E:\虚拟机高级web开发\web`  
> 基线：事件驱动 + 线程池 HTTP 服务器 → C++20 协程 Reactor  
> 审计日期：2026-07-16  
> 状态：**迁移约 60–70%，当前无法稳定编译/运行**

---

## 目录

- [〇、迁移总览与完成度](#〇迁移总览与完成度)
- [一、原报告勘误](#一原报告勘误)
- [二、迁移错误清单（修订+增补）](#二迁移错误清单修订增补)
- [三、修改建议（按优先级）](#三修改建议按优先级)
- [四、后续推荐发展路线](#四后续推荐发展路线)
- [五、关键文件索引](#五关键文件索引)

---

## 〇、迁移总览与完成度

### 0.1 目标架构

```
主 Reactor (main.cpp)
  accept → round-robin 分发 fd
       ↓
SubReactor × N（每线程独立 epoll）
  processPendingFds → 创建 Connection + HttpSession
       ↓
HttpSession::run()  每连接一协程
  co_await ReadAwaiter  → EPOLLIN  → wakeReadCoroutine  → scheduler.add
  createResponse()      → Router 同步执行（当前仍阻塞 Reactor 线程）
  co_await WriteAwaiter → EPOLLOUT → wakeWriteCoroutine → scheduler.add
       ↓
CoroutineScheduler::runReady()  每轮 epoll 后批量 resume
```

### 0.2 迁移完成度对照

| 模块 | 状态 | 说明 |
|------|------|------|
| 主 Reactor accept 分发 | ✅ 保留 | `pendingFds` + `eventfd` 跨线程投递正确 |
| SubReactor epoll 循环 | ⚠️ 半完成 | 事件只唤醒协程，不再直接 recv/send |
| 每连接协程 `HttpSession::run` | ❌ 逻辑错误 | 读循环语义颠倒 + 死循环 |
| `ReadAwaiter` / `WriteAwaiter` | ⚠️ 半完成 | 双重注册；`await_suspend` 返回 void |
| `CoroutineScheduler` | ⚠️ 方向已确定、实现未闭环 | 采用 `suspend_always`；已加入初步去重，但销毁前未清理观察句柄 |
| 线程池处理业务 | ❌ 已拆除未补齐 | `router.handle` 同步跑在 Reactor；`ThreadPool pool` 未定义 |
| Pipeline / pendingResponses | ❌ 已删除字段 | 宏与注释残留，背压半成品 |
| FileCache mmap 预热 | ❌ 链接失败 | 依赖全局 `pool`，`main` 未实例化 |
| 构建系统 | ⚠️ | CMake 重复源文件、缺 `project()` |

### 0.3 一句话结论

协程骨架（`Task` / `AWaiter` / `HttpSession` / `wake*`）已搭好，但存在**多处编译阻断**与**运行时 UB**；必须先统一协程生命周期策略，再修 `HttpSession::run` 读循环，才能进入压测阶段。

### 0.4 已确定的生命周期决策

本项目明确采用：

```cpp
std::suspend_always initial_suspend() noexcept;
std::suspend_always final_suspend() noexcept;
```

不再保留 `final_suspend = std::suspend_never` 备选路线。其生命周期契约为：

1. `Task` 创建时持有协程帧；
2. `task.release()` 后，所有权转移给 `CoroutineScheduler`；
3. `readyQueue`、`waiting`、`CoroutineContext::handle` 只保存**非拥有型观察句柄**；
4. 协程 `co_return` 后停在 `final_suspend`，帧仍有效且 `h.done()==true`；
5. 只有调度器可以调用 `h.destroy()`；
6. 调度器销毁前必须清除 `waiting`、`scheduled` 和 `CoroutineContext::handle` 中指向该帧的引用；
7. 销毁后禁止再执行 `done()`、`resume()`、`address()` 或重复 `destroy()`。

因此，当前真正缺失的不是 `final_suspend` 选择，而是一个可验证的**单一所有权登记与完成清理协议**。

---

## 一、原报告勘误

对既有 `coroutine_migration_report.md`（E01–E20）的纠偏，避免按错误建议改坏：

| 原编号 | 原结论 | 勘误 |
|--------|--------|------|
| E01 | 必须改为 `suspend_never` | 已否决。项目确定使用 `suspend_always`，由调度器在完成清理后统一 `destroy()`。源码中所有 `suspend_never` 注释都应删除或改正。 |
| E02 | `if(!h)` 永远不为空 | 基本正确：`coroutine_handle` 的 `operator bool` 只检查地址非空，不能判断帧是否仍有效。采用 `suspend_always` 后，`resume → done → cleanup → destroy` 是确定流程。 |
| E04/E05 | 「状态无关，有 handle 就唤醒」 | **不建议**。EPOLLIN 不应唤醒正在等写的协程。真正的 bug 是：`wakeRead` 唤醒后未将 `state` 置为 `NONE`（与 `wakeWrite` 不一致），导致去重失效。 |
| E11 | 描述为 double free | 实际更严重：`afterSend` 在**正在运行的协程**里 `fd_close(fromCoroutine=false)`，会把**当前协程 handle 再次入队**，随后 `resume` 运行中/已结束帧 → **UB**。 |
| E13 | 只提返回 bool | 补充：`await_ready()==true`（连接已关）时不会进 `await_suspend`；危险窗口在 ready→suspend 之间。必须用 `bool await_suspend` 返回 `false` 取消挂起。 |
| E16 | 仅构造参数不匹配 | 补充：`HttpSession.h` 使用 `AwaitType` 却未包含定义头；`readRequest()` 空函数体无法编译。 |
| E18 | 建议恢复 `pendingResponses` 检查 | 当前 `Connection` **已无**该字段。应二选一：恢复 pipeline 队列，或删除宏/注释，不要假装检查仍存在。 |

---

## 二、迁移错误清单（修订+增补）

### 严重级别说明

- **P0**：编译失败 / 必现崩溃 / UB  
- **P1**：逻辑错误导致功能失效或泄漏  
- **P2**：架构债、死代码、可维护性  

---

### E01: `Task.h` 注释与已确定的 `suspend_always` 策略不一致 【P0】

**文件**: [Task.h](server/CoroutineScheduler/Task.h)

- `Task<void>` 与 `Task<T>` 的实际实现已经是 `suspend_always`
- `Task.h` 文件头仍声称协程会自动销毁
- `fd_close` 注释仍按 `suspend_never` 描述
- `runReady` 的 `done() + destroy()` 才与已选策略一致

**影响**: 两套假设并存时，必然出现「漏 destroy」或「double destroy / 访问已释放帧」。

**确定修复**：保持 `suspend_always`，删除另一套语义：

```cpp
std::suspend_always final_suspend() noexcept
{
    return {};
}
```

`co_return` 后帧停在 final suspend；调度器确认 `done()` 后先清理所有观察句柄，再唯一一次 `destroy()`。

---

### E02: `CoroutineScheduler::runReady()` 与策略耦合错误 【P0】

**文件**: [CoroutineScheduler.cpp](server/CoroutineScheduler/CoroutineScheduler.cpp)

```cpp
h.resume();
if(!h) continue;      // 无效：destroy 后地址仍可能非空
if (h.done()) {
    h.destroy();      // 仅在 suspend_always 下合法
}
```

**修复**: 固定执行 `suspend_always` 完成路径，并增加空 handle 防护：

```cpp
void CoroutineScheduler::runReady()
{
    while (!readyQueue.empty())
    {
        auto h = readyQueue.front();
        readyQueue.pop();
        if (!h) continue;
        scheduled.erase(h.address());
        h.resume();
        if (h.done()) {
            cleanupReferences(h); // 清 waiting/context/owner 记录
            h.destroy();
        }
    }
}
```

`cleanupReferences` 不能省略。当前源码直接 `destroy()`，而 `CoroutineContext::handle` 仍可能指向该帧，后续 `wake*` 对其调用 `done()` 本身就是 UB。

---

### E03: `scheduler.add()` 去重已部分实现，但未形成完整不变量 【P1】

**文件**: [CoroutineScheduler.cpp](server/CoroutineScheduler/CoroutineScheduler.cpp)

当前源码已新增 `scheduled` 集合，并在 `add()` 入队、`runReady()` 出队时维护，方向正确。但仍有三个问题：

1. `CoroutineScheduler.h` 使用 `std::unordered_set` 却未包含 `<unordered_set>`，会编译失败；
2. `resume()` 直接 `readyQueue.push(...)`，绕过 `add()`，破坏去重；
3. 去重只防止“同时在队列中”，不能防止销毁后的 stale handle 再入队。

**修复**:

```cpp
#include <unordered_set>

void CoroutineScheduler::add(Handle h)
{
    if (!h) return;
    if (!scheduled.insert(h.address()).second) return;
    readyQueue.push(h);
}

void CoroutineScheduler::resume(int fd, uint32_t event)
{
    // 校验 waiting 后：
    auto h = it->second.handle;
    waiting.erase(it);
    add(h); // 禁止直接 push
}
```

出队时在 `resume` 前执行 `scheduled.erase(h.address())`，允许协程在本次运行中再次挂起并被后续事件调度。

---

### E04: `wakeReadCoroutine` / `wakeWriteCoroutine` 状态机不对称 【P1】

**文件**: [SubReactor.cpp](server/SubReactor/SubReactor.cpp)

| 函数 | 当前行为 | 问题 |
|------|----------|------|
| `wakeRead` | 要求 `state==READ`，唤醒后仍设为 `READ` | 未清状态，无法用 state 防重复入队 |
| `wakeWrite` | 要求 `state==WRITE`，唤醒后设为 `NONE` | 正确模式 |

**修复**（保留类型匹配，统一清状态）:

```cpp
void SubReactor::wakeReadCoroutine(int fd)
{
    auto it = conns.find(fd);
    if (it == conns.end()) return;
    auto &ctx = it->second->session->coroutine_context;
    if (ctx.state != AwaitType::READ) return;
    if (!ctx.handle || ctx.handle.done()) return;
    ctx.state = AwaitType::NONE;   // 与 wakeWrite 一致
    ctx.waiting = false;
    scheduler.add(ctx.handle);
}
```

`wakeWrite` 同样设置 `waiting=false`。  
**不要**做成「任意状态都唤醒」——会破坏读/写等待语义。

---

### E05: Handle 双重注册 【P1】

**文件**: [AWaiter.cpp](server/CoroutineScheduler/AWaiter.cpp)

挂起时同时写入：

1. `conn.session->coroutine_context.handle`
2. `scheduler.waiting[fd]`

唤醒只走 `wake*`（读 context），`scheduler.resume()` **从未被调用**。`fd_close` 虽 `cancel(fd)`，但仍有两套真相源。

**修复**: 选定单一所有者：

- **推荐**: 以 `coroutine_context` 为唯一 handle 存储；删除 `scheduler.waiting` / `suspend` / `resume`，或
- 以 `scheduler.waiting` 为唯一存储；`wake*` 改为 `scheduler.resume(fd, EPOLLIN/OUT)`

---

### E06 + E07: `HttpSession::run()` 读循环致命错误 【P0】

**文件**: [HttpSession.cpp](server/http/HttpSession.cpp)

```cpp
if (readSocket()) {   // recvSocket: true=读成功/EAGAIN 正常结束
    co_return;        // 成功却退出 → 连接秒断
}
if (!parseOneRequest(...))
    co_await ReadAwaiter(...);
// PARSE_OK 时不 break → 死循环反复 parse
```

另：`recvSocket` 在背压（`pauseByMemory`）时返回 `false`，会被当成致命错误 `co_return`（见 E21）。

**修复**:

```cpp
while (true) {
    auto conn = getConn();
    if (!conn || conn->state.closed) co_return;

    if (!reactor->recvSocket(fd)) {
        // 区分：连接已关闭 vs 需要等待更多数据 vs 背压
        if (!getConn() || getConn()->state.closed) co_return;
        co_await ReadAwaiter(reactor, fd);
        continue;
    }
    conn = getConn();
    if (!conn) co_return;

    if (reactor->parseOneRequest(req, *conn))
        break;   // 得到完整请求，进入业务处理
    // NEED_MORE
    co_await ReadAwaiter(reactor, fd);
}
```

建议将 `recvSocket` 改为三态枚举：`RecvOk / RecvNeedWait / RecvClosed`，杜绝 bool 歧义。

---

### E08: `updateEvent` 背压不完整 + 字段已删除 【P1】

**文件**: [SubReactor.cpp](server/SubReactor/SubReactor.cpp)、[Buffer.h](server/Buffer/Buffer.h)

```cpp
readPaused = pauseByMemory;  // 仅此一项
```

`MAX_PIPELINE` / `MAX_PENDING_RESPONSES` 仍定义，但 `ConnState` 已无 `pauseByPipeline` / `pauseByWriteBacklog`，`Connection` 已无 `pendingResponses`。

**修复**:

1. 短期：删除无效宏与误导注释，只保留 `pauseByMemory`
2. 中期：若需 HTTP pipeline，恢复响应队列并实现完整背压三元组

---

### E09: `CoroutineScheduler::suspend` 初始化不完整 【P0·编译】

**文件**: [CoroutineScheduler.cpp](server/CoroutineScheduler/CoroutineScheduler.cpp)

```cpp
waiting[fd] = {event, h};  // WaitNode 有 event/type/handle 三字段
```

**修复**:

```cpp
waiting[fd] = WaitNode{event, AwaitType::NONE, h};
// 或由调用方传入 AwaitType::READ/WRITE
```

---

### E10: `processPendingFds` 使用已 move 的 `unique_ptr` 【P0·UB】

**文件**: [SubReactor.cpp](server/SubReactor/SubReactor.cpp)

```cpp
conns.emplace(fd, std::move(conn));
conn->state.readPaused = false;  // conn 已空 → UB
realConn.session = std::make_unique<HttpSession>(fd, &realConn);  // 见 E16
```

**修复**:

```cpp
auto conn = std::make_unique<Connection>();
conn->fd = fd;
conn->id = ++global_conn_id;
conn->state.readPaused = false;
auto *raw = conn.get();
conns.emplace(fd, std::move(conn));
raw->session = std::make_shared<HttpSession>(fd, this);  // SubReactor*
auto task = raw->session->run();
auto h = task.release();
raw->session->coroutine_context.handle = h;
scheduler.add(h);
scheduler.runReady();
wheel.add(fd);
```

---

### E11: 协程内 `fd_close` 未传 `fromCoroutine=true` 【P0】

**文件**: [HttpSession.cpp](server/http/HttpSession.cpp)

```cpp
reactor->fd_close(fd, "keepalive false");  // 默认 false
```

同理，`parseOneRequest` → `fd_close(..., "parse error")`、`recvSocket` 内关闭，若从协程路径间接调用，也会误把运行中协程再次入队。

**修复**:

- 凡协程栈内调用：`fd_close(fd, reason, true)`
- 超时/外部：`fromCoroutine=false`，仅唤醒挂起中的协程
- 长期：`fd_close` 检测「当前是否持有该 fd 的协程帧」自动判断，避免漏传

---

### E12: `recvSocket` 缺少返回值 【P0·编译/UB】

**文件**: [SubReactor.cpp](server/SubReactor/SubReactor.cpp)

```cpp
if (it == conns.end())
    return;  // 应为 return false;
```

---

### E13: `await_suspend` 返回 `void` 【P1】

**文件**: [AWaiter.h](server/CoroutineScheduler/AWaiter.h) / [AWaiter.cpp](server/CoroutineScheduler/AWaiter.cpp)

连接已关闭时 `return;` 仍会挂起协程，且无人再唤醒 → 帧泄漏（在 `suspend_always` 下更明显）。

**修复**:

```cpp
bool await_suspend(std::coroutine_handle<> h)
{
    auto it = reactor->conns.find(fd);
    if (it == reactor->conns.end() || it->second->state.closed)
        return false;  // 不挂起，立即继续
    // ... 注册 ...
    return true;
}
```

`WriteAwaiter` 同步修改。

---

### E14: `scheduler.resume` 死代码，唤醒路径分裂 【P2】

`CoroutineScheduler::resume` 已实现但从不被调用；实际唤醒全靠 `wake*`。增加维护成本与双重注册风险（E05）。

**修复**: 删除其一，统一 API。

---

### E15: `CoroutineScheduler.h` 重复 `#include <queue>` 【P2】

删除重复即可；顺带删除未使用的 `ready` 队列成员，只保留 `readyQueue`。

---

### E16: `HttpSession` 构造 / 类型 / 头文件依赖错误 【P0·编译】

**问题组合**:

1. 构造函数签名 `HttpSession(int, SubReactor*)`，调用传入 `Connection*`
2. `HttpSession.h` 使用 `AwaitType` 未 `#include "CoroutineScheduler.h"`（或前向/独立头）
3. `session` 为 `shared_ptr`，用 `make_unique` 赋值虽可隐式转换，但意图不清，建议统一 `make_shared`

**修复**: `explicit HttpSession(int fd, SubReactor* r)`，调用 `HttpSession(fd, this)`。

---

### E17: `readRequest` / `execute` / `send` 与 `run` 逻辑分裂 【P2】

- `readRequest()` **空函数体** → `Task<HttpRequest>` 缺少 `co_return` → **编译失败**
- `execute`/`send` 已实现但 `run()` 内联了一套平行逻辑

**修复**: 删空壳或让 `run()` 调用三者；禁止保留不可编译的空协程函数。

---

### E18: Pipeline 背压「假修复」 【P1·架构债】

注释写「检查 pendingResponses」，代码为空；字段已随协程迁移删除。

**修复**: 明确产品决策：

- **协程模型默认一连接一请求流**：删除 `MAX_PIPELINE` / `MAX_PENDING_RESPONSES` 及注释
- **若需 pipelining**：在 `ConnPipeline` 恢复有序响应队列，并在 `parseOneRequest`/`afterSend` 更新背压标志

---

### E19: `fd_close` 与 `final_suspend` 语义不匹配 【P0】

见 E01。`coroutineToWake` 路径在 `suspend_always` 下依赖后续 `runReady` 完成销毁；源码注释却仍写“帧自动释放”。外部关闭时若协程已经完成，仍必须交给统一完成路径，不能在多个位置随意 `destroy()`。

**修复原则**:

```cpp
// fd_close 只做：
// 1. cancel(fd) 清等待注册
// 2. 清 Connection/Session 中的观察句柄
// 3. 非协程内关闭时，将未完成帧重新交给 scheduler.add(h)
// 4. 禁止 fd_close 直接 destroy；完成帧统一由 scheduler.reap(h)
```

建议给调度器增加 `reap(Handle)` 或 `complete(Handle)`，将“清引用 + destroy”集中在一个函数，避免 `runReady`、`fd_close`、`Task` 析构各自销毁。

---

### E20: `AwaitType` 放置位置不合理 【P2】

宜抽到 `AwaitTypes.h` 或放在 `AWaiter.h`，供 `HttpSession` / `CoroutineScheduler` 共用，切断不必要的头依赖。

---

### E21【增补】: 背压返回值被当成连接错误 【P0】

**文件**: [SubReactor.cpp](server/SubReactor/SubReactor.cpp) `recvSocket`

```cpp
if (conn.readBuffer.readableBytes() > MAX_PENDING_BYTES) {
    conn.state.pauseByMemory = true;
    updateEvent(fd);
    return false;  // HttpSession::run 会 co_return 关掉连接！
}
```

**修复**: 背压应返回「成功但暂停」或独立枚举，**禁止**与连接关闭共用 `false`。

---

### E22【增补】: 全局 `ThreadPool pool` 未定义 【P0·链接】

**文件**: [FileBody.h](server/Repsonse/FileBody.h) `extern ThreadPool pool;`  
**文件**: [FileBody.cpp](server/Repsonse/FileBody.cpp) `pool.addTask(...)`  
**文件**: [main.cpp](main.cpp) — **无** `ThreadPool pool(...);`

访问 `/logo` 等文件路径会在链接期失败；即便强行链接，运行期也无池可用。

**修复**:

```cpp
// main.cpp
ThreadPool pool(std::thread::hardware_concurrency());
```

或将 FileCache 预热改为独立 `std::jthread` / 专用小池，不再依赖 HTTP 旧线程池概念。

---

### E23【增补】: `Buffer.h` 缺少 `#include <string_view>` 【P0·编译】

`append(std::string_view)` 未包含对应头。

---

### E24【增补】: CMake 配置缺陷 【P1】

**文件**: [CMakeLists.txt](CMakeLists.txt)

- 缺少 `project(...)` → `${PROJECT_SOURCE_DIR}` 不可靠
- `CoroutineScheduler.cpp` / `AWaiter.cpp` **重复列出两次**
- 未设置 `-fcoroutines`（视编译器而定；GCC/Clang 在 C++20 下通常自动开启）

**修复示例**:

```cmake
cmake_minimum_required(VERSION 3.16)
project(webserver LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(webserver
  main.cpp
  # ... 每个 .cpp 仅一次
)
target_include_directories(webserver PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(webserver PRIVATE pthread)
```

---

### E25【增补】: `Connection` 移动赋值未转移 `session` 【P1】

**文件**: [SubReactor.h](server/SubReactor/SubReactor.h)

移动构造转移了 `session`，移动赋值没有 → 若未来对 `Connection` 做赋值会丢会话/泄漏。

当前主要用 `unique_ptr<Connection>`，风险较低，但仍应补齐或 `=delete` 拷贝/赋值。

---

### E26【增补】: 业务同步执行阻塞整个 SubReactor 【P1·架构】

`createResponse` → `router.handle` 在协程内、**Reactor 线程**同步执行。

`main.cpp` 中 `/stream2` 含 `sleep(1)` 循环 → **阻塞该 SubReactor 上所有连接**。

旧模型用线程池隔离阻塞业务；迁移后该隔离丢失，却未用「协程 + 工作队列」补回。

**修复方向**: 见第三节 B / 路线 Phase 2。

---

### E27【增补】: Keep-Alive 后写侧状态与 epoll 兴趣 【P1】

`afterSend` 在 keep-alive 时 `wantWrite=false` + `updateEvent`，但若响应对象/缓冲未完全复位，或 `SEND_OK` 后未确保读兴趣重新注册，下一请求可能饿死。

建议在 `afterSend` keep-alive 路径增加断言/日志：`!wantWrite && !readPaused ⇒ EPOLLIN 已 rearm`。

---

### E28【增补】: `Task` 异常直接 `terminate` 【P2】

`unhandled_exception()` 调用 `std::terminate()`，单个请求异常拖垮进程。

生产环境应记录日志并 `co_return` / 关闭连接。

---

### E29【本次复核新增】: `Task<T>` 与 `Task<void>` 析构语义不一致 【P0】

**文件**: [Task.h](server/CoroutineScheduler/Task.h)

- `Task<void>::~Task()` 会销毁仍持有的帧；
- `Task<T>::~Task()` 当前为空；
- 两者均使用 `final_suspend = suspend_always`。

这会导致未调用 `release()` 的 `Task<T>` 永久泄漏协程帧。例如 `readRequest()`、`execute()` 一旦作为局部 Task 使用，其所有权行为与 `Task<void>` 完全不同。

**修复**：两个特化必须遵守同一 RAII 规则：

```cpp
~Task()
{
    if (handle) {
        handle.destroy();
    }
}
```

调用 `release()` 后 `handle` 已置空，Task 析构不会与调度器重复销毁。

---

### E30【本次复核新增】: `Task` 移动构造读取未初始化 handle 【P0·UB】

两个移动构造函数都先执行：

```cpp
if (handle)
    handle.destroy();
```

但移动构造时目标对象的 `handle` 尚未初始化，读取它就是 UB。移动构造不需要清理目标对象，应直接交换或使用初始化列表：

```cpp
Task(Task&& other) noexcept
    : handle(std::exchange(other.handle, nullptr))
{
}

Task& operator=(Task&& other) noexcept
{
    if (this != &other) {
        if (handle) handle.destroy();
        handle = std::exchange(other.handle, nullptr);
    }
    return *this;
}

Task(const Task&) = delete;
Task& operator=(const Task&) = delete;
```

还需 `#include <utility>`。

---

### E31【本次复核新增】: 调度器销毁帧前未清除观察句柄 【P0·UB】

当前 `runReady()` 在 `h.done()` 后直接 `h.destroy()`，但以下位置可能仍保存同一地址：

- `CoroutineContext::handle`
- `waiting[fd].handle`
- 未来的 owner/connection 索引

随后 `wakeReadCoroutine` / `wakeWriteCoroutine` 对 stale handle 调用 `done()` 已经是 UB，不需要等到 `resume()` 才出错。

**修复**：完成回收必须原子化为一个调度器操作：

```cpp
void CoroutineScheduler::reap(Handle h)
{
    clearWaitingFor(h);
    scheduled.erase(h.address());
    onCompleted(h);       // SubReactor 清 CoroutineContext::handle
    owned.erase(h.address());
    h.destroy();          // 永远最后执行
}
```

---

### E32【本次复核新增】: `release()` 后没有真正的所有权登记 【P0·泄漏风险】

源码注释写“handle 交给 scheduler 管理”，但调度器只有临时 `readyQueue`、`waiting` 与去重集合；协程挂起后并不存在明确的 owning container。

**建议增加**：

```cpp
struct OwnedCoroutine {
    Handle handle;
    int fd;
};

std::unordered_map<void*, OwnedCoroutine> owned;

void adopt(int fd, Handle h);  // 接收 task.release() 的所有权
void schedule(Handle h);       // 只排队，不转移所有权
void reap(Handle h);           // 唯一销毁入口
```

`processPendingFds` 应执行 `scheduler.adopt(fd, task.release())`，而不是用一次 `add()` 暗示所有权转移。

---

## 三、修改建议（按优先级）

### 3.1 立即修复清单（建议按序）

| 序 | 编号 | 动作 |
|----|------|------|
| 1 | E01/E02/E19/E31/E32 | 落实 `suspend_always`：`adopt → schedule → reap`，保证只有调度器销毁 |
| 2 | E29/E30 | 统一两个 Task 特化的 RAII，并修复移动构造 UB |
| 3 | E16/E17/E09/E12/E23/E24/E22 | 让工程在 Linux 上编译链接通过 |
| 4 | E10 | 修 `processPendingFds` 失效指针与 `HttpSession(this)` |
| 5 | E06/E07/E21 | 重写 `HttpSession::run` 读循环 + `recvSocket` 三态 |
| 6 | E03 | 补 `<unordered_set>`；所有唤醒统一经过 `add/schedule` 去重 |
| 7 | E11 | 所有协程路径 `fd_close(..., true)` |
| 8 | E04/E13/E05 | 唤醒状态机、`await_suspend` bool、单一等待注册 |
| 9 | E08/E18 | 清理或恢复背压，禁止假代码 |

### 3.2 架构改进建议

#### A. 统一协程生命周期（必做）

```
确定方案:
  Task::final_suspend = suspend_always
  processPendingFds: task.release() → scheduler.adopt(fd, h)
  epoll event: scheduler.schedule(h)
  runReady: resume → if done → scheduler.reap(h)
  reap: 清 waiting/scheduled/context/owned → destroy

所有权边界:
  Task 持有未 release 的帧
  Scheduler 持有已 release 的帧
  readyQueue / waiting / CoroutineContext 仅观察，不拥有

硬约束:
  destroy 只能出现在 Task 析构/移动赋值和 Scheduler::reap
  Task 已 release 时析构不得 destroy
  Scheduler 已 adopt 时其他模块不得 destroy
  destroy 后任何容器都不得保留 handle 地址
```

`std::suspend_always` 的优势是完成后帧仍有效，调度器可以安全读取 `done()`、执行统一清理并记录指标；代价是每条完成路径都必须进入 `reap()`，否则必然泄漏。

#### B. 阻塞业务移出 Reactor（补回线程池价值）

```
HttpSession::run:
  读完请求
  co_await ThreadPoolAwaiter(work);  // 或 channel + 回调再 resume
  拿回 HttpResponse*
  co_await 写回
```

短路由（`/`、`/user/:id`）可继续 inline；文件/DB/sleep/流式生成必须下沉到工作池。

#### C. 纯协程 I/O 闭环

```
loop:
  epoll_wait → 仅翻译为 scheduler.resume(fd, event)
  runReady()
禁止在 loop 里直接 recv/send（当前已基本做到）
```

#### D. 背压与 Pipeline 产品决策

| 选项 | 适用 | 工作量 |
|------|------|--------|
| 不做 pipelining | 教学/初版 | 删宏，只留 memory 背压 |
| 做 pipelining | 高并发 keep-alive | 恢复有序队列 + 三条件背压 |

#### E. EPOLLONESHOT vs 纯 ET

ONESHOT 降低惊群/重复事件，但每次都要 `epoll_ctl MOD`。可增加 `registeredEvents` 缓存，无变化则跳过 `ctl`（原报告 Phase 2 建议保留，仍推荐）。

---

## 四、后续推荐发展路线

### Phase 0 — 可编译（预计 0.5–1 天）

- [ ] 修复 E03（缺头文件）、E09/E12/E16/E17/E22/E23/E24/E29/E30
- [ ] Linux/WSL 下 `cmake && build` 成功
- [ ] 跑通 `CoroutineScheduler/test.cpp` 与最小 `curl localhost:8080/`

### Phase 1 — 可存活（崩溃清零）

- [ ] E01/E02/E06/E07/E10/E11/E19/E21/E31/E32
- [ ] wrk/ab 短压测：`/` 与 keep-alive 无 malloc corruption
- [ ] 超时关连接与主动关连接均无泄漏（观察 fd / 协程帧数量）

**验收**: 1k 并发 keep-alive 5 分钟无崩溃。

### Phase 2 — 行为正确 + 补回并发模型

- [ ] E04/E05/E13：唤醒与 awaiter 语义正确
- [ ] 引入 `ThreadPoolAwaiter`（或等价），`/stream2`、`sendfile` 预热不阻塞 Reactor
- [ ] 明确 pipeline 策略（删或做完）
- [ ] `recvSocket`/`sendBody` 错误码枚举化

**验收**: `/logo`、`/stream1` 正确；阻塞路由不影响同 Reactor 其他连接延迟。

### Phase 3 — 性能

- [ ] `registeredEvents` 减少 `epoll_ctl`
- [ ] 评估去掉 ONESHOT 改纯 ET + 应用层状态机
- [ ] 对象池/SegmentPool 在协程路径下的热点复测
- [ ] 协程调度批量与缓存友好（减少跨队列跳动）

**验收**: 与迁移前线程池版本对比 QPS/延迟，目标不低于旧版 90%。

### Phase 4 — 可观测与工程化

- [ ] 指标：活跃连接、就绪队列深度、await 次数、解析错误率、协程帧数
- [ ] 结构化日志（已有异步 Logger，补 request-id）
- [ ] 单测：解析、Awaiter 状态机、scheduler 去重、fd_close 两路径
- [ ] 集成测：keep-alive、pipeline（若启用）、慢客户端、半开连接

### Phase 5 — 能力扩展（按需）

- [ ] TLS（OpenSSL/BoringSSL + 协程握手）
- [ ] HTTP/2（多流与现有「一连接一协程」模型冲突，需重新设计）
- [ ] 优雅退出：停止 accept → 排空协程 → join SubReactor
- [ ] 配置文件（端口、线程数、超时、背压水位）

### 不建议的过早优化

- 在 Phase 1 完成前引入 HTTP/2 / io_uring
- 重新引入自动销帧语义，绕过已经确定的 `suspend_always + reap` 契约
- 在未统一 handle 所有权前做复杂的多协程 per connection

---

## 五、关键文件索引

| 路径 | 角色 |
|------|------|
| `main.cpp` | 主 Reactor accept；缺 `ThreadPool pool` 定义 |
| `CMakeLists.txt` | 构建；缺 project、源文件重复 |
| `server/http/HttpSession.cpp` | 协程主循环（E06/E07/E11 核心） |
| `server/http/HttpSession.h` | 会话与 CoroutineContext |
| `server/SubReactor/SubReactor.cpp` | epoll、唤醒、关闭、I/O |
| `server/SubReactor/SubReactor.h` | Connection 分层、背压宏残留 |
| `server/CoroutineScheduler/Task.h` | 协程 promise（生命周期策略） |
| `server/CoroutineScheduler/CoroutineScheduler.*` | 就绪队列 / waiting |
| `server/CoroutineScheduler/AWaiter.*` | Read/Write awaitable |
| `server/Repsonse/FileBody.*` | 依赖全局线程池 |
| `server/threadpoll/*` | 旧线程池；业务路径已不用，文件预热仍依赖 |
| `server/Buffer/Buffer.h` | ConnState；缺 string_view 头 |

---

## 附录：推荐的最小正确 `run()` 骨架

```cpp
Task<void> HttpSession::run()
{
    while (true) {
        auto* conn = /* find */;
        if (!conn || conn->state.closed) co_return;

        HttpRequest req;
        // 1) 读到完整请求
        for (;;) {
            auto rs = reactor->recvSocket(fd); // Ok / NeedWait / Closed
            if (rs == RecvClosed) co_return;
            if (rs == RecvNeedWait) {
                co_await ReadAwaiter(reactor, fd);
                continue;
            }
            conn = /* find */;
            if (!conn) co_return;
            if (reactor->parseOneRequest(req, *conn)) break;
            co_await ReadAwaiter(reactor, fd);
        }

        // 2) 业务（短路径 inline；长路径 co_await 线程池）
        HttpResponse* resp = reactor->createResponse(req);

        // 3) 写完响应
        for (;;) {
            auto st = reactor->sendBody(fd, *resp);
            if (st == SEND_OK) {
                responsePool.release(resp);
                afterSend(); // 内部 fd_close(..., true) 或 keep-alive rearm
                break;
            }
            if (st == SEND_AGAIN) {
                co_await WriteAwaiter(reactor, fd);
                continue;
            }
            reactor->fd_close(fd, "send error", true);
            co_return;
        }
    }
}
```

---

*本报告已按当前源码再次复核，确定采用 `std::suspend_always final_suspend() noexcept`，并增补 E29–E32。修复时以「第三节 3.1 顺序」为准：先建立 `adopt → schedule → reap` 生命周期闭环，再修改业务循环。*
