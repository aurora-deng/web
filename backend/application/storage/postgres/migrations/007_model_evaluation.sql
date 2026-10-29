BEGIN;

CREATE TABLE model_evaluation_suites (
    id          BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name        VARCHAR(120) NOT NULL UNIQUE,
    description VARCHAR(1000) NOT NULL DEFAULT '',
    created_at  TIMESTAMPTZ NOT NULL
);

CREATE TABLE model_evaluation_cases (
    id              BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    suite_id        BIGINT NOT NULL REFERENCES model_evaluation_suites(id),
    name            VARCHAR(120) NOT NULL,
    prompt          TEXT NOT NULL,
    expected_answer TEXT NOT NULL DEFAULT '',
    weight          DOUBLE PRECISION NOT NULL DEFAULT 1.0,
    required        BOOLEAN NOT NULL DEFAULT TRUE,
    CONSTRAINT model_evaluation_cases_weight CHECK (weight > 0.0 AND weight <= 1000.0),
    CONSTRAINT model_evaluation_cases_unique_name UNIQUE(suite_id, name)
);

CREATE TABLE model_evaluation_runs (
    id                    BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    suite_id              BIGINT NOT NULL REFERENCES model_evaluation_suites(id),
    model_version_id      BIGINT NOT NULL REFERENCES model_versions(id),
    status                VARCHAR(16) NOT NULL,
    total_cases           BIGINT NOT NULL,
    evaluated_cases       BIGINT NOT NULL DEFAULT 0,
    passed_cases          BIGINT NOT NULL DEFAULT 0,
    weighted_score        DOUBLE PRECISION NOT NULL DEFAULT 0.0,
    required_cases_passed BOOLEAN NOT NULL DEFAULT FALSE,
    detail                VARCHAR(1000) NOT NULL DEFAULT '',
    started_at            TIMESTAMPTZ NOT NULL,
    completed_at          TIMESTAMPTZ,
    CONSTRAINT model_evaluation_runs_status
        CHECK (status IN ('running', 'completed', 'aborted')),
    CONSTRAINT model_evaluation_runs_counts CHECK (
        total_cases > 0 AND evaluated_cases >= 0 AND passed_cases >= 0 AND
        evaluated_cases <= total_cases AND passed_cases <= evaluated_cases),
    CONSTRAINT model_evaluation_runs_score
        CHECK (weighted_score >= 0.0 AND weighted_score <= 1.0),
    CONSTRAINT model_evaluation_runs_terminal CHECK (
        (status = 'running' AND completed_at IS NULL) OR
        (status <> 'running' AND completed_at IS NOT NULL))
);
CREATE INDEX model_evaluation_runs_version_suite_idx
    ON model_evaluation_runs(model_version_id, suite_id, completed_at DESC, id DESC)
    WHERE status = 'completed';

CREATE TABLE model_evaluation_case_results (
    run_id        BIGINT NOT NULL REFERENCES model_evaluation_runs(id) ON DELETE CASCADE,
    case_id       BIGINT NOT NULL REFERENCES model_evaluation_cases(id),
    score         DOUBLE PRECISION NOT NULL,
    passed        BOOLEAN NOT NULL,
    output_sha256 VARCHAR(64) NOT NULL DEFAULT '',
    latency_ms    BIGINT NOT NULL DEFAULT 0,
    detail        VARCHAR(1000) NOT NULL DEFAULT '',
    PRIMARY KEY(run_id, case_id),
    CONSTRAINT model_evaluation_case_results_score CHECK (score >= 0.0 AND score <= 1.0),
    CONSTRAINT model_evaluation_case_results_latency CHECK (latency_ms >= 0),
    CONSTRAINT model_evaluation_case_results_hash CHECK (
        output_sha256 = '' OR length(output_sha256) = 64)
);

CREATE TABLE model_activation_policies (
    node_id                   BIGINT PRIMARY KEY REFERENCES model_nodes(id),
    suite_id                  BIGINT NOT NULL REFERENCES model_evaluation_suites(id),
    minimum_pass_rate         DOUBLE PRECISION NOT NULL,
    minimum_weighted_score    DOUBLE PRECISION NOT NULL,
    max_run_age_seconds       BIGINT NOT NULL,
    updated_at                TIMESTAMPTZ NOT NULL,
    CONSTRAINT model_activation_policies_thresholds CHECK (
        minimum_pass_rate >= 0.0 AND minimum_pass_rate <= 1.0 AND
        minimum_weighted_score >= 0.0 AND minimum_weighted_score <= 1.0),
    CONSTRAINT model_activation_policies_age CHECK (
        max_run_age_seconds >= 60 AND max_run_age_seconds <= 31536000)
);

INSERT INTO phase11_schema_migrations(version, description)
VALUES (7, 'model evaluation suites results and activation quality gate');

COMMIT;
