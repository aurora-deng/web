\set ON_ERROR_STOP on
BEGIN;

INSERT INTO users(username, display_name, password_hash, ai_account)
VALUES ('phase11_ai_' || :'test_suffix', 'Phase 11 Runtime AI', 'login-disabled', TRUE)
RETURNING id AS ai_user_id \gset

INSERT INTO model_nodes(name, kind, created_at)
VALUES ('runtime-node-' || :'test_suffix', 'specialist', CURRENT_TIMESTAMP)
RETURNING id AS model_node_id \gset

INSERT INTO model_versions(
    node_id, runtime, model_artifact, adapter_artifact,
    checksum, state, created_at)
VALUES (
    :model_node_id, 'ollama', 'qwen-runtime', '',
    'sha256:runtime-' || :'test_suffix', 'candidate', CURRENT_TIMESTAMP)
RETURNING id AS model_version_id \gset

UPDATE model_nodes
SET active_version_id = :model_version_id
WHERE id = :model_node_id;

UPDATE model_versions
SET state = 'active'
WHERE id = :model_version_id;

INSERT INTO model_bindings(ai_user_id, model_node_id)
VALUES (:ai_user_id, :model_node_id);

COMMIT;
SELECT :ai_user_id AS ai_user_id;
