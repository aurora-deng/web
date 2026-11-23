# Outbox、消息队列、重放与缓存 · 业务说明

业务写入与 Outbox 同事务提交。投递器按顺序取得事件，运行配置决定走本地多实例投递还是已配置的 RocketMQ。消费者经 Inbox 去重并在确认之前提交状态；人工重放保留审计。Redis 读缓存采用可选配置，聊天写入和历史查询的权威状态在 PostgreSQL。

生产验收需要在实际选择的 Broker 或本地投递路径上重现中断、重复、积压和恢复。调试时核对数据库里的事件接收、发布和消费阶段，避免只看发消息成功或缓存命中。

[投递域实现](../../../backend/application/modules/messaging) · [Redis 读缓存](../../../backend/application/modules/cache/infrastructure/RedisReadCache/RedisReadCache.cpp) · [Outbox 表和状态](../../database/phase48-outbox.md) · [MQ 接入](../../architecture/business/messaging/phase29.md)

[实操学习](learning.md) · [业务目录](../README.md)
