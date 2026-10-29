BEGIN;

CREATE TABLE model_training_lineages (
    model_version_id          BIGINT PRIMARY KEY REFERENCES model_versions(id) ON DELETE CASCADE,
    authorization_mode       VARCHAR(24) NOT NULL,
    dataset_version_id       BIGINT REFERENCES dataset_versions(id),
    dataset_sha256           CHAR(64) NOT NULL,
    dataset_records          BIGINT NOT NULL,
    base_repository          VARCHAR(240) NOT NULL,
    base_revision            VARCHAR(160) NOT NULL,
    base_weights_sha256      CHAR(64) NOT NULL,
    training_manifest_sha256 CHAR(64) NOT NULL,
    training_config_sha256   CHAR(64) NOT NULL,
    peft_weights_sha256      CHAR(64) NOT NULL,
    onnx_adapter_sha256      CHAR(64) NOT NULL,
    onnx_model_sha256        CHAR(64) NOT NULL,
    actor                    VARCHAR(128) NOT NULL,
    created_at               TIMESTAMPTZ NOT NULL,
    CONSTRAINT model_training_lineages_mode CHECK (
        (authorization_mode='pipeline-smoke' AND dataset_version_id IS NULL) OR
        (authorization_mode='database-verified' AND dataset_version_id IS NOT NULL)),
    CONSTRAINT model_training_lineages_records CHECK (dataset_records > 0),
    CONSTRAINT model_training_lineages_text CHECK (
        length(base_repository) > 0 AND length(base_revision) > 0 AND
        length(actor) > 0),
    CONSTRAINT model_training_lineages_hashes CHECK (
        dataset_sha256 ~ '^[0-9a-f]{64}$' AND
        base_weights_sha256 ~ '^[0-9a-f]{64}$' AND
        training_manifest_sha256 ~ '^[0-9a-f]{64}$' AND
        training_config_sha256 ~ '^[0-9a-f]{64}$' AND
        peft_weights_sha256 ~ '^[0-9a-f]{64}$' AND
        onnx_adapter_sha256 ~ '^[0-9a-f]{64}$' AND
        onnx_model_sha256 ~ '^[0-9a-f]{64}$')
);

INSERT INTO phase11_schema_migrations(version, description)
VALUES (12, 'immutable dataset training artifact and model version lineage');

COMMIT;
