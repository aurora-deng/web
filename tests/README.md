# 测试分层（学习目标）

| 层 | 文件 | 学什么 | 本层不测什么 |
|---|---|---|---|
| unit | `unit_tests.cpp` | Buffer / Parser / Router / WS 消息组装等纯逻辑 | 不起 epoll、不起线程 |
| component | `transport_tests.cpp` | OutboundTask / TransportWriter（socketpair） | 不起完整 SubReactor |
| lifecycle | `coroutine_lifecycle_tests.cpp` | 协程槽、关闭路径 | 不做全链路业务 |
| integration | `integration/*.py` | HTTP / WS 黑盒端到端 | 不测内部私有符号 |

事实源与排错：

- 架构：`docs/architecture/phase5_connection_identity_and_root_coroutine.md`、`docs/architecture/phase6_websocket_frame_and_message_state.md`、`docs/architecture/phase7_websocket_utf8_and_close.md`、`docs/architecture/phase8_websocket_backpressure.md`、`docs/architecture/phase9_outbound_completion_and_fairness.md`、`docs/architecture/phase10_websocket_application_delivery.md`
- 排错：`docs/debugging/CHEATSHEET.md`
- 习题：`docs/exercises/README.md`

本地（Linux）建议：

```bash
cmake -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
