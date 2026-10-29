BEGIN;
-- Agent确认操作的服务端幂等收据，不能被客户端按钮状态代替。
CREATE TABLE profile_operation_receipts(
 user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 operation_id TEXT NOT NULL CHECK(octet_length(operation_id) BETWEEN 1 AND 128),
 display_name TEXT NOT NULL,
 biography TEXT NOT NULL,
 created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
 PRIMARY KEY(user_id,operation_id)
);
INSERT INTO phase11_schema_migrations(version,description)VALUES(20,'payload bound profile operation receipts');
COMMIT;
