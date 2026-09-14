# 第九阶段：发送完成语义与 Writer 公平性

这一阶段回答两个容易混在一起的问题：

1. `sendText()` 返回 `Ok` 时，到底成功到了哪一步？
2. 一个大文件或消息很多的连接，为什么不能一直占着 Reactor 写？

## 1. 准入结果与最终结果是两张单

把跨 Reactor 发送想成寄快递：

- `EnqueueResult` 是收件窗口的小票：窗口有没有接下包裹。
- `OutboundReceipt` 是物流回执：包裹最终在运输链哪一步结束。

```text
业务线程
  │ sendTextTracked
  ▼
跨线程邮箱 ── Ok ──► Connection 队列 ──► Writer ──► 内核 socket 缓冲
  │                    │                    │
  ├─ Backpressured     ├─ Backpressured     ├─ WriteError
  ├─ Invalid           ├─ Stale             └─ Written
  └─ Closed            └─ Closed
```

因此 `Ok` 与 `Written` 不能互换：`Ok` 可能只表示任务进入邮箱，此后仍可能因为 fd 代际失效、
连接关闭或第二级队列背压而失败。

## 2. `OutboundOutcome` 的精确定义

| 状态 | 含义 | 典型发生位置 |
|---|---|---|
| `Pending` | 任务仍在发送链上 | 邮箱或 Connection 队列 |
| `Written` | 本任务全部字节已交给本机内核 socket 缓冲 | `TransportWriter::completeFront` |
| `Backpressured` | 邮箱或 Connection 队列拒绝新任务 | `post` / `enqueue` |
| `Closed` | Reactor 或连接关闭，未完成任务被取消 | `fd_close` / 队列析构 |
| `Stale` | fd 还存在，但 `connId` 已不是投递时的连接 | `processPending` |
| `Invalid` | 目标身份或编码字节非法 | Manager / ReactorGroup |
| `WriteError` | `writev` / `sendfile` 发生真实系统错误 | `writerLoop` 错误收尾 |

`Written` 的边界必须说准确：它只证明内核接收了字节，不证明网络送达，更不证明对端业务已经处理。

```text
邮箱接单 < 连接排队 < 写入本机内核 < 对端 TCP 收到 < 对端应用处理
```

如果聊天业务需要“已读”，仍要在 WebSocket 应用协议中加入消息 id 和客户端 ACK。传输层不能猜测
对端业务是否处理成功。

## 3. 回执为何只放原子状态，不在 Writer 中执行回调

`OutboundReceipt` 内部是一个原子终态，并且只允许第一次完成成功：

```cpp
auto receipt = std::make_shared<OutboundReceipt>();
auto task = OutboundTask::encoded(
    bytes, 0, OutboundCompletion::None, receipt);

postOutbound(fd, connId, std::move(task));

// 其他线程可在适当的调度点观察；不能把它当忙等循环使用。
if (receipt->outcome() == OutboundOutcome::Written) {
    // 字节已写入本机内核
}
```

不让 Writer 直接调用业务回调有三个原因：

1. 回调可能很慢，会把 Reactor 卡住。
2. 回调可能再次发送或关闭连接，形成难以推理的重入。
3. 回调在哪条线程执行容易被误解，进而访问了错误线程拥有的数据。

当前回执适合监控、测试和由上层调度器定期收割结果。若以后需要 `co_await receipt`，应增加专门的
完成队列和 `eventfd` 唤醒，把续体调回它所属的线程，而不是给这个原子对象塞任意回调。

## 4. “终态只写一次”为什么重要

一张任务可能先遇到写错误，随后连接关闭。若后来的 `Closed` 能覆盖先前的 `WriteError`，排错信息就
丢了。`compare_exchange` 保证状态只能执行：

```text
Pending ──► 某一个终态
终态    ──x 任何其他终态
```

`OutboundTask` 析构时还会兜底写入 `Closed`。这像快递单离开发货系统前必须盖一个结果章，防止某条
异常分支忘记收尾，使回执永远停在 `Pending`。自定义移动赋值也会先关闭被覆盖的旧任务。

## 5. 关闭连接时为什么要先取消队列

`fd_close()` 擦除 `Connection` 后，队列中的 `OutboundTask` 会随对象一起析构。现在关闭流程在擦除前
显式调用：

```cpp
transportWriter.cancelAll(conn, OutboundOutcome::Closed);
```

系统写错误则先用 `WriteError` 取消，再进入通用关闭流程。因为终态只写一次，后续 `Closed` 不会覆盖
更具体的错误原因。

## 6. Writer 公平性：给每桌固定上菜时间

旧 `flush()` 会一直写到队列空或 `EAGAIN`。如果某连接一直可写并积压几十 MiB，它就像一桌客人
不断点菜，服务员可能一直服务这一桌，其他连接的读写和定时器只能等待。

现在每次 `flush` 默认只有 256 KiB 的 byte quantum：

```cpp
inline constexpr std::size_t kWriteQuantumBytes = 256 * 1024;

FlushResult result = writer.flush(conn); // 最多写一个 quantum
```

预算耗尽且队列还有任务时返回 `FlushStatus::Yielded`。`writerLoop` 重新武装 `EPOLLOUT` 并挂起，
让 epoll 回到所有连接的公平入口。下一次可写事件再继续当前偏移，不复制、不重发。

## 7. 为什么预算必须按“实际写出字节”扣

非阻塞 `writev` 可能请求写 64 KiB，内核只接受 7 KiB。预算应扣 7 KiB；若按请求量扣 64 KiB，
连接会过早让出，吞吐下降。反过来少扣会突破公平上限。

`FlushResult::bytesWritten` 保存本轮实际写出量，既能测试预算，也能成为以后吞吐指标的来源。

## 8. 为什么 HTTP、WebSocket、sendfile 必须共用预算

只限制 WebSocket 字节帧没有意义。一个大文件下载若仍在 `sendfile` 循环中一次写到底，同样会占住
Reactor。因此预算贯穿三条路径：

| 路径 | 限制位置 |
|---|---|
| WS / 已编码字节 | `flushEncoded` 构造 iovec 时限制总长度 |
| HTTP Header / 内存 Body | `buildSegments(block, remainingBudget)` |
| 文件 Body | `sendFile(fd, remainingBudget)` |

这体现了统一 Transport 层的价值：协议层只生产任务，公平、背压、错误和系统调用策略只实现一次。

## 9. `ChunkedBody` 暴露出的零拷贝难点

接入预算时发现，旧 `ChunkedBody::buildSegments(max)` 没有严格使用 `max`，还曾把 Segment 指向
函数内的 `prefix_copy` / `suffix_copy`。函数返回后局部字符串析构，Writer 再 `writev` 就可能读取
悬空地址。

修复后的原则是：

1. Segment 直接借用 `stream->chunks.front()` 中原始字符串和 Buffer 的地址。
2. prefix、data、suffix 三段共享同一份剩余预算，合计不超过 `max`。
3. `consume()` 只按内核实际写出的字节推进 `sent`。
4. `finish()` 将终止块入队和 `done=true` 放在同一把锁内，避免终止事件丢失。
5. `finished()` 与 `remain()` 同样加锁，保持生产者和 Writer 之间的数据竞争边界完整。

零拷贝不是“没有内存管理”，而是把复制成本换成了更严格的地址稳定性和所有权约束。

另一个容易漏掉的边界是 `CloseConnection`。`flushEncoded` 可以把多张任务合成一次 `writev`，但扫到
Close 任务后必须停止收集。否则 Close 后面刚到的任务可能已经被内核写出，Writer 却因关连接屏障
没有推进它的 offset，造成“实际写了、账上没写”的错位。现在 Close 任务同时是一道 writev 屏障，
后续任务由关闭流程标记为 `Closed`。

## 10. WebSocket 业务如何选择确认强度

| 场景 | 建议确认 |
|---|---|
| 在线人数广播、输入状态 | 只看 `EnqueueResult`，允许丢最新 |
| 运维指标、慢连接诊断 | 保留 `OutboundReceipt`，观察最终传输状态 |
| 私聊“已发送”标记 | 至少等 `Written`，并明确它不是送达 |
| 订单、支付、可靠通知 | 消息 id + 对端 ACK + 超时重试 + 幂等去重 |

回执更强会增加状态、内存和调度成本。业务应按消息价值选择，不应让所有心跳和广播都承担可靠消息
协议的成本。

## 11. 与上传 web-test6.1 的区别

上传版本的核心是 HTTP 单连接响应发送：调用者主要关心响应是否继续写、是否等待可写。2.0 还要处理
跨 Reactor WebSocket 主动推送，因此新增了两条维度：

- 空间维度：任务可能暂存在别的 Reactor 邮箱，必须用 `fd + connId` 防止发给复用后的连接。
- 时间维度：`Ok` 之后仍可能异步失败，需要独立于同步返回值的最终回执。

Writer 公平预算则把旧版“尽量一次写完”升级成“每轮写一份，再回到事件循环”。这通常会多一次 epoll
调度，但能显著缩短其他连接被大连接挡住的时间。

## 12. 优点、代价与适用场景

优点：

- 异步丢弃不再静默，故障位置可区分。
- 单连接的连续写时间有上界，尾延迟更稳定。
- HTTP、WebSocket 和文件发送遵守同一套公平规则。
- 原子回执没有业务回调重入 Writer 的风险。

代价：

- 跟踪发送会多一个 `shared_ptr` 和原子状态；普通无需跟踪的任务仍可不创建回执。
- 大响应会经历更多轮 epoll 唤醒，峰值吞吐与公平性之间需要基准测试调参。
- `Written` 仍不提供端到端可靠性；强可靠业务必须设计应用层 ACK。

适合长连接网关、聊天、推送、同时提供大文件下载的多连接服务。若只有少量短 HTTP 响应，回执与量子
调度的收益较小。

## 13. 验证点

1. 3 字节预算发送 8 字节任务：首轮 `Yielded + bytesWritten=3`，回执仍为 `Pending`。
2. 第二轮写完剩余 5 字节：返回 `Drained`，回执变为 `Written`。
3. HTTP Header 与 Body 也按小预算分多轮写完。
4. 高水位拒绝将回执置为 `Backpressured`，后续状态不能覆盖。
5. 连接关闭批量取消在途任务并清零队列计数。
6. 旧 `connId` 的邮箱任务得到 `Stale`。
7. Chunked 三段合计不超过调用者给出的预算，切片内容连续。
8. Close 任务后的迟到任务不会被同一次 `writev` 发出，并在关闭时得到 `Closed`。

## 14. 理解检查

1. 为什么 `EnqueueResult::Ok` 与 `OutboundOutcome::Written` 必须是两个值？
2. `Written` 为什么不能作为聊天消息“已读”的依据？
3. Writer 为什么按实际写出的字节扣预算？
4. 为什么不能只给 WebSocket 帧加预算，却让 `sendfile` 不受限制？
5. `OutboundTask` 析构兜底与“终态只写一次”分别防住什么问题？
6. `ChunkedBody` 的 Segment 为什么不能指向函数里的局部字符串副本？
