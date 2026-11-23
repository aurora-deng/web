# Outbox、消息队列、重放与缓存 · 学习与复验

1. 把 Outbox 看作同一笔交易附带的寄件单。运行 `ctest --test-dir "$WEB_BUILD" -R 'function3_commit_wakeup|function3_postgres_contract' --output-on-failure`，找出事务提交和唤醒投递器的交界。
2. 用双实例隔离实验模拟 ACK 后进程退出。对照同一幂等键的数据库行数、另一端送达和历史查询。重复消费应保持数据正确。
3. 有选配 RocketMQ 或 Redis 时在独立环境做断连与恢复。记录积压峰值、恢复耗时、重复事件计数和读请求退化情况。

[双实例和告警实验](../../observability/dual-instance-lab.md) · [Outbox 教学](../../architecture/business/guides/phase48-teaching.md)

[业务说明](README.md) · [业务目录](../README.md)
