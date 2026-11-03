# 第八阶段：WebSocket 背压、慢客户端与跨线程邮箱

背压解决一个非常现实的问题：**业务生产消息的速度，大于 socket 把消息发出去的速度时，
谁来踩刹车？**

如果没有明确答案，消息只会不断堆在内存里，最终一个不读数据的客户端就可能拖垮整个服务。

## 1. 用水池理解背压

```text
业务消息 = 不断流入的水
出站队列 = 水池
socket 发送 = 排水口
```

排水口变慢时有四种选择：

1. 无限扩建水池：最终 OOM，不可接受。
2. 静默丢消息：系统还活着，但调用方以为发送成功。
3. 暂停上游读取：让 TCP 窗口逐步把压力传回客户端。
4. 明确拒绝或关闭慢连接：保护其他连接。

2.0 现在组合使用后两种方案，并把拒绝原因返回给调用者。

Python 中最小的背压模型类似：

```python
from queue import Queue, Full

mailbox = Queue(maxsize=3)
try:
    mailbox.put_nowait(message)
except Full:
    # 明确执行“拒绝最新消息”的策略
    report_backpressure()
```

关键并不是 `Queue` 这个类，而是队列必须有界，`put` 必须有可观察的失败结果。

## 2. 为什么需要两级背压

2.0 的跨线程主动推送会经过两个队列：

```mermaid
flowchart LR
    A[业务/其他 Reactor] -->|postOutbound| B[Reactor 跨线程邮箱]
    B -->|eventfd 唤醒| C[processPending]
    C --> D[目标 Connection 出站队列]
    D --> E[writerLoop]
    E --> F[kernel socket buffer]
    F --> G[客户端]
```

原有代码只限制了 `Connection.outboundQueue`。这相当于仓库内部货架有限，但仓库门外的卡车
可以无限排队：货还没进入货架，门外队伍就已经耗尽内存。

因此本阶段新增 `OutboundAdmission`，先限制跨线程邮箱，再由 `TransportWriter` 限制目标连接。

## 3. 两级水位的具体数值

### 3.1 Reactor 跨线程邮箱

| 范围 | 任务上限 | 待发送 wire 字节上限 |
|---|---:|---:|
| 单个 Reactor 邮箱 | 16384 | 64 MiB |
| 单个目标连接在邮箱中 | 256 | 4 MiB |

全局限制保护 Reactor；单连接限制防止一个慢客户端占满所有人的邮箱。

广播使用共享字符串减少数据复制，但邮箱仍按“每个目标需要发送多少字节”计费。因为共享 1 MiB
内存不代表网络只需发送 1 MiB：广播给 1000 个用户仍然产生约 1000 MiB 的发送工作。

### 3.2 Connection 出站队列

| 配置 | 数值 | 作用 |
|---|---:|---|
| `kWriteHighWatermark` | 4 MiB | 新任务若会越线则拒绝并暂停读 |
| `kWriteLowWatermark` | 2 MiB | 写积压降到此处后恢复读 |
| `kMaxOutboundTasks` | 4096 | 防止大量小任务绕过字节限制 |

高水位和低水位不设成同一个数，是为了形成**迟滞区间**。像空调不会在 25.000℃ 开、
24.999℃ 关，否则状态会频繁抖动；写队列也要等积压真正降下来再恢复读取。

## 4. `OutboundAdmission` 为什么独立成模块

`OutboundQueue` 同时涉及 mutex、eventfd、Connection 查表和 Writer。如果把所有水位判断继续
塞进去，测试一个大小比较也必须启动系统 I/O 环境。

现在职责拆成：

- `OutboundAdmission`：只计算“还能不能接单”，保存全局和单连接配额。
- `OutboundQueue`：加锁、保存任务、合并 eventfd 唤醒。
- `TransportWriter`：限制连接队列并执行真实 socket 写入。
- `SessionManager`：把准入结果返回业务层。

纯准入策略不依赖 socket，可直接用普通 C++ 程序测试全部边界。

## 5. 返回值为什么不能再用 `bool`

现在统一使用：

```cpp
enum class EnqueueResult {
    Ok,
    Backpressure,
    Closed,
    Invalid
};
```

| 结果 | 含义 | 业务可以怎么做 |
|---|---|---|
| `Ok` | 当前一级已经接单 | 返回 accepted，或继续等待更强确认 |
| `Backpressure` | 邮箱或连接队列已满 | 稍后重试、降级、丢弃或关闭慢连接 |
| `Closed` | Reactor/连接/会话已失效 | 清理在线状态 |
| `Invalid` | 连接身份或编码结果非法 | 修复调用逻辑 |

`Ok` 不是“对端已经收到”。它可能只表示任务进入目标 Reactor 的跨线程邮箱。真正的交付强度
可以分为：

```text
accepted by mailbox
    < queued on connection
    < written to kernel
    < received by peer
    < processed by peer application
```

越往右，需要的确认协议越强，成本也越高。

## 6. 慢客户端发生了什么

假设客户端 B 停止读取：

1. 内核 socket 发送缓冲逐渐写满。
2. `writev` 返回 `EAGAIN`，writerLoop 挂起等待 `EPOLLOUT`。
3. B 的 `pendingWriteBytes` 继续增加。
4. 下一条任务会越过 4 MiB 时，Writer 返回 `Backpressure`。
5. `pauseByWrite=true`，`updateEvent()` 撤销 `EPOLLIN`，不再继续读取 B 的新消息。
6. 主动广播采用“丢最新任务并记录本批拒绝数”。
7. 若 B 正在请求服务器产生回复，WebSocketSession 尝试发送 Close 1013 后结束会话。

这里的重点是：慢 B 只影响 B 自己的配额，不应该吃光 A、C、D 的邮箱空间。

## 7. 为什么关闭尾包可以越过连接水位

普通任务满了以后，如果 Close 帧也被同一水位拒绝，连接会卡在“知道该关，却无法发送关闭
通知”的状态。因此带 `CloseConnection` 的尾包拥有连接队列的应急通道。

它仍排在已有数据后面，尽量完成正常关闭握手；如果对端始终不读，时间轮最终负责强制收尾。

## 8. 字节上限为什么还要配任务数上限

只限制 4 MiB 不够。攻击者可以排入几十万个极小消息，每条 payload 只有 1 字节，但每个
`OutboundTask`、deque 节点、shared_ptr 和调度元数据都要占内存。

因此必须同时控制：

```text
总字节数：防大包
总任务数：防海量小包
```

这也是网络系统常见的“双维度配额”。

## 9. 本阶段修复的隐藏问题

原来的字节判断是：

```cpp
pendingWriteBytes + incomingBytes > highWatermark
```

无符号整数相加可能溢出后变成很小的数。本阶段改成：

```cpp
incomingBytes > highWatermark ||
pendingWriteBytes > highWatermark - incomingBytes
```

先排除单项过大，再做减法，避免溢出绕过限制。同时删除了“队列为空时无条件接受第一个超大
encoded 任务”的例外，使 4 MiB 真正成为上限。Close 尾包仍有单独的应急语义。

## 10. 业务层现在能看见什么

`WebSocketSessionManager::sendText()` 会把 `EnqueueResult` 原样返回。示例聊天 handler 给发送方
返回：

| 结果 | 回执文本 |
|---|---|
| `Ok` | `accepted` |
| `Backpressure` | `target busy` |
| `Closed` | `target unavailable` |
| `Invalid` | `invalid outbound message` |

`accepted` 这个词刻意表示“系统接单”，没有冒充端到端送达确认。

## 11. 与上传 web-test6.1 的关系

上传版本以 HTTP ResponseSender 为主，没有 WebSocket SessionManager、跨 Reactor 广播和两级
出站邮箱。它帮助你理解单连接的响应发送；2.0 把问题扩展成：

```text
多协议共用 Writer
+ 多线程向目标 Reactor 投递
+ 一个消息广播给许多连接
+ 每个连接的发送速度不同
```

所以 2.0 不能只依靠旧版“发送失败就关闭当前 HTTP 连接”的策略，还需要邮箱准入、单连接
隔离和业务可见的拒绝结果。

## 12. 本阶段验证矩阵

| 场景 | 预期 |
|---|---|
| 单连接邮箱超过 2 条测试上限 | Backpressure |
| Reactor 邮箱超过 3 条测试上限 | Backpressure |
| 被拒任务 | 不消耗配额 |
| drain 完成 | 邮箱配额归零 |
| 相同 fd、不同 connId | 使用独立单连接配额 |
| 超大首个 encoded 任务 | Backpressure，不再绕过上限 |
| size_t 极大输入 | 不通过溢出绕过 |
| CloseConnection 尾包 | 可进入连接队列 |
| 旧 connId 的迟到任务 | drain 时丢弃，不发给复用 fd |
| 私聊在线目标 | 目标收到消息，发送方收到 accepted |
| 私聊离线目标 | 发送方收到 target unavailable |

准入策略已在当前环境实际运行；Linux transport/unit 和完整 socket 黑盒用例已经补齐，等待
Linux 构建环境执行。

## 13. 后续阶段进展

上述两项限制已在[第九阶段](lesson09_outbound_completion_and_fairness.md)处理：可选
`OutboundReceipt` 保存任务离开邮箱后的最终状态，Writer 使用每轮 256 KiB 的统一写预算约束
WebSocket、HTTP 内存体和 sendfile 路径。

## 14. 理解检查

1. 为什么 Connection 队列已有 4 MiB 上限，跨线程邮箱仍必须单独设上限？
2. 为什么广播共享一份字符串后，仍要按每个目标的 wire 字节计算配额？
3. 高水位 4 MiB、低水位 2 MiB 比都设为 4 MiB 好在哪里？
4. `Ok` 为什么不能直接翻译成“客户端已收到”？
5. 为什么 CloseConnection 尾包需要应急通道？
