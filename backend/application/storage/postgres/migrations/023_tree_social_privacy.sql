BEGIN;
ALTER TABLE profile_display_settings ADD COLUMN allow_tree_friend_requests BOOLEAN NOT NULL DEFAULT FALSE;
INSERT INTO phase11_schema_migrations(version,description) VALUES(23,'opt-in tree author friend requests and profile audience boundaries');
COMMIT;
