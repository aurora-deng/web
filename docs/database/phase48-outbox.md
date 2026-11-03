# Phase48 数据库兼容说明

本轮没有新增、修改表或迁移。迁移版本仍为024，原业务库没有被操作。

`outbox_events.published_at`含义仍是事件已被投递层接受并确认，不是客户端已读。新驱动只将成功前缀的多次UPDATE合并成一次参数化CTE；已有数据和旧程序仍兼容。

- 一个批次最多1000个互异ID；空批次成功，不访问数据库。
- 任一ID不存在或重复时，原生PG/内存实现均不产生部分确认。
- 外层已有事务时，批量确认参与该事务，可以整体回滚。
- 发布失败后的事件不进入确认集合，保持原有顺序；确认前进程故障可能重投。
- 没有改synchronous_commit、没有添加数据库触发器、没有创建新的账号库。

读写实现见 [PostgresOutboxRepository](../../backend/application/storage/postgres/PostgresOutboxRepository/PostgresOutboxRepository.cpp)，业务流程见 [Phase48](../architecture/business/engineering/phase48.md)。
