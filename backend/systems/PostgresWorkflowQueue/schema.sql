BEGIN;

CREATE TABLE workflow_cluster_run (
    run_id text PRIMARY KEY CHECK (run_id ~ '^[A-Za-z0-9_.-]{1,64}$'),
    trace_id text NOT NULL UNIQUE CHECK (trace_id ~ '^[a-f0-9]{32}$' AND trace_id <> repeat('0',32)),
    workflow text NOT NULL CHECK (workflow ~ '^[A-Za-z0-9_.-]{1,80}$'),
    definition text NOT NULL CHECK (octet_length(definition) <= 131072),
    created_at timestamptz NOT NULL DEFAULT statement_timestamp(),
    deadline timestamptz NOT NULL
);

CREATE TABLE workflow_cluster_node (
    run_id text NOT NULL REFERENCES workflow_cluster_run ON DELETE CASCADE,
    node_id text NOT NULL CHECK (node_id ~ '^[A-Za-z0-9_.-]{1,80}$'),
    effect text NOT NULL CHECK (effect IN ('read','idempotent','external')),
    max_attempts integer NOT NULL CHECK (max_attempts BETWEEN 1 AND 3 AND (effect <> 'external' OR max_attempts = 1)),
    timeout_ms integer NOT NULL CHECK (timeout_ms BETWEEN 1 AND 600000),
    backoff_ms integer NOT NULL CHECK (backoff_ms BETWEEN 0 AND 5000),
    state text NOT NULL DEFAULT 'queued' CHECK (state IN ('queued','started','success','failed','timeout','uncertain','skipped')),
    attempt integer NOT NULL DEFAULT 0 CHECK (attempt BETWEEN 0 AND max_attempts),
    ready_at timestamptz NOT NULL DEFAULT statement_timestamp(),
    owner text,
    lease_until timestamptz,
    attempt_deadline timestamptz,
    value text NOT NULL DEFAULT '' CHECK (octet_length(value) <= 4096),
    error_code text NOT NULL DEFAULT '' CHECK (error_code ~ '^[A-Za-z0-9_.-]{0,80}$'),
    PRIMARY KEY(run_id,node_id),
    CHECK ((state = 'started') = (owner IS NOT NULL AND lease_until IS NOT NULL AND attempt_deadline IS NOT NULL))
);
CREATE INDEX workflow_cluster_ready ON workflow_cluster_node(run_id,ready_at,node_id) WHERE state = 'queued';
CREATE INDEX workflow_cluster_expiry ON workflow_cluster_node(run_id,lease_until) WHERE state = 'started';

CREATE TABLE workflow_cluster_edge (
    run_id text NOT NULL, predecessor text NOT NULL, successor text NOT NULL,
    PRIMARY KEY(run_id,successor,predecessor),
    FOREIGN KEY(run_id,predecessor) REFERENCES workflow_cluster_node ON DELETE CASCADE,
    FOREIGN KEY(run_id,successor) REFERENCES workflow_cluster_node ON DELETE CASCADE
);

CREATE TABLE workflow_cluster_attempt (
    run_id text NOT NULL, node_id text NOT NULL, attempt integer NOT NULL CHECK (attempt BETWEEN 1 AND 3),
    owner text NOT NULL CHECK (owner ~ '^[A-Za-z0-9_.-]{1,64}$'),
    started_at timestamptz NOT NULL, ended_at timestamptz,
    state text NOT NULL CHECK (state IN ('started','success','failed','timeout','uncertain','retry')),
    error_code text NOT NULL DEFAULT '' CHECK (error_code ~ '^[A-Za-z0-9_.-]{0,80}$'),
    PRIMARY KEY(run_id,node_id,attempt),
    FOREIGN KEY(run_id,node_id) REFERENCES workflow_cluster_node ON DELETE CASCADE
);

CREATE FUNCTION workflow_cluster_claim(p_run text, p_owner text, p_lease_ms integer)
RETURNS TABLE(node_id text, attempt integer, trace_id text, remaining_ms integer)
LANGUAGE plpgsql AS $$
DECLARE
    item workflow_cluster_node%ROWTYPE;
    run_limit timestamptz;
    now_db timestamptz;
    next_deadline timestamptz;
    next_lease timestamptz;
    next_attempt integer;
    stop_state text;
    stop_reason text;
    blocked boolean;
BEGIN
    IF p_owner !~ '^[A-Za-z0-9_.-]{1,64}$' OR p_lease_ms NOT BETWEEN 50 AND 30000 THEN
        RAISE EXCEPTION 'invalid lease parameters';
    END IF;
    SELECT r.deadline INTO run_limit FROM workflow_cluster_run r WHERE r.run_id = p_run;
    IF NOT FOUND THEN RAISE EXCEPTION 'unknown run'; END IF;
    -- At most one graph's worth of terminal transitions before returning work.
    FOR scan IN 1..128 LOOP
        SELECT n.* INTO item
        FROM workflow_cluster_node n
        WHERE n.run_id = p_run AND (
            (n.state = 'started' AND n.lease_until <= statement_timestamp())
            OR (n.state = 'queued' AND (
                run_limit <= statement_timestamp()
                OR EXISTS (
                    SELECT 1 FROM workflow_cluster_edge e JOIN workflow_cluster_node d
                      ON d.run_id = e.run_id AND d.node_id = e.predecessor
                    WHERE e.run_id = n.run_id AND e.successor = n.node_id
                      AND d.state IN ('failed','timeout','uncertain','skipped')
                )
                OR (n.ready_at <= statement_timestamp() AND NOT EXISTS (
                    SELECT 1 FROM workflow_cluster_edge e JOIN workflow_cluster_node d
                      ON d.run_id = e.run_id AND d.node_id = e.predecessor
                    WHERE e.run_id = n.run_id AND e.successor = n.node_id AND d.state <> 'success'
                ))
            ))
        )
        ORDER BY CASE WHEN n.state = 'started' THEN 0 ELSE 1 END, n.ready_at, n.node_id
        LIMIT 1 FOR UPDATE OF n SKIP LOCKED;
        EXIT WHEN NOT FOUND;
        now_db := clock_timestamp();
        stop_state := NULL;
        stop_reason := '';
        IF item.state = 'started' THEN
            stop_reason := 'lease_expired';
            UPDATE workflow_cluster_attempt a SET ended_at = now_db, error_code = stop_reason,
                state = CASE WHEN item.effect = 'external' THEN 'uncertain' ELSE 'timeout' END
            WHERE a.run_id = p_run AND a.node_id = item.node_id AND a.attempt = item.attempt;
            IF item.effect = 'external' THEN stop_state := 'uncertain';
            ELSIF item.attempt >= item.max_attempts THEN stop_state := 'timeout';
            END IF;
        END IF;
        SELECT EXISTS (
            SELECT 1 FROM workflow_cluster_edge e JOIN workflow_cluster_node d
                ON d.run_id = e.run_id AND d.node_id = e.predecessor
            WHERE e.run_id = p_run AND e.successor = item.node_id AND d.state IN ('failed','timeout','uncertain','skipped')
        ) INTO blocked;
        IF stop_state IS NULL AND run_limit <= now_db THEN stop_state := 'timeout'; stop_reason := 'run_timeout'; END IF;
        IF stop_state IS NULL AND blocked THEN stop_state := 'skipped'; stop_reason := 'dependency_failed'; END IF;
        IF stop_state IS NOT NULL THEN
            UPDATE workflow_cluster_node n
            SET state = stop_state, error_code = stop_reason, owner = NULL, lease_until = NULL, attempt_deadline = NULL
            WHERE n.run_id = p_run AND n.node_id = item.node_id;
            CONTINUE;
        END IF;

        next_deadline := least(run_limit, now_db + item.timeout_ms * interval '1 ms');
        next_lease := least(next_deadline, now_db + p_lease_ms * interval '1 ms');
        next_attempt := item.attempt + 1;
        UPDATE workflow_cluster_node n SET state = 'started', attempt = next_attempt, owner = p_owner,
            lease_until = next_lease, attempt_deadline = next_deadline, error_code = ''
        WHERE n.run_id = p_run AND n.node_id = item.node_id;
        INSERT INTO workflow_cluster_attempt VALUES (p_run,item.node_id,next_attempt,p_owner,now_db,NULL,'started','');
        RETURN QUERY SELECT item.node_id, next_attempt, r.trace_id,
              greatest(1, floor(extract(epoch FROM (next_deadline - now_db)) * 1000)::integer)
            FROM workflow_cluster_run r WHERE r.run_id = p_run;
        RETURN;
    END LOOP;
END $$;

CREATE FUNCTION workflow_cluster_finish(p_run text, p_node text, p_owner text, p_attempt integer,
                                         p_status text, p_value text, p_error text)
RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE written workflow_cluster_node%ROWTYPE;
BEGIN
    IF p_status NOT IN ('success','failed','retry','uncertain') THEN RAISE EXCEPTION 'invalid completion'; END IF;
    UPDATE workflow_cluster_node n SET
        state = CASE WHEN p_status = 'retry' THEN
            CASE WHEN n.effect = 'external' THEN 'uncertain'
                 WHEN n.attempt < n.max_attempts THEN 'queued' ELSE 'failed' END
            ELSE p_status END,
        value = CASE WHEN p_status = 'success' THEN p_value ELSE '' END,
        error_code = p_error,
        ready_at = clock_timestamp() + n.backoff_ms * n.attempt * interval '1 ms',
        owner = NULL, lease_until = NULL, attempt_deadline = NULL
    WHERE n.run_id = p_run AND n.node_id = p_node AND n.owner = p_owner AND n.attempt = p_attempt
      AND n.state = 'started' AND n.lease_until > clock_timestamp()
    RETURNING n.* INTO written;
    IF NOT FOUND THEN RETURN false; END IF;
    UPDATE workflow_cluster_attempt a SET ended_at = clock_timestamp(), error_code = p_error,
        state = CASE WHEN written.state = 'queued' THEN 'retry' ELSE written.state END
    WHERE a.run_id = p_run AND a.node_id = p_node AND a.attempt = p_attempt;
    RETURN true;
END $$;

COMMIT;
