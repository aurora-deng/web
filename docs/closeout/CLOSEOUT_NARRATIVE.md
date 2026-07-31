# Web 项目收尾总述（CLOSEOUT Narrative）

> 本文整合：架构边界讲解、你对收尾的口头理解（已纠偏）、验证证据与停扩约定。  
> 配套清单：[CLOSEOUT.md](CLOSEOUT.md)  
> 阶段 A 改动：[closeout-A_tcp_nodelay_graceful_shutdown.md](closeout-A_tcp_nodelay_graceful_shutdown.md)

**状态**：功能与验证主线已收口；**本仓库停止横向扩张协议栈**。后续学习测试须等你明确下令后再开。

---

## 1. 收尾结论（一句话）

这是一个 Linux 上可运行、可测试、可压测的 **C++20 多 Reactor + 协程会话 + Executor 业务卸载** 的 HTTP/1.1 服务端；对象化与停服/限流已落地，虚拟机上 `GET /` 约 **3 万 QPS** 量级可复现。  
**刻意不做** WebSocket / SSE / HTTP/2 / gRPC / io_uring / SSL。收口后主线转向 ROS 2 / DDS / 实时（见 roadmap）。

---

## 2. 正确架构（纠偏后的定稿表述）

### 2.1 层级（不要写成 reactor→runtime→session→http）

```text
ServerRuntime（总管生命周期）
  ├─ Accept 循环：listen / accept / 信号停服 / 尽早释放端口
  ├─ ReactorGroup → 多个 SubReactor（epoll、连接表、时间轮、协程调度）
  ├─ 共享 Executor（ThreadPool Worker：跑业务 handler）
  ├─ 共享 HttpCodec + Router
  └─ 每连接一条 HttpSession 协程（读 → 提交 → 等完成 → 写）
```

| 层 | 职责 | 禁止 |
|----|------|------|
| **Runtime** | 生命周期、listen、accept、共享 Executor/Codec | 业务 sleep、协议解析细节 |
| **SubReactor** | I/O、解析驱动、协程调度、发响应 | 长时间阻塞（会拖死同核连接） |
| **Executor / Worker** | 执行可能慢的 handler | 直接操作 epoll / 半截写 socket（约定做完再 notify） |
| **HttpSession 协程** | 串起单连接上的步骤 | 跨 `co_await` 死捏 `Connection*` |

### 2.2 易混点（你原先说法 → 定稿）

| 原先说法 | 定稿 |
|----------|------|
| 任务丢到 wake 线程做业务 | **业务在 Executor Worker**；Worker 完成后写 **eventfd 叫醒 SubReactor**，再 `processComplete` **唤醒会话协程**。wake ≠ 业务线程。 |
| 业务也用协程跑 | **会话状态机用协程**；`/slow` 的 `sleep` 在 **Worker 同步执行**；协程在 `ExecuteAwaiter` 上等待。 |
| 背压只是防内存爆 | 还有 **连接上限**、**执行队列满→503**；背压 = 有界资源 + 明确拒绝/降速。 |
| 层级 runtime 在 reactor 后 | **Runtime 包住 ReactorGroup**，是总管不是下游。 |

### 2.3 `/slow` 时序（收尾必会讲）

```text
Reactor:  submit ──► co_await EXECUTE ──────────────────────────► resume ──► write
                │                                              ▲
Worker:         └── sleep(10) ──► notify(eventfd + completeQueue)┘
```

要点：

1. `submit` 成功才挂起；队列满则 **503**（背压出口）。  
2. `ExecuteAwaiter` **不挂 epoll**，等的是 Worker 完成通知。  
3. `notifyExecuteComplete` 投递 `{fd, connId}` 并敲 eventfd。  
4. `processComplete`：conn 仍在且 id 匹配 → 唤醒；否则查 **zombieWakes**，防协程泄漏。  
5. 恢复后再次 `getConn()`：没了则安静 `co_return`。

黑盒 `/slow`+`/fast` 证明的是：**Worker 阻塞不得拖死同 Reactor 上其他连接**。

---

## 3. 请求生命周期

- **连接级**：Session / Connection 有共享所有权，保证协程未结束时对象仍在。  
- **请求级**：`RequestContext` 挂在 Session 上；每轮 `reset`；response 常走对象池。  
- **等待点之后**：必须重新 `getConn()`，不跨 `co_await` 缓存裸指针。

文档定稿句：  
「请求态在 Session 的 Context 中流转；连接寿命用共享所有权；每次挂起恢复后重验连接。」

---

## 4. 背压（三闸）

1. **Accept**：`maxConnections_`（默认 10000），超额关闭新连接。  
2. **读侧**：入站水位（如 1MiB）暂停读，防慢客户端撑爆内存。  
3. **执行侧**：线程池队列有界（如 4096）；`submit` 失败 → **503 Service Unavailable**。

---

## 5. 优雅停服顺序（定稿）

1. SIGINT/SIGTERM → `requestStop()`（`running_=false` + eventfd 唤醒 accept）。  
2. **先 `releaseListener()` 关闭 listen，尽快归还端口**。  
3. 停止并 join SubReactor。  
4. 成员析构顺序保证 **Executor 先 drain**，再销毁仍可能被引用的 Reactor 侧对象。

原则：**先停接听，再收尾线程**——与「先 join 再关端口」相反。

---

## 6. 验证与证据

### 6.1 四层验证各自证明什么

| 层 | 工具 | 证明什么 |
|----|------|----------|
| 单元 | GoogleTest | 组件契约（Parser/Buffer/Router/Sender…） |
| 黑盒 | Python + 真 TCP | 端到端行为 + `/slow`/`/fast` 边界 |
| 压测 | wrk（不必 wrk2） | 吞吐与尾延迟基线 |
| Fuzz | libFuzzer | 解析器在畸形输入下不崩 |

一键入口：`bash scripts/gen_report.sh`（含简易 wrk 压测）。

### 6.2 近期基线（虚拟机 loopback，`GET /`）

环境：CentOS Stream 9，约 4 vCPU / 6GiB，VMware。

| 场景 | 结果（约） |
|------|------------|
| `wrk -t4 -c128 -d10s`（gen_report 内置） | **QPS ≈ 31060**，Avg ≈ 6.11ms，P50 ≈ 3.66ms，P99 ≈ 85.54ms |

解读：

- **吞吐**：当前设备上主路径健康，约 3 万 QPS 量级。  
- **P99 远大于 P50**：尾部排队/抖动，不是「平均路径慢 20 倍」。  
- **不要和裸机比绝对值**；比同环境前后版本。  
- **不必上 HTTP/2/WebSocket 来「再榨设备」**——那是新阶段，不是收尾。

更早 TCP_NODELAY 修复后的对照表见 [CLOSEOUT.md](CLOSEOUT.md)。

### 6.3 测试学习「度」（已把关，未开课）

- **值得**：会解释现有四层各证明什么；会为边界写断言（如 Executor 隔离）。  
- **不值得（收尾期）**：系统学测试学科、堆覆盖率工具、为涨 QPS 开协议。  
- **开课条件**：等你明确下令「开始学测试」后再进入 `docs/testing/learning/` 节奏；见 [../testing/LEARNING_SCOPE.md](../testing/LEARNING_SCOPE.md)。

---

## 7. 已知限制

- 仅 Linux epoll；无 TLS / HTTP2 / WebSocket / io_uring（**停扩**）。  
- 设备与虚拟化限制尾延迟；架构在**当前设备**上极限大致见压测，不靠加协议硬抬。  
- 取消目前是协作式关连接 + 僵尸协程，**无通用 cancellation token**。  
- 部分静态/条件请求行为以实现为准（测试已按行为对齐，不臆造 API）。  
- 正式竞品对比、物理机火焰图：可选加深，**非收尾阻塞项**。

---

## 8. 稳定性 & 性能闭环（能力地图，非收尾必做完）

```text
Benchmark →（可选）故障注入 → Sanitizer → Metrics(QPS/P99/错误) → 火焰图(仅异常时)
```

收尾阶段：Benchmark 基线已有；其余保持「知道何时用」，不必产品化。

---

## 9. 刻意不做 / 下一步

**不做：** WS / SSE / HTTP2 / gRPC / io_uring / SSL / 为简历堆名词。  

**下一步（仓库外主线）：** [../roadmap/ROBOTICS_ROADMAP.md](../roadmap/ROBOTICS_ROADMAP.md)  

**本仓若再动刀：** 仅明确 bug、文档、验证脚本；不扩张协议。

---

## 10. 收口自检

- [x] Session / Parser / Sender / Executor / Runtime 对象化  
- [x] Executor 边界（`/slow` vs `/fast`）  
- [x] TCP_NODELAY + 优雅退出 + 连接上限  
- [x] 单元 + 黑盒 + fuzz + wrk 基线可复现  
- [x] 架构表述纠偏并落本文  
- [x] 文档目录整理  
- [ ] （可选）你下令后再学测试深化  
- [ ] （可选）ASan/TSan 定期绿  
- [ ] 宣布收口完成 → 开 ROS 2 主线  
