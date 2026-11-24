BEGIN;

-- The v1 schema remains usable for old release archives. Stop v1 workers before upgrading.
CREATE INDEX workflow_cluster_live_runs ON workflow_cluster_node(run_id, state)
    WHERE state IN ('queued','started');

CREATE FUNCTION workflow_cluster_ready_runs(p_workflows text[], p_limit integer)
RETURNS TABLE(run_id text, workflow text)
LANGUAGE sql STABLE AS $$
    SELECT r.run_id, r.workflow
    FROM workflow_cluster_run r
    WHERE r.workflow = ANY(p_workflows) AND EXISTS (
        SELECT 1 FROM workflow_cluster_node n
        WHERE n.run_id = r.run_id AND n.state IN ('queued','started') AND (
            (n.state = 'started' AND n.lease_until <= statement_timestamp())
            OR (n.state = 'queued' AND (
                r.deadline <= statement_timestamp()
                OR EXISTS (
                    SELECT 1 FROM workflow_cluster_edge e JOIN workflow_cluster_node d
                    ON d.run_id=e.run_id AND d.node_id=e.predecessor
                    WHERE e.run_id=n.run_id AND e.successor=n.node_id
                    AND d.state IN ('failed','timeout','uncertain','skipped')
                )
                OR (n.ready_at <= statement_timestamp() AND NOT EXISTS (
                    SELECT 1 FROM workflow_cluster_edge e JOIN workflow_cluster_node d
                    ON d.run_id=e.run_id AND d.node_id=e.predecessor
                    WHERE e.run_id=n.run_id AND e.successor=n.node_id AND d.state <> 'success'
                ))
            ))
        )
    )
    ORDER BY r.created_at, r.run_id
    LIMIT least(greatest(p_limit, 1), 64)
$$;

CREATE FUNCTION workflow_cluster_claim_v2(p_run text, p_owner text, p_lease_ms integer)
RETURNS TABLE(node_id text, attempt integer, trace_id text, remaining_ms integer)
LANGUAGE plpgsql AS $$
BEGIN
    -- Claims still compete on individual node rows. Reconciliation has the exclusive run lock.
    PERFORM 1 FROM workflow_cluster_run r WHERE r.run_id=p_run FOR SHARE;
    RETURN QUERY SELECT * FROM workflow_cluster_claim(p_run,p_owner,p_lease_ms);
END $$;

CREATE FUNCTION workflow_cluster_renew(p_run text, p_node text, p_owner text, p_attempt integer, p_lease_ms integer)
RETURNS boolean
LANGUAGE plpgsql AS $$
BEGIN
    IF p_lease_ms NOT BETWEEN 50 AND 30000 THEN RAISE EXCEPTION 'invalid lease duration'; END IF;
    UPDATE workflow_cluster_node AS n
    SET lease_until=least(n.attempt_deadline,
        greatest(n.lease_until,clock_timestamp()+p_lease_ms*interval '1 ms'))
    WHERE n.run_id=p_run AND n.node_id=p_node AND n.owner=p_owner AND n.attempt=p_attempt
          AND n.state='started' AND n.lease_until > clock_timestamp();
    RETURN FOUND;
END $$;

CREATE TABLE workflow_cluster_verification (
    run_id text NOT NULL,
    node_id text NOT NULL,
    attempt integer NOT NULL,
    verdict text NOT NULL CHECK (verdict IN ('success','failed')),
    evidence_ref text NOT NULL CHECK (evidence_ref ~ '^[A-Za-z0-9_.-]{1,80}$'),
    value text NOT NULL CHECK (octet_length(value)<=4096),
    actor name NOT NULL DEFAULT session_user,
    checked_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY(run_id,node_id,attempt),
    FOREIGN KEY(run_id,node_id,attempt) REFERENCES workflow_cluster_attempt ON DELETE CASCADE
);

CREATE FUNCTION workflow_cluster_reconcile(p_run text, p_node text, p_attempt integer,
                                           p_verdict text, p_evidence text, p_value text)
RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE prior workflow_cluster_verification%ROWTYPE;
        run_deadline timestamptz;
BEGIN
    IF p_verdict NOT IN ('success','failed') OR p_evidence !~ '^[A-Za-z0-9_.-]{1,80}$'
       OR (p_verdict='failed' AND p_value <> '') THEN RAISE EXCEPTION 'invalid verification'; END IF;
    SELECT deadline INTO run_deadline FROM workflow_cluster_run WHERE run_id=p_run FOR UPDATE;
    IF NOT FOUND THEN RETURN false; END IF;
    SELECT * INTO prior FROM workflow_cluster_verification WHERE run_id=p_run AND node_id=p_node AND attempt=p_attempt;
    IF FOUND THEN
        RETURN prior.verdict=p_verdict AND prior.evidence_ref=p_evidence AND prior.value=p_value;
    END IF;
    UPDATE workflow_cluster_node n SET state=p_verdict, value=p_value, error_code='verified_outcome'
    WHERE n.run_id=p_run AND n.node_id=p_node AND n.effect='external'
          AND n.state='uncertain' AND n.attempt=p_attempt;
    IF NOT FOUND THEN RETURN false; END IF;
    INSERT INTO workflow_cluster_verification(run_id,node_id,attempt,verdict,evidence_ref,value)
    VALUES(p_run,p_node,p_attempt,p_verdict,p_evidence,p_value);
    -- A checked success can release dependency skips while the original run still has time.
    -- Independently failed nodes and in-flight attempts keep their existing decisions.
    IF p_verdict='success' AND run_deadline > clock_timestamp() THEN
        WITH RECURSIVE downstream(id) AS (
            SELECT successor FROM workflow_cluster_edge WHERE run_id=p_run AND predecessor=p_node
            UNION
            SELECT e.successor FROM workflow_cluster_edge e JOIN downstream d ON e.predecessor=d.id
            WHERE e.run_id=p_run
        )
        UPDATE workflow_cluster_node n SET state='queued', ready_at=clock_timestamp(), error_code=''
        FROM downstream d
        WHERE n.run_id=p_run AND n.node_id=d.id AND n.state='skipped'
              AND n.error_code='dependency_failed' AND n.attempt=0;
    END IF;
    RETURN true;
END $$;
REVOKE EXECUTE ON FUNCTION workflow_cluster_reconcile(text,text,integer,text,text,text) FROM PUBLIC;

-- The attempts and the Observer outbox commit together. Readers acknowledge explicit IDs.
CREATE TABLE workflow_cluster_event (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    run_id text NOT NULL REFERENCES workflow_cluster_run ON DELETE CASCADE,
    occurred_at timestamptz NOT NULL,
    kind text NOT NULL CHECK (kind IN ('dag.run','dag.edge','dag.node')),
    fields jsonb NOT NULL,
    exported_at timestamptz
);
CREATE INDEX workflow_cluster_event_pending ON workflow_cluster_event(id) WHERE exported_at IS NULL;
CREATE INDEX workflow_cluster_event_retention ON workflow_cluster_event(occurred_at,id);
ALTER TABLE workflow_cluster_run ADD COLUMN observer_state text NOT NULL DEFAULT 'started';

CREATE FUNCTION workflow_cluster_emit()
RETURNS trigger
LANGUAGE plpgsql AS $$
DECLARE meta jsonb;
BEGIN
    IF TG_TABLE_NAME='workflow_cluster_run' THEN
        INSERT INTO workflow_cluster_event(run_id,occurred_at,kind,fields)
        VALUES(NEW.run_id,CASE WHEN TG_OP='INSERT' THEN NEW.created_at ELSE clock_timestamp() END,
               'dag.run',jsonb_build_object('workflow',NEW.workflow,'state',NEW.observer_state));
    ELSIF TG_TABLE_NAME='workflow_cluster_edge' THEN
        INSERT INTO workflow_cluster_event(run_id,occurred_at,kind,fields)
        VALUES(NEW.run_id,clock_timestamp(),'dag.edge',
               jsonb_build_object('predecessor',NEW.predecessor,'successor',NEW.successor));
    ELSIF TG_TABLE_NAME='workflow_cluster_attempt' THEN
        meta := jsonb_build_object('nodeId',NEW.node_id,'attempt',NEW.attempt,'workerId',NEW.owner,'state',NEW.state);
        IF NEW.ended_at IS NOT NULL THEN
            meta := meta || jsonb_build_object('durationMs',
                greatest(0,extract(epoch FROM NEW.ended_at-NEW.started_at)*1000));
        END IF;
        IF NEW.error_code <> '' THEN meta := meta || jsonb_build_object('errorCode',NEW.error_code); END IF;
        INSERT INTO workflow_cluster_event(run_id,occurred_at,kind,fields)
        VALUES(NEW.run_id,coalesce(NEW.ended_at,NEW.started_at),'dag.node',meta);
    ELSIF TG_TABLE_NAME='workflow_cluster_node' THEN
        INSERT INTO workflow_cluster_event(run_id,occurred_at,kind,fields)
        VALUES(NEW.run_id,clock_timestamp(),'dag.node',jsonb_build_object(
            'nodeId',NEW.node_id,'attempt',NEW.attempt+1,'state',NEW.state,'errorCode',NEW.error_code));
    ELSIF TG_TABLE_NAME='workflow_cluster_verification' THEN
        INSERT INTO workflow_cluster_event(run_id,occurred_at,kind,fields)
        SELECT NEW.run_id,NEW.checked_at,'dag.node',jsonb_build_object(
            'nodeId',NEW.node_id,'attempt',NEW.attempt,'state',NEW.verdict,'errorCode','verified_outcome',
            'durationMs',greatest(0,extract(epoch FROM a.ended_at-a.started_at)*1000))
        FROM workflow_cluster_attempt a
        WHERE a.run_id=NEW.run_id AND a.node_id=NEW.node_id AND a.attempt=NEW.attempt;
    END IF;
    RETURN NULL;
END $$;

CREATE TRIGGER workflow_cluster_trace_run AFTER INSERT ON workflow_cluster_run
    FOR EACH ROW EXECUTE FUNCTION workflow_cluster_emit();
CREATE TRIGGER workflow_cluster_trace_run_state AFTER UPDATE OF observer_state ON workflow_cluster_run
    FOR EACH ROW WHEN (OLD.observer_state IS DISTINCT FROM NEW.observer_state) EXECUTE FUNCTION workflow_cluster_emit();
CREATE TRIGGER workflow_cluster_trace_edge AFTER INSERT ON workflow_cluster_edge
    FOR EACH ROW EXECUTE FUNCTION workflow_cluster_emit();
CREATE TRIGGER workflow_cluster_trace_attempt AFTER INSERT OR UPDATE OF state ON workflow_cluster_attempt
    FOR EACH ROW EXECUTE FUNCTION workflow_cluster_emit();
CREATE TRIGGER workflow_cluster_trace_skip AFTER UPDATE OF state ON workflow_cluster_node
    FOR EACH ROW WHEN (OLD.state='queued' AND NEW.state IN ('skipped','timeout'))
    EXECUTE FUNCTION workflow_cluster_emit();
CREATE TRIGGER workflow_cluster_trace_verification AFTER INSERT ON workflow_cluster_verification
    FOR EACH ROW EXECUTE FUNCTION workflow_cluster_emit();

CREATE FUNCTION workflow_cluster_run_state(p_run text)
RETURNS text
LANGUAGE sql STABLE AS $$
    SELECT CASE WHEN bool_or(state IN ('queued','started')) THEN 'started'
                WHEN bool_or(state='uncertain') THEN 'uncertain'
                WHEN bool_or(state='timeout') THEN 'timeout'
                WHEN bool_or(state IN ('failed','skipped')) THEN 'failed'
                ELSE 'success' END
    FROM workflow_cluster_node WHERE run_id=p_run HAVING count(*) > 0
$$;

CREATE FUNCTION workflow_cluster_export_batch()
RETURNS TABLE(id bigint, envelope jsonb)
LANGUAGE plpgsql AS $$
BEGIN
    -- Update derived run outcomes from committed rows. Every terminal node writes an outbox event,
    -- so a later pass observes a join completed by concurrent workers.
    IF pg_try_advisory_xact_lock(2031406231) THEN
        WITH waiting AS (
            SELECT DISTINCT pending.run_id FROM (
                SELECT e.run_id FROM workflow_cluster_event e
                WHERE e.exported_at IS NULL AND e.occurred_at >= clock_timestamp()-interval '7 days'
                ORDER BY e.id LIMIT 64
            ) pending
        ), status AS (
            SELECT w.run_id,workflow_cluster_run_state(w.run_id) AS state FROM waiting w
        )
        UPDATE workflow_cluster_run r SET observer_state=s.state FROM status s
        WHERE r.run_id=s.run_id AND r.observer_state IS DISTINCT FROM s.state;
    END IF;
    RETURN QUERY SELECT e.id,jsonb_build_object(
        'schemaVersion',1,'eventId',r.trace_id||'.pg.'||e.id::text,'eventType',e.kind,
        'occurredAt',((extract(epoch FROM e.occurred_at)*1000000000)::bigint)::text,
        'traceId',r.trace_id,'fields',e.fields||jsonb_build_object('runId',e.run_id)
    ) || CASE WHEN e.kind='dag.node' THEN jsonb_build_object('spanId',
        substr(md5(e.run_id||':'||(e.fields->>'nodeId')||':'||(e.fields->>'attempt')),1,16)) ELSE '{}'::jsonb END
    FROM workflow_cluster_event e JOIN workflow_cluster_run r ON r.run_id=e.run_id
    WHERE e.exported_at IS NULL AND e.occurred_at >= clock_timestamp()-interval '7 days'
    ORDER BY e.id LIMIT 64;
END $$;

COMMIT;
