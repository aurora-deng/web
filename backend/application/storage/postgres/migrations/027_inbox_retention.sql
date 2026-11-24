BEGIN;

CREATE INDEX function3_inbox_processed_idx
    ON function3_inbox (processed_at, consumer, event_id);

-- Retain the delivery receipt on its existing Outbox row when the detailed
-- Inbox marker is removed. A late Broker delivery must still be a duplicate.
ALTER TABLE function3_outbox ADD COLUMN inbox_retired_at timestamptz;

CREATE FUNCTION function3_prune_inbox(batch_size integer)
RETURNS integer
LANGUAGE sql
AS $$
    WITH picked AS MATERIALIZED (
        SELECT o.id, i.consumer
        FROM function3_inbox i
        JOIN function3_outbox o ON o.id = i.event_id
        WHERE i.processed_at < statement_timestamp() - interval '14 days'
          AND o.state = 'published' AND o.inbox_retired_at IS NULL
          AND i.consumer = CASE WHEN o.event_type = 'chat.legacy'
                                THEN 'chat.realtime' ELSE 'function3.delivery' END
          AND NOT EXISTS (
              SELECT 1 FROM function3_delivery_failures f
              WHERE f.event_id = o.id AND f.state = 'failed'
          )
        ORDER BY i.processed_at, i.consumer, i.event_id
        LIMIT greatest(0, least(coalesce(batch_size, 0), 2000))
        FOR UPDATE OF o SKIP LOCKED
    ), retired AS (
        UPDATE function3_outbox o SET inbox_retired_at = statement_timestamp()
        FROM picked p WHERE o.id = p.id
        RETURNING o.id, p.consumer
    ), removed AS (
        DELETE FROM function3_inbox i USING retired r
        WHERE i.event_id = r.id AND i.consumer = r.consumer
        RETURNING i.event_id
    )
    SELECT count(*)::integer FROM removed
$$;

INSERT INTO phase11_schema_migrations(version,description) VALUES (27,'bounded Inbox retention and late delivery fencing');
COMMIT;
