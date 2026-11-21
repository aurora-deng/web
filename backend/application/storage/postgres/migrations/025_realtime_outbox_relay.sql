-- Relay positions are allocated by a serialized staging statement. Outbox IDs
-- themselves are allocated before the originating transaction can commit.
BEGIN;
CREATE SEQUENCE phase11_realtime_relay_position AS bigint;
ALTER TABLE outbox_events ADD COLUMN relay_position bigint;
CREATE UNIQUE INDEX outbox_events_relay_position_idx
    ON outbox_events(relay_position) WHERE relay_position IS NOT NULL;
INSERT INTO phase11_schema_migrations(version,description) VALUES (25,'realtime outbox relay positions');
COMMIT;
