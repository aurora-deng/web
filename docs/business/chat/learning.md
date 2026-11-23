# 缘续聊天、群组与消息可靠性 · 学习与复验

1. 在隔离站用两个账号建私聊。发送一条消息，记录幂等键、消息编号、会话序号与 ACK。把 ACK 当成已入账的收据，再比较另一端收到通知与发送 `chat.read` 的时机。
2. 跑 `ctest --test-dir "$WEB_BUILD" -R 'phase11_chat_reliability_tests|function3_workspace_contract' --output-on-failure`。从断线、重试和撤回用例中找一条，画出命令、事务、ACK 和推送的先后顺序。
3. 用 [慢 ACK 练习](../../observability/ack-diagnosis.md) 的隔离方法对比业务排队、PG 取连接和提交时间。切换浏览器的免打扰，再观察未读水位。
4. 用普通成员试图撤回他人消息，再用有权角色复测。退出群后用旧搜索上下文请求核对访问拒绝。

[从 HTTP 到事务的讲义](../../architecture/business/guides/phase49-teaching.md) · [双实例恢复](../../observability/dual-instance-lab.md)

[业务说明](README.md) · [业务目录](../README.md)
