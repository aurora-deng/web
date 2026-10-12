# Phase 16：可观测性、Outbox 主动唤醒与运维闭环

Phase 16 解决两个问题：先让服务器能说明“内部哪里在排队”，再消除 Phase 15 定位到的 250 ms
固定轮询等待。它还补上迁移审计、备份恢复演练、依赖感知的就绪检查、日志边界和稳定性巡检。

可以把它想成给仓库增加仪表盘和门铃：仪表盘显示工人、货架和装卸口是否拥堵；门铃让新包裹入库后
立即通知快递员。原来的定时巡仓仍保留，用来处理漏通知和外部写入。

## 1. 本阶段完成内容

| 能力 | 完成状态 | 主要入口 |
|---|---|---|
| 数据库执行器指标 | 已完成 | `BoundedExecutor::snapshot()`、`/metrics` |
| PostgreSQL 连接池指标 | 已完成 | `PostgresConnectionPool::snapshot()`、`/metrics` |
| Outbox 发布指标 | 已完成 | `Phase11OutboxPump::snapshot()`、`/metrics` |
| 事务提交后主动唤醒 | 已完成 | `notifyCommitted()` + generation counter |
| 250 ms 兜底轮询 | 保留 | 防止外部写入或通知遗漏 |
| 同二进制 A/B 对照 | 已完成 | `WEB_PHASE11_OUTBOX_POLL_ONLY=1` |
| 数据库感知 readiness | 已完成 | `/health/ready` 在 Worker 上检查 PostgreSQL |
| 迁移版本审计 | 已完成 | `postgres-migration-status.sh` |
| 逻辑备份、校验、隔离恢复 | 已完成 | `postgres-backup.sh` |
| 日志与磁盘边界 | 已完成基础闭环 | 启动磁盘门槛、停机轮转、运维 JSONL 事件 |
| 有界稳定性巡检 | 已完成 | `phase16-soak.py` |

## 2. 新的数据路径

```mermaid
flowchart LR
    Client[HTTP / WebSocket 写操作] --> Business[Chat / Social / Presence]
    Business --> Tx[PostgreSQL 事务]
    Tx --> Message[(messages)]
    Tx --> Outbox[(outbox_events)]
    Tx -->|COMMIT 成功后| Bell[notifyCommitted]
    Bell --> Pump[Outbox Pump]
    Timer[250 ms 兜底轮询] --> Pump
    Pump --> WS[WebSocket]
    Pump --> SSE[SSE]
    Executor[DB Executor 快照] --> Metrics[/metrics]
    Pool[连接池快照] --> Metrics
    Pump --> Metrics
```

通知严格放在事务提交成功之后。若在提交前通知，Pump 可能先查询数据库却看不到尚未提交的 Outbox；
更严重时，客户端可能收到一条最终回滚的消息。

## 3. 主动唤醒为什么不会丢通知

`Phase11OutboxPump` 保存一个单调递增的 generation。每次成功提交后：

1. 写路径递增 generation；
2. `condition_variable` 唤醒 Pump；
3. Pump 比较“等待前观察到的 generation”和当前值；
4. 若两者不同，即使通知恰好发生在进入等待之前，也不会继续睡眠。

普通布尔值像只贴一张“有新邮件”的纸条：连续两封信可能合并成一次状态。generation 像取号机，每次
提交都留下递增号码，Pump 能判断等待期间是否发生过变化。Pump 每次仍会批量扫描 Outbox，因此无需
为每条消息建立一条线程或一项永久内存状态。

环境变量 `WEB_PHASE11_OUTBOX_POLL_ONLY=1` 只用于回归对照，它关闭主动按铃而保留 250 ms 轮询。

## 4. 指标说明

`GET /metrics` 继续返回 Prometheus 文本，并增加下列 `webserver_phase11_` 指标：

| 指标 | 含义 | 需要关注的现象 |
|---|---|---|
| `db_executor_submitted_total` | 接收的数据库任务数 | 与业务流量同向变化 |
| `db_executor_rejected_total` | 有界队列拒绝数 | 正常运行应为 0 |
| `db_executor_active` / `queued` | 当前执行和等待任务 | 长期贴近容量表示拥堵 |
| `db_executor_queue_high_watermark` | 进程启动后的队列峰值 | 用于容量回顾 |
| `postgres_pool_in_use` | 当前占用连接 | 长期等于池大小表示连接瓶颈 |
| `postgres_pool_waited_acquires_total` | 曾等待连接的获取次数 | 增长过快需结合时延分析 |
| `postgres_pool_timeouts_total` | 获取连接超时 | 正常运行应为 0 |
| `outbox_wakeups_total` | 主动唤醒次数 | 主动模式应随提交增长 |
| `outbox_published_total` | 成功发布事件数 | 应最终追上可发布事件 |
| `outbox_blocked_total` / `failures_total` | 背压或发布失败 | 正常运行应为 0 |
| `outbox_oldest_attempt_age_microseconds` | 本轮最老事件年龄 | 持续增长说明积压 |

这些是进程内累计值，进程重启后会归零。告警系统应使用一段时间内的增量，不应把累计总数直接当成
瞬时 QPS。

## 5. 同机同二进制 A/B 结果

测试环境、业务规模和 Release 二进制完全相同，只切换
`WEB_PHASE11_OUTBOX_POLL_ONLY`。每组连续三轮，所有场景总错误均为 **0**。

| 指标 | 250 ms 轮询 | 主动唤醒 | 变化 |
|---|---:|---:|---:|
| 私聊端到端 P50 | 262.088 ms | 9.325 ms | -96.44% |
| 私聊数据库确认 P50 | 6.756 ms | 7.050 ms | 基本相当 |
| 私聊吞吐中位数 | 15.386 条/s | 397.629 条/s | 25.84 倍 |
| 8 人群聊全部到达 P50 | 253.389 ms | 4.593 ms | -98.19% |
| 群聊数据库确认 P50 | 4.056 ms | 3.505 ms | 基本相当 |
| 群聊消息吞吐中位数 | 4.015 条/s | 214.726 条/s | 53.48 倍 |
| Outbox 主动唤醒增量/轮 | 0 | 322 | 符合开关语义 |
| DB Executor 拒绝/失败 | 0 / 0 | 0 / 0 | 无错误换性能 |
| PostgreSQL 获取超时 | 0 | 0 | 无连接池超时 |
| RSS 峰值中位数 | 152,680 KiB | 152,692 KiB | 基本不变 |

原始结果：

- [轮询组 summary.json](../../evidence/phase16/phase16-poll/20261006T173245Z/summary.json)
- [主动组 summary.json](../../evidence/phase16/phase16-active/20261006T173428Z/summary.json)
- 两个目录都保留三轮 JSON、环境、日志和 Release 二进制 SHA-256。

主动组平均 CPU 百分比更高，原因之一是相同工作在约 10.7 秒内结束，而轮询组约 22.5 秒；它不能直接
解释为“总 CPU 消耗翻倍”。要比较能耗需要采集 CPU time，而不是只比较时间窗平均利用率。

## 6. Liveness 与 Readiness

| 端点 | 回答的问题 | PostgreSQL 停止时 |
|---|---|---|
| `/health/live` | 进程和 HTTP 循环是否活着 | 仍返回 200 |
| `/health/ready` | 当前是否能承接完整 Phase 11 请求 | 返回 503 |

数据库检查在 HTTP Worker 上运行，避免同步 libpq 查询堵塞 Reactor。Linux 故障演练中停止 PostgreSQL 后，
liveness 保持 200、readiness 变为 503；数据库重新启动后，第 2 次探测恢复 200，Web 服务进程未重启。

`webserver-user-service.sh start` 现在以 `/health/ready` 返回成功作为启动完成标准，不再把“端口打开”
误当作“业务可以工作”。

## 7. 数据库运维

### 7.1 迁移状态

```bash
./scripts/phase11/postgres-migration-status.sh phase11
```

脚本只读比较仓库中的 `001`–`004` 与 `phase11_schema_migrations`。缺版本或出现仓库不知道的版本都会
非零退出。实测结果为 `expected=4 applied=4 latest=4`。

### 7.2 备份和恢复演练

```bash
backup_path="$(./scripts/phase11/postgres-backup.sh create | tail -n 1)"
./scripts/phase11/postgres-backup.sh verify "${backup_path}"
./scripts/phase11/postgres-backup.sh restore-drill "${backup_path}"
```

备份先写 `.partial`，`pg_restore --list` 成功后才原子改名，并生成 SHA-256。恢复只允许写入名称以
`phase11_restore_drill` 开头的隔离数据库，结束后自动删除。实测恢复出 4 个迁移版本、59 个用户、
39 个会话和 58 条消息，正式库未被覆盖。

这是逻辑备份，不包含时间点恢复所需的 WAL 归档；公网关键业务仍需异机备份、加密、保留策略和定期
灾难恢复演练。

## 8. 日志、磁盘和稳定性

`webserver-user-service.sh` 增加：

- 启动前最小空闲空间检查，默认 512 MiB，可用 `WEB_PHASE11_MIN_FREE_KIB` 调整；
- 停机状态下日志超过 100 MiB 自动轮转，可用 `WEB_PHASE11_MAX_LOG_BYTES` 调整；
- `rotate` 命令先优雅停服、归档日志、再通过 readiness 启动；
- `operations.jsonl` 记录启动、停止和轮转等结构化运维事件。

有界巡检示例：

```bash
python3 scripts/phase11/phase16-soak.py \
  --duration 3600 --interval 1 \
  --output /home/pikachu/phase11-data/phase16-soak.jsonl
```

本阶段先运行 15 秒、30 次探测的 smoke，失败数为 0。脚本支持更长时间运行，但本次结果不能替代
24 小时或 7 天稳定性测试。

## 9. 文件索引

| 文件 | 作用 |
|---|---|
| `server/phase11/runtime/BoundedExecutor/` | 有界任务队列及低竞争快照 |
| `server/phase11/storage/postgres/PostgresConnectionPool/` | 连接池等待、超时和水位指标 |
| `server/phase11/business/OutboxDispatcher/` | 发布结果和最老事件年龄 |
| `server/phase11/transport/realtime/Phase11Realtime/` | Outbox Pump 主动唤醒、快照与 WS 提交通知 |
| `server/phase11/transport/http/Phase11SocialChatHttp/` | HTTP 写操作提交后的统一通知 |
| `main.cpp` | 装配回调、readiness 和 Prometheus 输出 |
| `benchmarks/phase15_business_benchmark.py` | 采集前后应用指标 |
| `benchmarks/phase15_aggregate.py` | 聚合指标增量中位数 |
| `scripts/phase15-business-benchmark.sh` | A/B 环境变量和证据记录 |
| `scripts/phase11/postgres-migration-status.sh` | 只读迁移审计 |
| `scripts/phase11/postgres-backup.sh` | 备份、校验、隔离恢复演练 |
| `scripts/phase11/phase16-soak.py` | 有界 liveness/readiness/metrics 巡检 |
| `scripts/phase11/webserver-user-service.sh` | readiness 启动门禁、磁盘和日志边界 |

## 10. 验证结果与限制

- Linux Release：93/93；Debug：93/93；ASan + LSan + UBSan：93/93。
- 轮询和主动唤醒 A/B：各三轮，业务错误均为 0。
- 15 秒稳定性 smoke：30/30 成功。
- PostgreSQL 停止/恢复：liveness 200，readiness 503 → 200，无需重启 Web 进程。
- 迁移审计、备份 SHA-256、`pg_restore --list` 和隔离恢复均通过。

完整运维验收摘要见 [ops-validation.md](../../evidence/phase16/ops-validation.md)。

当前仍是单机学习与预生产基线：缺少外部指标存储和告警、完整应用 JSON 日志、WAL
时间点恢复、多实例 Outbox 竞争协调以及长时间故障注入。它适合学习可观测性、回归和单机部署演练；
关键生产业务还要由 systemd/Kubernetes、Prometheus/Alertmanager、集中日志和异机备份补齐外围系统。

Phase 25 已补充不依赖安装的用户态自动进程监督、指数退避和重启预算；systemd/Kubernetes 外层监督、
集中告警和正式进程故障演练仍属于未完成边界，详见 [Phase 25](phase25.md)。

### Phase 23 后续增强

Phase 23 在保留本节历史 15 秒 smoke 结果的基础上升级了同一脚本：增加固定服务 PID 与启动时钟、Linux `/proc`
资源采样、RSS 增长门限、P50/P95/P99/max 延迟、原始 JSONL SHA-256 和原子汇总文件。固定 PID 消失、
被替换或无法采样时，探测即使还能访问相同端口也会判为失败。新的 24 小时运行方法和结果边界见
[Phase 23](phase23.md#6-长稳证据采集)；在汇总生成前，不能把正在运行写成已经通过。

## 11. 下一步学习建议

下一阶段进入文件消息时，应先坚持本阶段形成的边界：文件元数据进 PostgreSQL，文件字节进入对象存储
抽象；上传、扫描、下载和聊天消息分别可观测，并设置大小、并发、类型和配额限制。理解完本阶段后再
开始文件模块，可以避免把大文件直接塞进 Reactor、WebSocket 帧或数据库行。
