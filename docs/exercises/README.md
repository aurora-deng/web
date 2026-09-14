# 架构习题卡（学习项目）

已知技术债刻意保留或标出，方便当作练习。建议：**先写复现测试，再修**。

当前实现事实源：[`../architecture/phase4_upgrade.md`](../architecture/phase4_upgrade.md)。

---

## EX-1：`zombieWakes` 单槽覆盖

| 项 | 内容 |
|---|---|
| 现象 | Main 与 Writer 同时处于 `EXECUTE`，`fd_close` 后可能只唤醒其中一个 |
| 位置 | `server/SubReactor/SubReactor.cpp` → `zombieWakes[connId] = h` |
| 考察点 | 双协程生命周期、map value 设计 |
| 修复思路 | 按 `(connId, CoroutineRole)` 或 `array<handle, Role::Count>` 存 |
| 验收 | 补 lifecycle 测试：双 role 同时 EXECUTE 关闭后均被 schedule |

---

## EX-2：`waitingTicket` 单值

| 项 | 内容 |
|---|---|
| 现象 | 同一连接不能同时等多个 HTTP 响应完成（无真正 pipeline） |
| 位置 | `server/transport/Connection.h` → `waitingTicket` |
| 考察点 | 顺序票据、多 in-flight |
| 修复思路 | `deque<uint64_t> waitingTickets` 或按请求作用域 |
| 验收 | 同连接连续入队两个 `HttpStreamTask`，Main 可按序完成 |

---

## EX-3：死代码识别（已部分清理）

| 项 | 内容 |
|---|---|
| 现象 | 历史上存在 `StreamNotify`、`protocol()` 摆设等演进残渣 |
| 现状 | `StreamNotify` / `Session::protocol()` 已在本轮清理 |
| 考察点 | 学会用 grep 找「定义了却无人用」的 API |
| 练习 | `rg "TODO|FIXME|unused" server/` 并评估是否该删 |

---

## EX-4：Session 直接摸 `conns`

| 项 | 内容 |
|---|---|
| 现象 | `HttpSession::getConn` / `WebSocketSession::getConn` 写 `reactor->conns.find` |
| 位置 | 两个 Session 的 `getConn()` |
| 考察点 | 封装泄漏、窄接口 |
| 修复思路 | `SubReactor::findConnection(fd)` / `findConnection(fd, connId)`，`conns` 私有化 |
| 验收 | `rg "->conns" server/http server/websocket` 仅剩通过窄 API |

---

## EX-5：HttpStreamTask 背压计量

| 项 | 内容 |
|---|---|
| 现象 | Encoded 任务按字节计背压；大文件 HttpStream 可能计量不全 |
| 位置 | `server/transport/TransportWriter.cpp` |
| 考察点 | 背压维度（任务数 vs 字节 vs 文件） |
| 修复思路 | `remainingBytes()` 覆盖 FileBody；高水位统一计量 |
| 验收 | 慢客户端拉取大文件时触发 pauseByWrite，水位降后恢复 |

---

## 推荐学习路径

1. 读 `docs/debugging/CHEATSHEET.md`
2. 做 EX-4（小改动、立刻改善边界）
3. 做 EX-1（TDD：先写失败测试）
4. 可选：SSE 推送作为架构验收练习
