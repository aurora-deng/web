BEGIN;

-- 训练正文留在受控文件目录；数据库保存可审计的相对路径、记录数和 SHA-256。
ALTER TABLE dataset_versions
    ADD COLUMN artifact_path TEXT NOT NULL DEFAULT 'legacy-untracked',
    ADD COLUMN record_count BIGINT NOT NULL DEFAULT 0;

ALTER TABLE dataset_versions
    ADD CONSTRAINT dataset_versions_artifact_path CHECK (
        length(artifact_path) > 0 AND
        artifact_path !~ '(^|/)\.\.(/|$)' AND
        artifact_path !~ '^/'),
    ADD CONSTRAINT dataset_versions_record_count CHECK (record_count >= 0),
    ADD CONSTRAINT dataset_versions_sha256 CHECK (
        checksum ~ '^[0-9a-fA-F]{64}$' OR checksum LIKE 'sha256:%');

INSERT INTO phase11_schema_migrations(version, description)
VALUES (10, 'training dataset artifact path record count and checksum constraints');

COMMIT;
