BEGIN;

CREATE TABLE IF NOT EXISTS phase11_schema_migrations (
    version         BIGINT PRIMARY KEY,
    description     TEXT NOT NULL,
    applied_at      TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE users (
    id              BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    username        VARCHAR(32) NOT NULL UNIQUE,
    display_name    VARCHAR(64) NOT NULL,
    biography       VARCHAR(512) NOT NULL DEFAULT '',
    password_hash   TEXT NOT NULL,
    disabled        BOOLEAN NOT NULL DEFAULT FALSE,
    ai_account      BOOLEAN NOT NULL DEFAULT FALSE,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT users_username_shape CHECK (username ~ '^[a-z0-9_-]{3,32}$')
);

CREATE TABLE user_sessions (
    id              VARCHAR(128) PRIMARY KEY,
    user_id         BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    token_hash      TEXT NOT NULL UNIQUE,
    csrf_hash       TEXT NOT NULL,
    created_at      TIMESTAMPTZ NOT NULL,
    expires_at      TIMESTAMPTZ NOT NULL,
    revoked_at      TIMESTAMPTZ,
    CONSTRAINT user_sessions_expiry CHECK (expires_at > created_at)
);
CREATE INDEX user_sessions_user_active_idx
    ON user_sessions(user_id, expires_at) WHERE revoked_at IS NULL;

CREATE TABLE friend_requests (
    id              BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    sender_id       BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    receiver_id     BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    state           VARCHAR(16) NOT NULL DEFAULT 'pending',
    created_at      TIMESTAMPTZ NOT NULL,
    decided_at      TIMESTAMPTZ,
    CONSTRAINT friend_requests_distinct_users CHECK (sender_id <> receiver_id),
    CONSTRAINT friend_requests_state CHECK (state IN ('pending', 'accepted', 'rejected'))
);
-- LEAST/GREATEST 把 A→B 和 B→A 归一成同一对用户，避免两个并发方向各插入一条
-- pending 申请。应用层检查用于友好报错，本索引负责并发下的最终一致性防线。
CREATE UNIQUE INDEX friend_requests_one_pending_pair_idx
    ON friend_requests(
        LEAST(sender_id, receiver_id),
        GREATEST(sender_id, receiver_id))
    WHERE state = 'pending';
CREATE INDEX friend_requests_receiver_pending_idx
    ON friend_requests(receiver_id, created_at) WHERE state = 'pending';

CREATE TABLE friendships (
    user_low_id     BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    user_high_id    BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (user_low_id, user_high_id),
    CONSTRAINT friendships_ordered_pair CHECK (user_low_id < user_high_id)
);

CREATE TABLE conversations (
    id              BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    kind            VARCHAR(16) NOT NULL,
    title           VARCHAR(80) NOT NULL DEFAULT '',
    created_by      BIGINT NOT NULL REFERENCES users(id),
    next_sequence   BIGINT NOT NULL DEFAULT 1,
    direct_pair_key VARCHAR(64) UNIQUE,
    created_at      TIMESTAMPTZ NOT NULL,
    CONSTRAINT conversations_kind CHECK (kind IN ('direct', 'group')),
    CONSTRAINT conversations_sequence CHECK (next_sequence >= 1),
    CONSTRAINT conversations_direct_key CHECK (
        (kind = 'direct' AND direct_pair_key IS NOT NULL) OR
        (kind = 'group' AND direct_pair_key IS NULL))
);

CREATE TABLE conversation_members (
    conversation_id         BIGINT NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    user_id                 BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    role                    VARCHAR(16) NOT NULL DEFAULT 'member',
    last_delivered_sequence BIGINT NOT NULL DEFAULT 0,
    last_read_sequence      BIGINT NOT NULL DEFAULT 0,
    joined_at               TIMESTAMPTZ NOT NULL,
    PRIMARY KEY (conversation_id, user_id),
    CONSTRAINT conversation_members_role CHECK (role IN ('member', 'owner')),
    CONSTRAINT conversation_members_cursors CHECK (
        last_delivered_sequence >= 0 AND
        last_read_sequence >= 0 AND
        last_read_sequence <= last_delivered_sequence)
);
CREATE INDEX conversation_members_user_idx
    ON conversation_members(user_id, conversation_id);

CREATE TABLE messages (
    id                  BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    conversation_id     BIGINT NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    conversation_seq    BIGINT NOT NULL,
    sender_id           BIGINT NOT NULL REFERENCES users(id),
    client_message_id   VARCHAR(128) NOT NULL,
    body                TEXT NOT NULL,
    model_node_id       BIGINT,
    model_version_id    BIGINT,
    adapter_name        TEXT NOT NULL DEFAULT '',
    created_at          TIMESTAMPTZ NOT NULL,
    UNIQUE (conversation_id, conversation_seq),
    UNIQUE (conversation_id, sender_id, client_message_id),
    CONSTRAINT messages_sequence_positive CHECK (conversation_seq >= 1),
    CONSTRAINT messages_body_size CHECK (
        octet_length(body) BETWEEN 1 AND 16384)
);
CREATE INDEX messages_conversation_history_idx
    ON messages(conversation_id, conversation_seq);
CREATE INDEX messages_body_search_idx
    ON messages USING GIN (to_tsvector('simple', body));

CREATE TABLE outbox_events (
    id              BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    event_type      VARCHAR(80) NOT NULL,
    aggregate_type  VARCHAR(80) NOT NULL,
    aggregate_id    BIGINT NOT NULL,
    payload         TEXT NOT NULL,
    created_at      TIMESTAMPTZ NOT NULL,
    published_at    TIMESTAMPTZ
);
CREATE INDEX outbox_events_pending_idx
    ON outbox_events(id) WHERE published_at IS NULL;

CREATE TABLE model_nodes (
    id                  BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name                VARCHAR(80) NOT NULL UNIQUE,
    kind                VARCHAR(16) NOT NULL,
    active_version_id   BIGINT,
    created_at          TIMESTAMPTZ NOT NULL,
    CONSTRAINT model_nodes_kind CHECK (kind IN ('root', 'specialist'))
);

CREATE TABLE model_versions (
    id                  BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    node_id             BIGINT NOT NULL REFERENCES model_nodes(id) ON DELETE CASCADE,
    runtime             VARCHAR(32) NOT NULL,
    model_artifact      TEXT NOT NULL,
    adapter_artifact    TEXT NOT NULL DEFAULT '',
    checksum            TEXT NOT NULL,
    state               VARCHAR(16) NOT NULL DEFAULT 'candidate',
    created_at          TIMESTAMPTZ NOT NULL,
    CONSTRAINT model_versions_runtime CHECK (
        runtime IN ('ollama', 'onnx-inprocess', 'onnx-grpc')),
    CONSTRAINT model_versions_state CHECK (
        state IN ('candidate', 'active', 'retired', 'rejected')),
    UNIQUE (node_id, checksum)
);
ALTER TABLE model_nodes
    ADD CONSTRAINT model_nodes_active_version_fk
    FOREIGN KEY (active_version_id) REFERENCES model_versions(id);

ALTER TABLE messages
    ADD CONSTRAINT messages_model_node_fk
    FOREIGN KEY (model_node_id) REFERENCES model_nodes(id),
    ADD CONSTRAINT messages_model_version_fk
    FOREIGN KEY (model_version_id) REFERENCES model_versions(id);

CREATE TABLE model_edges (
    id              BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    from_node_id    BIGINT NOT NULL REFERENCES model_nodes(id) ON DELETE CASCADE,
    to_node_id      BIGINT NOT NULL REFERENCES model_nodes(id) ON DELETE CASCADE,
    kind            VARCHAR(32) NOT NULL,
    CONSTRAINT model_edges_distinct CHECK (from_node_id <> to_node_id),
    CONSTRAINT model_edges_kind CHECK (
        kind IN ('parent_controls_child', 'peer_collaborates')),
    UNIQUE (from_node_id, to_node_id, kind)
);

CREATE TABLE model_bindings (
    ai_user_id      BIGINT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    model_node_id   BIGINT NOT NULL REFERENCES model_nodes(id)
);

CREATE TABLE training_candidates (
    id                  BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    owner_user_id       BIGINT NOT NULL REFERENCES users(id),
    source_message_id   BIGINT NOT NULL REFERENCES messages(id),
    sanitized_prompt    TEXT NOT NULL,
    sanitized_response  TEXT NOT NULL,
    consent_version     VARCHAR(80) NOT NULL,
    state               VARCHAR(16) NOT NULL DEFAULT 'submitted',
    dataset_version_id  BIGINT,
    created_at          TIMESTAMPTZ NOT NULL,
    CONSTRAINT training_candidates_state CHECK (
        state IN ('submitted', 'approved', 'rejected', 'exported', 'revoked'))
);

CREATE TABLE dataset_versions (
    id              BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name            VARCHAR(120) NOT NULL UNIQUE,
    checksum        TEXT NOT NULL UNIQUE,
    created_at      TIMESTAMPTZ NOT NULL
);

ALTER TABLE training_candidates
    ADD CONSTRAINT training_candidates_dataset_fk
    FOREIGN KEY (dataset_version_id) REFERENCES dataset_versions(id);

CREATE TABLE dataset_candidates (
    dataset_version_id  BIGINT NOT NULL REFERENCES dataset_versions(id) ON DELETE CASCADE,
    candidate_id        BIGINT NOT NULL REFERENCES training_candidates(id),
    PRIMARY KEY (dataset_version_id, candidate_id)
);

INSERT INTO phase11_schema_migrations(version, description)
VALUES (1, 'phase11 account social chat model and training foundation');

COMMIT;
