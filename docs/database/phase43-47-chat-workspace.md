# 迁移024：会话提醒偏好与读取索引

迁移文件：`backend/application/storage/postgres/migrations/024_chat_workspace.sql`。只追加，不改已应用迁移。不创建新的账号库，原业务库仍是 `phase11`；本轮实际执行仅在已有隔离库。

| 列 | 含义 |
|---|---|
| conversation_id | 设置属于哪一个会话 |
| user_id | 设置属于哪一位当前成员 |
| notifications_muted | 此用户是否关闭该会话提醒 |
| revision | 单调递增版本；相同值重试不增加，用于多端收敛 |
| updated_at | 数据库记录的更新时间 |

联合主键 `(conversation_id,user_id)`；联合外键引用 `conversation_members`，成员移除后偏好级联删除。它不保存未读计数，不影响messages/outbox和成员禁言。未显式设置会话偏好时，私聊仍兼容旧 `contact_preferences` 默认值；显式偏好优先。

新增索引：未禁用用户的小写用户名/显示名前缀索引、活动消息ID与会话游标索引。不是中文分词索引，不声称ILIKE包含搜索都能走索引。应结合EXPLAIN和真实数据量判断。

偏好更新和 `conversation.preferences` Outbox共用原事务连接；同值无新事件，事件接收范围仅操作者。退出、解散、撤回等原有规则不变。

启动检查001–024的记录完整性；缺失即报错，服务器不会自行升级原库。正式升级前仍需要用户批准并备份，停止旧服务后按已有迁移工具逐份执行。回退可以保留新增表与索引，不应删除用户数据；旧版不会识别新会话偏好，因此回退期间该设置可能暂不显示。
