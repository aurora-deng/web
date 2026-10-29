BEGIN;
CREATE TABLE function3_delivery_failures (
 id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
 topic TEXT NOT NULL, broker_key VARCHAR(256) NOT NULL,
 event_id BIGINT NOT NULL DEFAULT 0, attempts INTEGER NOT NULL DEFAULT 1,
 state TEXT NOT NULL DEFAULT 'failed' CHECK(state IN ('failed','resolved')),
 last_error TEXT NOT NULL, failed_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
 UNIQUE(topic,broker_key)
);
CREATE INDEX function3_delivery_failure_event_idx ON function3_delivery_failures(event_id) WHERE state='failed';
CREATE INDEX announcement_audio_pending_idx ON announcement_audio(available_at,announcement_id,voice) WHERE state='pending';
CREATE INDEX announcement_audio_expiry_idx ON announcement_audio(expires_ms) WHERE state='pending';
INSERT INTO phase11_schema_migrations(version,description)
 VALUES(17,'Function3 consumer failure visibility, replay references and bounded speech task scans');
COMMIT;
