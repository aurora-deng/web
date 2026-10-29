BEGIN;
CREATE TABLE conversation_preferences (
 conversation_id BIGINT NOT NULL,
 user_id BIGINT NOT NULL,
 notifications_muted BOOLEAN NOT NULL DEFAULT FALSE,
 revision BIGINT NOT NULL DEFAULT 1 CHECK(revision>0),
 updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
 PRIMARY KEY(conversation_id,user_id),
 FOREIGN KEY(conversation_id,user_id) REFERENCES conversation_members(conversation_id,user_id) ON DELETE CASCADE
);
CREATE INDEX users_username_prefix_idx ON users(lower(username) text_pattern_ops) WHERE NOT disabled;
CREATE INDEX users_display_name_prefix_idx ON users(lower(display_name) text_pattern_ops) WHERE NOT disabled;
CREATE INDEX messages_active_id_page_idx ON messages(id DESC,conversation_id) WHERE state='active';
INSERT INTO phase11_schema_migrations(version,description) VALUES(24,'chat workspace search and per-member conversation preferences');
COMMIT;
