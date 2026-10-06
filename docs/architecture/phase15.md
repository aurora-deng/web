# Phase 15：真实业务性能基线

Phase 15 不再用 `/fast` 的空响应代表整个聊天系统，而是从真实网络入口驱动账号、Cookie/CSRF、
PostgreSQL、事务 Outbox、WebSocket 和 SSE。它回答的是“用户完成一次业务动作要多久”，不是
“事件循环最多能回多少个固定字符串”。

可以把两类测试想成汽车测试：`/fast` 像发动机台架，能看网络热路径上限；本阶段像整车道路测试，
刹车、变速箱、轮胎和载重都会进入结果。两者都需要，但不能互相替代。

## 1. 本阶段增加的内容

| 文件 | 作用 |
|---|---|
| [phase15_business_benchmark.py](../../benchmarks/phase15_business_benchmark.py) | 只用 Python 标准库驱动 HTTP、WebSocket、SSE 和真实业务 |
| [phase15_aggregate.py](../../benchmarks/phase15_aggregate.py) | 聚合多轮结果，以中位数减少单轮抖动 |
| [phase15-business-benchmark.sh](../../scripts/phase15-business-benchmark.sh) | 建立隔离数据库、独占启动 18091 服务、运行三轮并恢复 18090 正式服务 |
| [summary.json](../evidence/phase15/20261006T131729Z/summary.json) | 三轮汇总证据 |
| [environment.txt](../evidence/phase15/20261006T131729Z/environment.txt) | 内核、CPU、PostgreSQL、Python 和二进制 SHA-256 |

脚本不会安装依赖，也不会把测试账号写入正式 `phase11` 数据库。它只允许重建名称以
`phase15_benchmark` 开头的专用数据库，防止参数写错后删除正式库。

## 2. 测量链路

```mermaid
flowchart LR
    Load[标准库压测器] --> HTTP[HTTP API]
    Load --> WS[WebSocket]
    Load --> SSE[SSE]
    HTTP --> Business[Auth / Social / Chat]
    WS --> Business
    Business --> DBExec[DatabaseExecutor]
    DBExec --> PG[(PostgreSQL)]
    PG --> Outbox[(outbox_events)]
    Outbox --> Pump[Outbox Pump]
    Pump --> WS
    Pump --> SSE
    Load --> Proc[/proc 资源采样]
```

消息测试同时记录两个时间点：

1. `databaseAcceptedLatency`：服务端完成消息和 Outbox 的同一事务，并向发送者返回
   `chat.accepted`；相当于快递已经可靠入仓。
2. `end-to-endLatency`：接收者 WebSocket 真正收到 `chat.message`；相当于包裹送到对方门口。

两个数字分开后，数据库慢和实时发布慢不会混成一个模糊的“WebSocket 慢”。

## 3. 业务场景

| 场景 | 完整行为 | 吞吐单位 |
|---|---|---|
| 登录 | 密码校验、Session 写库、Cookie/CSRF 返回 | 登录/秒 |
| 会话列表 | Session 校验、成员关系与摘要查询 | 查询/秒 |
| 私聊端到端 | WS 入站、事务写库、Outbox、接收端 WS | 消息/秒 |
| 8 人群发 | 一次持久化后向 8 个在线成员送达 | 消息/秒与收件人投递/秒 |
| 离线历史 | 对方离线时写入 50 条，再通过 HTTP 拉取 | 页面/秒与消息/秒 |
| WebSocket 重连 | Cookie 鉴权、Upgrade、注册、Close | 握手/秒 |
| SSE 重连 | Cookie 鉴权、chunked 响应、`ready` 事件 | 就绪连接/秒 |
| 混合负载 | 50% 会话列表、20% 历史、30% 实时发送 | 业务操作/秒 |

测试器会跳过无关的 Presence 等实时事件，只在 `clientMessageId` 匹配时确认目标消息，避免把事件
乱序误报成失败。SSE 的 `data` 按协议既支持 JSON 也支持普通 UTF-8 文本；`ready` 的
`data: connected` 因此不会被错误地强制解析为 JSON。

## 4. 2026-10-06 Linux 三轮结果

配置：4 个并发 Worker；每轮登录/列表/私聊/混合各 80 次，8 人群发 20 次，50 条历史读取
30 次，WS/SSE 重连各 30 次。三轮总错误数为 **0**。

| 场景 | 中位吞吐 | P50 | P95 | P99 |
|---|---:|---:|---:|---:|
| 登录 + Session 写入 | 10.08 次/s | 390.23 ms | 502.24 ms | 548.46 ms |
| 会话列表 | 111.83 次/s | 21.88 ms | 78.96 ms | 87.73 ms |
| 私聊端到端 | 12.72 条/s | 310.36 ms | 376.85 ms | 446.31 ms |
| 8 人群发全部到达 | 3.62 条/s，28.95 次收件/s | 278.66 ms | 316.47 ms | 323.74 ms |
| 50 条离线历史 | 72.45 页/s，3622.41 条/s | 7.97 ms | 42.20 ms | 63.52 ms |
| WebSocket 握手 | 85.89 次/s | 28.20 ms | 169.35 ms | 186.15 ms |
| SSE ready 重连 | 139.27 次/s | 15.77 ms | 70.89 ms | 103.94 ms |
| 50/20/30 混合负载 | 32.83 次/s | 27.71 ms | 346.53 ms | 406.53 ms |

进程资源三轮峰值的中位数：RSS **152,508 KiB**、fd **65**、线程 **20**；时间窗平均 CPU
中位数 **69.19%**，峰值 **188.64%**。多线程进程超过 100% 表示同时使用了多个逻辑核。

原始记录保存在：

- [run-1.json](../evidence/phase15/20261006T131729Z/run-1.json)
- [run-2.json](../evidence/phase15/20261006T131729Z/run-2.json)
- [run-3.json](../evidence/phase15/20261006T131729Z/run-3.json)

## 5. 如何读这些数字

### 5.1 登录较慢不等于 Reactor 慢

登录必须执行抗暴力破解的密码散列校验。它像银行金库故意把开锁过程做得更贵，目标是增加攻击者
批量猜密码的成本。因此登录不会追求 `/fast` 那种万级 QPS；后续应通过登录限流、CPU 预算和独立
认证容量规划控制，而不是削弱密码算法。

### 5.2 当前最大差距在 Outbox 等待

私聊事务确认的三轮 P50 中位数是 **32.16 ms**，但接收端 P50 是 **310.36 ms**；8 人群发事务
确认 P50 是 **26.41 ms**，全部送达 P50 是 **278.66 ms**。代码中的 Outbox Pump 在队列未满时
固定等待 250 ms，测量差值与这个周期高度吻合。

这是一条定位结论，不是因果证明。下一阶段应增加 Outbox backlog、发布时间和数据库执行队列指标，
再实现“事务提交后主动唤醒 + 低频兜底轮询”，用同一套测试做前后对照。不能直接把轮询改成 1 ms，
因为那会让空闲服务每秒查询数据库上千次，只是用数据库 CPU 换表面延迟。

### 5.3 这不是容量上限

本轮固定并发为 4，单机同时运行服务和负载发生器，也没有逐级提高到饱和点，所以这些数值是可复现
基线，不是最大 QPS。要找容量上限，应按并发 1、4、8、16 逐级测试，并提前定义错误率和 P99 门槛。

## 6. 为什么本轮没有混入 AI 数字

基准使用全新的隔离数据库，里面没有正式库的 AI 账号与模型绑定。ONNX 生成还强烈依赖具体模型、
输出 token 数、CPU 指令集和并发队列；把它塞进聊天基线会让网络、数据库和模型推理三个变量混在一起。

测试器已经保留 `--ai-user-id`、TTFT、总生成耗时和 token/s 能力。等 AI 治理阶段建立专用模型夹具后，
再单独运行 AI 矩阵，并明确模型、版本、Adapter、prompt、最大 token 和冷热缓存状态。

## 7. 复现

在 Linux 仓库根目录执行：

```bash
./scripts/phase15-business-benchmark.sh
```

脚本会：

1. 记录环境和 Release 二进制 SHA-256；
2. 暂停 18090 正式服务；
3. 重建专用 `phase15_benchmark` 数据库并执行全部迁移；
4. 在 18091 独占启动相同 Release 二进制；
5. 连续运行三轮并聚合中位数；
6. 无论成功或失败都停止基准进程并恢复 18090 正式服务。

可通过 `PHASE15_WORKERS`、`PHASE15_OPERATIONS`、`PHASE15_RUNS` 等环境变量调整规模。结果默认写入
`artifacts/benchmarks/phase15/<UTC 时间>/`，该运行目录不提交 Git；需要长期保留的证据应像本阶段
一样复制到 `docs/evidence/`。

## 8. 适用场景、优点与限制

适用场景：聊天业务回归、架构改动前后对照、定位持久化与推送之间的等待、发布前容量摸底。

优点：不需要第三方压测库；通过真实协议与业务层；隔离正式数据；同时保留原始数据、尾延迟、错误和
进程资源。

限制：标准库客户端不是高压负载发生器；单机测试存在资源竞争；目前没有固定到达率；本阶段还没有
数据库池等待、执行器队列和 Outbox backlog 的内部指标；AI 结果未纳入本轮结论。

## 9. 下一步学习建议

下一阶段先补可观测性和运维闭环：理解“指标是服务器的仪表盘”，为数据库池、执行队列、Outbox
积压和端到端发布延迟增加有界、低竞争的指标；再做主动唤醒并复跑 Phase 15。理解完这一阶段后，
再进入文件消息和 AI 治理，避免在基础链路不可观察时继续堆功能。
