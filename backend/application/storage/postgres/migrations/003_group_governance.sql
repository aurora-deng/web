BEGIN;

-- 群聊采用软解散：保留成员、消息和审计信息，但不再出现在活动会话列表中，
-- 也不能继续写入消息。这样既能恢复历史事实，也不会把“解散”误做成物理删库。
ALTER TABLE conversations
    ADD COLUMN announcement TEXT NOT NULL DEFAULT '',
    ADD COLUMN state VARCHAR(16) NOT NULL DEFAULT 'active',
    ADD COLUMN dissolved_by BIGINT REFERENCES users(id),
    ADD COLUMN dissolved_at TIMESTAMPTZ,
    ADD CONSTRAINT conversations_announcement_size CHECK (
        octet_length(announcement) <= 4096),
    ADD CONSTRAINT conversations_state CHECK (
        state IN ('active', 'dissolved')),
    ADD CONSTRAINT conversations_dissolve_shape CHECK (
        (state = 'active' AND dissolved_by IS NULL AND dissolved_at IS NULL) OR
        (state = 'dissolved' AND kind = 'group' AND
         dissolved_by IS NOT NULL AND dissolved_at IS NOT NULL));

ALTER TABLE conversation_members
    DROP CONSTRAINT conversation_members_role,
    ADD COLUMN muted_until TIMESTAMPTZ,
    ADD CONSTRAINT conversation_members_role CHECK (
        role IN ('member', 'administrator', 'owner'));

INSERT INTO phase11_schema_migrations(version, description)
VALUES (3, 'phase13 group roles profile moderation mute leave and soft dissolve');

COMMIT;
