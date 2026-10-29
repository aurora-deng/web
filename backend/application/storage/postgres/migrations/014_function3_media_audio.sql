BEGIN;
ALTER TABLE tree_media_validation ADD COLUMN storage_path TEXT NOT NULL DEFAULT '';
ALTER TABLE tree_media_validation ADD COLUMN media_type TEXT NOT NULL DEFAULT '';
CREATE TABLE announcement_playback_claims (
 user_id BIGINT REFERENCES users(id), announcement_id BIGINT REFERENCES announcements(id),
 claimed_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(user_id,announcement_id)
);
ALTER TABLE announcement_audio ADD COLUMN lease_until TIMESTAMPTZ;
ALTER TABLE announcement_audio ADD COLUMN available_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP;
INSERT INTO phase11_schema_migrations(version,description) VALUES(14,'Function3 validated media paths and cross-tab announcement playback claims');
COMMIT;
