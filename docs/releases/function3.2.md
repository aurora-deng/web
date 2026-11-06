# Function3.2：可复现构建、故障恢复与 ACK 性能定位

对比 `Function3.1`。本版增加社区内容和持久事件投递的独立装配、两档 Linux 复现入口、数据库连接恢复、聊天 ACK 分段采样、隔离演练及学习计划。通信核心 `backend/server/`、数据库迁移 001–024 和日常运行服务不变。

[Obsidian 学习与助教提示词](../learning/nebula-performance-and-reliability.md) · [架构与目录](../architecture/overview/project-layout.md) · [构建和复现](../ops/validation/reproduce.md)

| 改动 | 文件和职责 |
|---|---|
| 装配拆分 | `CommunityContentRuntime` 负责树动态、内容治理和媒体；`CommunityDeliveryRuntime` 负责事件投递、重放及退出边界。旧业务接口继续使用。 |
| 干净环境复现 | `scripts/project/reproduce.py` 分通信和 PostgreSQL 业务两档；后者应用 001–024、检查 CTest 矩阵并验证真实接口、SSE 和重启后的历史。Dockerfile 是草案，未构建验收。 |
| 连接池恢复 | `PostgresConnectionPool::acquire` 在独占槽位后、释放池锁时尝试修复已知坏连接；失败归还槽位并返回错误。归还路径只对健康连接清理未完成事务；没有自动重跑 SQL 或 COMMIT。 |
| ACK 观测 | `ChatAckTrace`、`Phase11Realtime`、`PostgresStore::Transaction`：默认关闭；`WEB_CHAT_ACK_TRACE_LIMIT` 以进程为单位，最多尝试采样 10,000 次发送，只有成功返回业务 ACK 的请求输出记录。只记录消息 ID 和时间。 |
| 演练与压测 | `function32_lab.py` 只操作自建的 PostgreSQL 和测试进程；`function3_mixed.py` 支持配置工作端数量和按消息 ID 对齐 RTT。 |

## 复现

在可信 Linux 虚拟机以普通用户执行。先按[构建说明](../ops/validation/reproduce.md)获得启用 PostgreSQL 和 libsodium 的 Release 二进制，复用已装的 PostgreSQL 工具路径。新输出目录必须不存在；不接受现有数据库地址，不指定日常端口。

```bash
PG_PREFIX=/path/to/installed/postgresql
BIN="$PWD/build-repro-business/webserver"
python3 scripts/function3/validation/function32_lab.py faults \
  --binary "$BIN" --postgres-prefix "$PG_PREFIX" --output "$PWD/build-function3/f32-faults"
python3 scripts/function3/validation/function32_lab.py profile \
  --binary "$BIN" --postgres-prefix "$PG_PREFIX" --output "$PWD/build-function3/f32-profile" \
  --workers 1 4 16 --operations 4000
```

报告在各目录的 `report.json`，各档有原始压测 JSON 和服务端日志。实验使用本地事件投递；不启用 Redis、RocketMQ、AI、云 TTS 或 TLS 链路。每档每个工作端有两条 WebSocket、一个驻留 SSE，另有 HTTP 请求；一个工作端不等于一条 TCP 连接。

## Linux 验证与故障结果

从空构建目录完成 GCC 11.5 / CMake 3.31.8 / OpenSSL 3.5.5 / PostgreSQL 18.6 的 Release 构建。隔离业务复现 113 项 CTest：**112 通过，1 项可选 Redis 契约跳过**。注册登录、WebSocket、果实幂等发布、Outbox SSE、重启后 Session 与果实读取通过。验证使用公开源码快照；二进制 SHA-256 为 `c34b7723f00db2c88bb566269c15607848fa3879e9a401a16f61c7c44574e49f`。

| 注入故障 | 观察结果 |
|---|---|
| 终止临时数据库的 10 个连接 | 进程存活；一个发送尝试返回错误，同编号在第 2 次尝试收到持久化 ACK。从注入到成功约 115 ms。故障发生时 libpq 可能尚不知道闲置连接已失效，第一次 SQL 仍可能报错。 |
| 测试进程在 ACK 后被 SIGKILL | 重启后同编号返回 `duplicate` 和原消息 ID；聊天历史与 SQL 均只查到一条。杀进程到核对历史约 267 ms。 |

这是单次隔离观察，涵盖已确认消息的恢复；不代表全站 RTO/RPO，也未强制注入 COMMIT 结果未知、Broker 丢 ACK、磁盘写满或宿主机故障。没有触发日常服务重启、迁移或替换。

## 性能定位：当前证据指向哪里

同一 Release 二进制，关闭 ACK 采样，混合比例为会话查询 50%、历史查询 20%、聊天写入和在线送达 30%。每档固定 4,000 次操作、1,200 个持久化 ACK、0 错误。每档单轮；这是闭环客户端、短时隔离测试，不是生产容量或 A/B 对比。

| 工作端 | 混合操作/秒 | ACK P50 / P95 / P99（ms） | 服务端 RSS 峰值（KiB） | DB 执行器队列峰值 |
|---:|---:|---:|---:|---:|
| 1 | 149.131 | 7.525 / 13.940 / 21.097 | 11,600 | 1 |
| 4 | 446.433 | 9.167 / 18.007 / 25.810 | 12,744 | 2 |
| 16 | 510.284 | 29.214 / 44.236 / 56.483 | 13,604 | 12 |

4 → 16 工作端，吞吐只增加约 14%，ACK P99 增至约 2.19 倍。执行器有排队，未达到队列容量 256，拒绝为零；峰值不能说明某条 ACK 实际排了多久。

16 工作端另跑一轮开启采样，1,200 条成功匹配。客户端 ACK P99 **56.436 ms**；服务端从 handler 开始到 ACK 就绪 P99 **25.964 ms**，取连接 P99 **3.817 ms**，COMMIT P99 **5.827 ms**。COMMIT 前累计 P99 **24.339 ms**，这段包含取连接和业务处理。不能把各阶段 P99 相加或相减。最慢两条如下：

| 客户端 RTT | 取连接 | 开始 COMMIT 时累计 | COMMIT | ACK 就绪时累计 |
|---:|---:|---:|---:|---:|
| 76.740 ms | 18.799 ms | 33.659 ms | 10.286 ms | 43.968 ms |
| 74.109 ms | <0.001 ms | 17.418 ms | 2.496 ms | 19.959 ms |

结论：**优先拆查 DB 执行器等待和事务提交前路径**。个别慢样本取连接时间显著，但不能解释大多数慢样本；COMMIT 也不能解释全部尾部。仍需把同一请求的“入队 → worker 开始”与“ACK 就绪 → 客户端收到”分别测量，并用数据库执行计划和等待事件区分 SQL、锁与 I/O。暂不加连接池、改 Reactor 或降低持久化级别。

## 还没验收的事项

- 十对 A/A 测量噪声、同配置三轮 A/B 性能收益、`/fast` 与 ACK P99 性能门槛未复验。保留 [Phase49 超标记录](../architecture/business/engineering/phase49.md)。
- 本轮代码测的是 Release；Debug、Sanitizer、正式容量、24 小时稳定性、MQ 和完整外部依赖故障矩阵待验。
- 如需回退：日常运行版本无需处理；开发侧切回 `Function3.1` 或关闭 ACK 采样，复现环境用脚本自动清理。没有本轮数据库迁移需要撤销。
