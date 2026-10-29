BEGIN;
-- 使用原业务数据库新增具名表；不创建数据库，不修改已应用的迁移。
CREATE TABLE tree_reply_links (
    reply_id BIGINT PRIMARY KEY REFERENCES tree_replies(id),
    parent_reply_id BIGINT NOT NULL REFERENCES tree_replies(id),
    CHECK (reply_id > parent_reply_id)
);
CREATE INDEX tree_reply_links_parent ON tree_reply_links(parent_reply_id, reply_id);
INSERT INTO phase11_schema_migrations(version,description) VALUES (18,'Tree reply relationships in shared business database');
COMMIT;
