BEGIN;

CREATE TABLE model_activation_audits (
    id                  BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    node_id             BIGINT NOT NULL REFERENCES model_nodes(id),
    previous_version_id BIGINT REFERENCES model_versions(id),
    target_version_id   BIGINT NOT NULL REFERENCES model_versions(id),
    action              VARCHAR(16) NOT NULL,
    outcome             VARCHAR(16) NOT NULL,
    actor               VARCHAR(128) NOT NULL,
    detail              VARCHAR(512) NOT NULL DEFAULT '',
    created_at          TIMESTAMPTZ NOT NULL,
    CONSTRAINT model_activation_audits_action CHECK (action IN ('activate', 'rollback')),
    CONSTRAINT model_activation_audits_outcome CHECK (outcome IN ('succeeded', 'failed'))
);
CREATE INDEX model_activation_audits_node_time_idx
    ON model_activation_audits(node_id, created_at DESC, id DESC);

CREATE TABLE ai_generation_audits (
    generation_id       VARCHAR(128) PRIMARY KEY,
    requester_id        BIGINT NOT NULL REFERENCES users(id),
    ai_user_id           BIGINT NOT NULL REFERENCES users(id),
    conversation_id     BIGINT NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    model_node_id       BIGINT REFERENCES model_nodes(id),
    model_version_id    BIGINT REFERENCES model_versions(id),
    runtime             VARCHAR(32) NOT NULL DEFAULT '',
    model_artifact      TEXT NOT NULL DEFAULT '',
    adapter_artifact    TEXT NOT NULL DEFAULT '',
    model_checksum      TEXT NOT NULL DEFAULT '',
    prompt_sha256       CHAR(64) NOT NULL,
    prompt_bytes        BIGINT NOT NULL,
    output_sha256       CHAR(64) NOT NULL DEFAULT '',
    output_bytes        BIGINT NOT NULL DEFAULT 0,
    output_tokens       BIGINT NOT NULL DEFAULT 0,
    status              VARCHAR(16) NOT NULL,
    error_detail        VARCHAR(512) NOT NULL DEFAULT '',
    requested_at        TIMESTAMPTZ NOT NULL,
    started_at          TIMESTAMPTZ,
    first_token_at      TIMESTAMPTZ,
    completed_at        TIMESTAMPTZ,
    response_message_id BIGINT REFERENCES messages(id),
    revision            BIGINT NOT NULL,
    CONSTRAINT ai_generation_audits_status CHECK (
        status IN ('queued', 'running', 'succeeded', 'failed', 'cancelled', 'timed_out')),
    CONSTRAINT ai_generation_audits_sizes CHECK (
        prompt_bytes >= 0 AND output_bytes >= 0 AND output_tokens >= 0 AND revision >= 1),
    CONSTRAINT ai_generation_audits_prompt_hash CHECK (length(prompt_sha256) = 64),
    CONSTRAINT ai_generation_audits_output_hash CHECK (
        output_sha256 = '' OR length(output_sha256) = 64)
);
CREATE INDEX ai_generation_audits_conversation_time_idx
    ON ai_generation_audits(conversation_id, requested_at DESC);
CREATE INDEX ai_generation_audits_model_time_idx
    ON ai_generation_audits(model_version_id, requested_at DESC)
    WHERE model_version_id IS NOT NULL;

INSERT INTO phase11_schema_migrations(version, description)
VALUES (6, 'model activation audit and AI generation trace');

COMMIT;
