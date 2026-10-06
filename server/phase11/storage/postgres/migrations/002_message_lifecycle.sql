BEGIN;

ALTER TABLE messages
    ADD COLUMN state VARCHAR(16) NOT NULL DEFAULT 'active',
    ADD COLUMN recalled_by BIGINT REFERENCES users(id),
    ADD COLUMN recalled_at TIMESTAMPTZ,
    ADD CONSTRAINT messages_state CHECK (state IN ('active', 'recalled')),
    ADD CONSTRAINT messages_recall_shape CHECK (
        (state = 'active' AND recalled_by IS NULL AND recalled_at IS NULL) OR
        (state = 'recalled' AND recalled_by IS NOT NULL AND recalled_at IS NOT NULL));

INSERT INTO phase11_schema_migrations(version, description)
VALUES (2, 'phase12 persisted message lifecycle and recall metadata');

COMMIT;
