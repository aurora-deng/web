# 测试分层（学习目标）

| 层 | 文件 | 学什么 | 本层不测什么 |
|---|---|---|---|
| unit | `unit_tests.cpp` | Buffer / Parser / Router / WS 消息组装、Dispatcher 并发、Executor drain、取消/deadline 与双池隔离 | 不起 epoll、不起完整服务器 |
| component | `transport_tests.cpp` | OutboundTask / TransportWriter（socketpair） | 不起完整 SubReactor |
| lifecycle | `coroutine_lifecycle_tests.cpp` | 协程槽、关闭路径 | 不做全链路业务 |
| integration | `integration/*.py` | HTTP / WS / SSE 黑盒端到端 | 不测内部私有符号 |
| client unit | `python/reliable_websocket_consumer_test.py` | 接收端幂等、ACK 丢失与有界窗口 | 不启动服务器 |

事实源与排错：

- SSE 接入：`docs/architecture/phase5.md`
- 架构：`docs/architecture/websocket-lessons/lesson05_connection_identity_and_root_coroutine.md`、`docs/architecture/websocket-lessons/lesson06_websocket_frame_and_message_state.md`、`docs/architecture/websocket-lessons/lesson07_websocket_utf8_and_close.md`、`docs/architecture/websocket-lessons/lesson08_websocket_backpressure.md`、`docs/architecture/websocket-lessons/lesson09_outbound_completion_and_fairness.md`、`docs/architecture/websocket-lessons/lesson10_websocket_application_delivery.md`、`docs/architecture/websocket-lessons/lesson11_websocket_retry_runtime.md`、`docs/architecture/websocket-lessons/lesson12_websocket_receiver_idempotency.md`、`docs/architecture/websocket-lessons/lesson13_websocket_backoff_and_metrics.md`、`docs/architecture/websocket-lessons/lesson14_websocket_executor_boundary.md`、`docs/architecture/websocket-lessons/lesson15_handler_cancellation_and_executor_isolation.md`
- 排错：`docs/debugging/CHEATSHEET.md`
- 习题：`docs/exercises/README.md`

本地（Linux）建议：

```bash
cmake -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
