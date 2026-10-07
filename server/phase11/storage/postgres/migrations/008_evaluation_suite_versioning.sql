BEGIN;

ALTER TABLE model_evaluation_suites
    ADD COLUMN version VARCHAR(64) NOT NULL DEFAULT 'v1',
    ADD COLUMN checksum CHAR(64) NOT NULL DEFAULT repeat('0', 64);

ALTER TABLE model_evaluation_suites
    DROP CONSTRAINT model_evaluation_suites_name_key,
    ADD CONSTRAINT model_evaluation_suites_name_version_key UNIQUE(name, version),
    ADD CONSTRAINT model_evaluation_suites_checksum CHECK (length(checksum) = 64);

INSERT INTO phase11_schema_migrations(version, description)
VALUES (8, 'versioned immutable model evaluation suites');

COMMIT;
