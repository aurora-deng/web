BEGIN;
ALTER TABLE announcement_audio ADD COLUMN source_text TEXT NOT NULL DEFAULT '';
ALTER TABLE announcement_audio ADD COLUMN expires_ms BIGINT NOT NULL DEFAULT 0;
INSERT INTO phase11_schema_migrations(version,description) VALUES(15,'Function3 durable bounded-retry speech jobs owned by Speech module');
COMMIT;
