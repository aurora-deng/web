BEGIN;
CREATE TABLE private_archive_purge_receipts(
 user_id BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 kind TEXT NOT NULL CHECK(kind IN('conversation','knowledge')),
 resource_id BIGINT NOT NULL,
 after_cursor BIGINT NOT NULL,
 through_cursor BIGINT NOT NULL,
 checksum TEXT NOT NULL CHECK(length(checksum)=64),
 created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
 PRIMARY KEY(user_id,kind,resource_id,after_cursor,through_cursor)
);
INSERT INTO phase11_schema_migrations(version,description)VALUES(21,'explicit private archive migration purge receipts');
COMMIT;
