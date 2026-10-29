BEGIN;

-- 同一个用户对同一条 AI 回复只能保留一个未撤销授权。撤销后允许用户基于新的
-- 脱敏文本重新授权；数据库唯一索引负责挡住多 Worker 并发提交的竞态。
CREATE UNIQUE INDEX training_candidates_active_source_unique
    ON training_candidates(owner_user_id, source_message_id)
    WHERE state <> 'revoked';

INSERT INTO phase11_schema_migrations(version, description)
VALUES (11, 'deduplicate active training consent by owner and source message');

COMMIT;
