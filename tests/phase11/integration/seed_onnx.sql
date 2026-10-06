\set ON_ERROR_STOP on

-- 普通模型不传 adapter_artifact 时沿用空值；Adapter 整链验证可通过
-- psql 的 -v adapter_artifact=adapter_weights.onnx_adapter 显式启用。
\if :{?adapter_artifact}
\else
\set adapter_artifact ''
\endif

-- 同一份种子既能验证进程内 Provider，也能验证独立推理进程。调用者只传受支持的
-- 固定值；默认保持原有 onnx-inprocess 行为。
\if :{?model_runtime}
\else
\set model_runtime 'onnx-inprocess'
\endif

BEGIN;

-- 模型目录、摘要和测试后缀由验证命令显式传入。数据库只保存相对目录和已校验的
-- 目录指纹，不保存主机绝对路径。
INSERT INTO users(username, display_name, password_hash, ai_account)
VALUES ('phase11_onnx_ai_' || :'test_suffix',
        'Phase 11 ONNX Runtime AI', 'login-disabled', TRUE)
RETURNING id AS ai_user_id \gset

INSERT INTO model_nodes(name, kind, created_at)
VALUES ('onnx-runtime-node-' || :'test_suffix', 'specialist', CURRENT_TIMESTAMP)
RETURNING id AS model_node_id \gset

INSERT INTO model_versions(
    node_id, runtime, model_artifact, adapter_artifact,
    checksum, state, created_at)
VALUES (
    :model_node_id, :'model_runtime', :'model_artifact', :'adapter_artifact',
    :'model_checksum', 'candidate', CURRENT_TIMESTAMP)
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
