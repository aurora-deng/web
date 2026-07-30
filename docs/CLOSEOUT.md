# Web 收口清单

按既定链路推进；**已完成的对象化不再重做**，只补验证与停扩。

本批次改动详解（逐步作用）：[../closeout-A_tcp_nodelay_graceful_shutdown.md](../closeout-A_tcp_nodelay_graceful_shutdown.md)（变更代号 `closeout-A`）。

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
| Executor/Runtime 边界 | ✓ | I/O 在 Reactor，业务在 Executor |
| TCP_NODELAY | ✓ | accept 后对 **newfd** 设置 |
| 取消 | △ | 协作式 fd_close / 僵尸协程；无通用 cancellation token |
| 背压 | △ | 读 1MiB、Executor 满→503、**最大连接数默认 10000** |
| 优雅退出 | ✓ | SIGINT/SIGTERM → 停 accept → stop/join Reactor |
| 单元/黑盒/fuzz 入口 | ✓ | 见 TESTING.md；须在 Linux 实测 |
| Sanitizer | △ | 脚本已有；Linux 上跑绿后勾完 |
| 故障注入 | □ | 未成体系（慢客户端等仅有部分黑盒） |
| 压测/P99 | △ | 已有 VM 实测快照；无物理机/火焰图/竞品正式对比 |
| 设计文档 | ✓ | ARCHITECTURE / TESTING / BENCHMARK / ROBOTICS |
| Web 停止扩张 | ✓ 约定 | 不做 WS/SSE/HTTP2/gRPC/io_uring/SSL |
| ROS2/DDS/实时 | □ | 收口后另开主线 |

## 刻意不做（停扩）

- WebSocket / SSE / HTTP/2 / gRPC  
- io_uring / SSL/TLS / NUMA 专项  
- 继续横向加协议以“凑简历名词”

了解级阅读可以，**本仓库不再实现**。

## 虚拟机压测快照（TCP_NODELAY 修复后）

环境：虚拟机 loopback，`GET /`，非物理机上限。

| 命令 | QPS | P50 | P99 |
|------|-----|-----|-----|
| `wrk -t2 -c16 -d30s --latency` | ~23329 | 0.56ms | 4.7ms |
| `wrk -t4 -c128 -d60s --latency` | ~23291 | 5.0ms | 18ms |
| `wrk -t4 -c1000 -d30s --latency` | ~19772 | 47ms | 115ms |

解读：可持续约 **2.0万～2.3万 QPS**；c1000 延迟为排队，不是 Nagle。修复前 c16 仅 ~385 QPS / ~41ms。

## 下一步（按序）

1. Linux：`bash scripts/run_tests.sh` 与 ASan/TSan 跑绿  
2. 验证：`kill -TERM <pid>` 打印 `server stopped.` 且可重启  
3. 可选：`/slow`+`/fast` 结果写入本页  
4. 宣布 Web 收口完成 → 按 [ROBOTICS_ROADMAP.md](ROBOTICS_ROADMAP.md) 开 ROS 2  
