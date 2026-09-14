# Web 收口清单

> **历史文档说明**：本结项文档记录的是项目演进历史，表中条目反映当时的对象化进度。
> 当前 2.0 架构已演进到**第四阶段**：出站统一走 **OutboundTask + OutboundQueue + TransportWriter + writerLoop**
> （`OutboundQueue` 类管理每连接出站队列 + ticket，由 SubReactor 驱动 writerLoop 冲刷），**已无 `ResponseSender`**
> （该类在第四阶段重构中删除，出站发送不在 Session 层抽象 sender）。表中
> `Sender 对象化 | ResponseSender` 一行作为历史记录保留，请勿当作当前架构事实。

按既定链路推进；**已完成的对象化不再重做**，只补验证与停扩。

- **收尾总述（推荐先读）**：[CLOSEOUT_NARRATIVE.md](CLOSEOUT_NARRATIVE.md)  
- **本批次改动详解**：[closeout-A_tcp_nodelay_graceful_shutdown.md](closeout-A_tcp_nodelay_graceful_shutdown.md)（变更代号 `closeout-A`）  
- **文档总索引**：[../README.md](../README.md)

## 总链路

```text
Session/Parser
→ Executor/Runtime 边界
→ 取消、背压、优雅退出、测试
→ Sanitizer 与故障注入
→ 压测、P99、火焰图、竞品对比
→ 完成设计文档
→ Web 项目停止扩张
→ gRPC/ROS2/DDS/实时 Linux
```

## 状态

| 项 | 状态 | 说明 |
|----|------|------|
| Session 状态机 | ✓ | READING→…→CLOSED |
| Parser 对象化 | ✓ | RequestLine/Header/Body |
| Sender 对象化 | ✓ | ResponseSender |
| Executor 对象化 | ✓ | Worker + ExecuteAwaiter |
| Runtime 独立 | ✓ | ServerRuntime |
| Executor/Runtime 边界 | ✓ | I/O 在 Reactor，业务在 Executor；`/slow`+`/fast` 可证 |
| TCP_NODELAY | ✓ | accept 后对 **newfd** 设置 |
| 取消 | △ | 协作式 fd_close / 僵尸协程；无通用 cancellation token |
| 背压 | △ | 读水位、Executor 满→503、**最大连接数默认 10000** |
| 优雅退出 | ✓ | 信号 → 停 accept → **先释端口** → stop/join Reactor |
| 单元/黑盒/fuzz | ✓ | `gen_report.sh` / TESTING.md；Linux 实测绿 |
| 压测/P99 | ✓ 基线 | 见下方快照；无物理机/火焰图/竞品正式对比 |
| 设计文档 | ✓ | 已整理分目录 + NARRATIVE |
| 测试深度学习 | ⏸ | 等你下令；范围见 [../testing/LEARNING_SCOPE.md](../testing/LEARNING_SCOPE.md) |
| Web 停止扩张 | ✓ 约定 | 不做 WS/SSE/HTTP2/gRPC/io_uring/SSL |
| ROS2/DDS/实时 | □ | 收口后另开主线 |

## 刻意不做（停扩）

- WebSocket / SSE / HTTP/2 / gRPC  
- io_uring / SSL/TLS / NUMA 专项  
- 继续横向加协议以“凑简历名词”

## 虚拟机压测快照

### TCP_NODELAY 修复后（历史）

| 命令 | QPS | P50 | P99 |
|------|-----|-----|-----|
| `wrk -t2 -c16 -d30s --latency` | ~23329 | 0.56ms | 4.7ms |
| `wrk -t4 -c128 -d60s --latency` | ~23291 | 5.0ms | 18ms |
| `wrk -t4 -c1000 -d30s --latency` | ~19772 | 47ms | 115ms |

### gen_report 内置简易 wrk（2026-07-30）

| 命令 | QPS | Avg | P50 | P99 |
|------|-----|-----|-----|-----|
| `wrk -t4 -c128 -d10s --latency` | ~31060 | 6.11ms | 3.66ms | 85.54ms |

解读：主路径吞吐健康；P99 为尾部问题。无需 wrk2。

## 下一步（按序）

1. （可选）你下令后按 LEARNING_SCOPE 学测试  
2. （可选）Linux 上 ASan/TSan 跑绿  
3. 宣布 Web 收口完成 → [../roadmap/ROBOTICS_ROADMAP.md](../roadmap/ROBOTICS_ROADMAP.md)  
