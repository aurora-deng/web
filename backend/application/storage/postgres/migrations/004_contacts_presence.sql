BEGIN;

-- 好友关系是双向的，联系人设置是每个用户自己的单向视图。
CREATE TABLE contact_preferences (
    owner_id               BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    contact_id             BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    remark                 VARCHAR(64) NOT NULL DEFAULT '',
    notifications_muted    BOOLEAN NOT NULL DEFAULT FALSE,
    PRIMARY KEY (owner_id, contact_id),
    CONSTRAINT contact_preferences_distinct_users CHECK (owner_id <> contact_id)
);

-- 黑名单是单向关系；A 屏蔽 B 不等于 B 主动屏蔽 A。
CREATE TABLE user_blocks (
    blocker_id     BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    blocked_id     BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at     TIMESTAMPTZ NOT NULL,
    PRIMARY KEY (blocker_id, blocked_id),
    CONSTRAINT user_blocks_distinct_users CHECK (blocker_id <> blocked_id)
);
CREATE INDEX user_blocks_blocked_idx ON user_blocks(blocked_id, blocker_id);

-- online 是进程内实时事实；这里只保存最后一次连接状态变化时间，重启后不会误报在线。
CREATE TABLE user_presence (
    user_id         BIGINT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    last_active_at  TIMESTAMPTZ NOT NULL
);

INSERT INTO phase11_schema_migrations(version, description)
VALUES (4, 'phase14 contact settings blocks multi-device presence and last active');

COMMIT;
