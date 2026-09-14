# 排错手册（症状 → 文件）

当前出站事实源：`OutboundTask` + `OutboundQueue` + `TransportWriter` + `writerLoop`。  
**不要**再查 `ResponseSender` / `WebSocketSender`（已删除）。

## 快速 grep

```bash
# 连接为何关闭
rg "\[CLOSE\]"   # 日志格式: fd= connId= reason= initiator=

# 出站入队/拒绝
rg "enqueueOutbound|Backpressure|outbound queue" server/

# 协议升级
rg "handoffWebSocket|createWebSocketSession" server/
```

## 症状对照表

| 症状 | 先看 | 其次 |
|---|---|---|
| 读不到 / 半包卡住 | `HttpSession::readRequest`、`recvSocket` | `ReadAwaiter`、`pauseByMemory` |
| HTTP 响应不返回 | `sendResponse`、`SendCompletionAwaiter` | `writerLoop` ticket 推进 |
| 发不出 / 队列满 | `OutboundQueue::enqueue`、`TransportWriter::enqueue` | 写水位常量 |
| 发一半卡住 | `writerLoop`、`TransportWriteAwaiter` | `updateEvent` / EPOLLOUT |
| WS 升级失败 | `prepareWebSocketUpgrade`、`handoffWebSocket` | `SessionFactory` 是否注入 |
| WS 私聊不到 | `WebSocketSessionManager::sendText` | `ReactorGroup::postOutbound`、connId |
| 心跳误杀 | `WebSocketSession::onTimeout` | `refreshIdleTimer`、waitingPong |
| 莫名断连 | 日志 `[CLOSE]` reason/initiator | `fd_close` 全部调用点 |
| 协程泄漏嫌疑 | `zombieWakes`、`processComplete` | EX-1 |

## 模块权责（一句话）

| 模块 | 干什么 |
|---|---|
| `SubReactor` | epoll、conns（私有）、唤醒、fd_close；对外 `findConnection` |
| `OutboundQueue` | 跨线程投递 + enqueue；Session 经 SubReactor 转发 |
| `TransportWriter` | 唯一写 socket |
| `HttpSession` | HTTP 状态机；不 new WS、不摸 conns |
| `WebSocketSession` | 帧循环 + onTimeout 心跳 |
| `SessionFactory` | 创建 WS 会话（唯一知 manager/dispatcher） |

## 架构文档怎么选

| 文档 | 用途 |
|---|---|
| `phase4_upgrade.md` | **当前实现**（优先） |
| `ARCHITECTURE.md` | 历史 HTTP 流程（已标注过期） |
| `docs/exercises/README.md` | 已知债练习 |
