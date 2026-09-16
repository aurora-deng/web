# 第十三阶段：指数退避、确定性抖动与投递指标

## 1. 本阶段解决什么问题

固定时间重试像广播通知所有快递员“整点一起出发”：当数据库、网络或大量客户端同时恢复时，
成千上万条消息会在同一毫秒再次冲击系统。这叫重试风暴或惊群。

本阶段加入三项升级：

1. 重试等待时间随 attempt 指数增长；
2. 每条消息加入稳定的伪随机抖动，错开出发时间；
3. `WebSocketDeliveryService` 暴露累计计数和当前水位，并由 `/delivery-metrics` 输出 JSON。

## 2. 指数退避怎样计算

配置项为：

```cpp
retryDelay = 500ms;          // attempt 2 前的初始退避
maxRetryDelay = 30000ms;     // 最大基础退避
retryJitterPercent = 20;     // ±20%
```

下一次尝试编号为 `n` 时，基础时间为：

```text
base(n) = min(retryDelay × 2^(n-2), maxRetryDelay)
```

关闭抖动后的例子：

| 下一次 attempt | 基础等待 |
|---:|---:|
| 2 | 500 ms |
| 3 | 1000 ms |
| 4 | 2000 ms |
| 5 | 4000 ms |
| 后续 | 最多 30000 ms |

默认 `maxAttempts=3`，所以默认只会使用前两级；调高尝试次数后上限才更明显。

## 3. 抖动为什么不能省

假设一万条连接同时断开，它们的 attempt、ACK 超时和固定 retryDelay 全部相同。即使使用指数退避，
它们仍会排着整齐方阵在 0.5 秒、1 秒、2 秒后同时冲击服务器。

抖动把每条消息的等待时间散到一个区间。例如基础 1000ms、抖动 20%：

```text
实际等待约为 800ms ～ 1200ms
```

2.0 使用 `(serverMessageId, nextAttempt)` 的稳定 FNV-1a 哈希产生抖动。它有三个特点：

- 不需要所有线程争抢一个随机数发生器；
- 同一个 ID 和 attempt 的测试结果可重复；
- 不同消息大概率落在不同时间点。

这是负载打散算法，不是密码学随机数，不能用于令牌或安全密钥。

## 4. 为什么超时后分成两个阶段

旧流程在 ACK 或传输超时后立即生成下一次发送：

```text
AwaitingAck --timeout--> AwaitingTransport(attempt+1)
```

现在改为：

```text
AwaitingAck --timeout--> RetryScheduled
                              │ 等待 backoff + jitter
                              ▼
                    AwaitingTransport(attempt+1)
```

`collectDue(now)` 第一次遇到超时，只记录超时类型并安排退避；到达退避截止点后，第二次调用才增加
attempt 并返回 `DeliveryRetry`。这样状态名和真实行为一致，也避免一次扫描内连续重试。

即时的 `Closed`、`Backpressured`、`Stale` 和 `WriteError` 同样进入 `RetryScheduled`。最后一次尝试
超时或失败时直接进入 `Failed`，不会再多等一轮无意义的退避。

## 5. 指标为什么属于 Service

Tracker 是纯状态机，负责决定状态；Service 连接发送回执、定时器和 ACK，因此只有 Service 能看见
完整事实：发送调用次数、最终写结果、旧 attempt 回执、ACK 分类及通知结果。

如果把指标散在日志里，统计程序只能解析文本；如果把它放进 Tracker，Tracker 又会知道过多 I/O
细节。Service 是最自然的观测边界。

## 6. 累计计数和当前水位

`WebSocketDeliveryMetricsSnapshot` 分成两类：

### 累计计数 counter

只增加，用于计算一段时间内发生了多少事件：

- `submissions`、`newMessages`、`duplicateSubmissions`、`rejectedSubmissions`；
- `attemptsDispatched`、`retriesDispatched`；
- `writtenAttempts`、`retryableAttemptFailures`、`permanentAttemptFailures`；
- `retrySchedules`、`transportTimeouts`、`ackTimeouts`；
- `ackRequests`、`acknowledgedMessages`、`duplicateAcks`、`rejectedAcks`；
- `failedMessages`、`ignoredAttemptResults`。

### 当前水位 gauge

可以上升或下降，描述读取瞬间还有多少工作：

- `pendingMessages`：尚未终态的可靠消息；
- `observedReceipts`：正在等待最终结果的出站回执。

可以把 counter 想成汽车总里程表，把 gauge 想成当前油量表。

## 7. 为什么指标使用 relaxed atomic

每个累计计数使用 `std::atomic<uint64_t>`，更新和读取采用 `memory_order_relaxed`。这些原子变量只负责
防止计数本身发生数据竞争，不用于发布 Tracker 状态，也不决定消息能否发送。

因此不需要 acquire/release 建立额外的线程先后关系。真正的投递状态仍由 Tracker、观察表和生命周期
锁保护。一次 metrics 快照中的不同字段可能相差一个并发事件，这是监控数据允许的近似，而状态机
决策不能依赖这份快照。

## 8. HTTP 观测面

`GET /delivery-metrics` 返回：

```json
{
  "counters": {
    "attemptsDispatched": 42,
    "retriesDispatched": 3,
    "ackTimeouts": 2,
    "failedMessages": 1
  },
  "gauges": {
    "pendingMessages": 4,
    "observedReceipts": 2
  }
}
```

真实响应包含全部字段。示例直接暴露路由便于学习；生产环境应限制到管理网络或加鉴权，再由
Prometheus exporter、日志采集器等转换成正式监控格式。

## 9. 怎样读这些指标

几个有用的组合：

```text
重试比例 ≈ retriesDispatched / attemptsDispatched
传输故障比例 ≈ retryableAttemptFailures / attemptsDispatched
ACK 超时趋势 = ackTimeouts 在单位时间内的增量
积压趋势 = pendingMessages 是否持续上升
```

- `transportTimeouts` 上升：最终回执链或 Writer/连接关闭路径可能卡住。
- `ackTimeouts` 上升但 `writtenAttempts` 正常：网络已写出，客户端处理或 ACK 链更可疑。
- `retryableAttemptFailures` 上升：离线、连接换代或背压较多。
- `ignoredAttemptResults` 上升：旧 attempt 的迟到回执较多，应检查传输延迟和 timeout 是否过短。
- `pendingMessages` 持续上升：进入速度长期高于 ACK/失败收敛速度。

单个累计值不能直接报警，应观察一段时间内的增量或速率。

## 10. 与上传版 web-test6.1 的关系

上传版关注 Reactor 是否能高效搬运连接和 HTTP 响应；当前 2.0 开始处理故障恢复对系统整体负载的
反作用。指数退避保护的是全局容量，抖动保护的是时间分布，指标则把隐藏的异步状态变成可观察事实。

机器人网关同样需要这些机制：设备断电后可能同时上线；若所有命令立即同步重发，串口、无线链路或
控制服务会再次被压垮。退避和抖动让恢复过程逐渐展开。

## 11. 优点、代价与限制

优点：

- 临时故障持续越久，重试频率越低；
- 不同消息错开重试，降低瞬时峰值；
- 哈希抖动无需共享随机锁，测试可重复；
- 指标区分传输超时、ACK 超时、失败和当前积压。

代价和限制：

- 重试成功的平均等待时间会增加；
- 确定性抖动不是安全随机数；
- 指标目前只保存在进程内，重启会清零；
- 没有直方图，暂时看不到 ACK 延迟的 P50/P95/P99；
- 投递记录仍未持久化，进程重启后不能继续旧任务。

## 12. 实践与理解检查

练习一：关闭 jitter，令初始退避 100ms、上限 250ms，写出 attempt 2～6 的等待时间。

练习二：让 100 条消息使用相同基础退避，计算加入 ±20% 抖动后可能分布在哪个时间区间。

练习三：构造“Written 正常增长但 ackTimeouts 快速增长”的情形，判断先检查服务端 Writer 还是客户端。

理解检查：

1. 指数退避为什么仍需要抖动？
2. 为什么超时后先进入 RetryScheduled，而不是立即递增 attempt？
3. `retryableAttemptFailures` 和 `ackTimeouts` 分别说明哪一段链路有问题？
4. 为什么 metrics 可以使用 relaxed atomic，而 Tracker 状态不能只用一组 relaxed atomic？
5. 为什么 `pendingMessages` 是 gauge，`failedMessages` 是 counter？
