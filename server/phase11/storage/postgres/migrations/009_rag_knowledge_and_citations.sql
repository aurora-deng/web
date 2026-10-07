BEGIN;

CREATE TABLE knowledge_documents (
    id              BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    owner_id        BIGINT NOT NULL REFERENCES users(id),
    scope           VARCHAR(16) NOT NULL,
    conversation_id BIGINT REFERENCES conversations(id),
    title           VARCHAR(200) NOT NULL,
    source_name     VARCHAR(255) NOT NULL,
    media_type      VARCHAR(120) NOT NULL,
    sha256          CHAR(64) NOT NULL,
    state           VARCHAR(16) NOT NULL,
    failure_detail  VARCHAR(500) NOT NULL DEFAULT '',
    created_at      TIMESTAMPTZ NOT NULL,
    indexed_at      TIMESTAMPTZ,
    CONSTRAINT knowledge_documents_scope CHECK (
        (scope = 'private' AND conversation_id IS NULL) OR
        (scope = 'conversation' AND conversation_id IS NOT NULL)),
    CONSTRAINT knowledge_documents_state CHECK (
        state IN ('indexing', 'ready', 'failed', 'deleted')),
    CONSTRAINT knowledge_documents_hash CHECK (sha256 ~ '^[0-9a-fA-F]{64}$'),
    CONSTRAINT knowledge_documents_terminal CHECK (
        (state = 'ready' AND indexed_at IS NOT NULL AND failure_detail = '') OR
        (state = 'failed' AND indexed_at IS NULL AND failure_detail <> '') OR
        (state IN ('indexing', 'deleted')))
);
CREATE INDEX knowledge_documents_private_idx
    ON knowledge_documents(owner_id, id) WHERE scope = 'private' AND state = 'ready';
CREATE INDEX knowledge_documents_conversation_idx
    ON knowledge_documents(conversation_id, id)
    WHERE scope = 'conversation' AND state = 'ready';

CREATE TABLE knowledge_chunks (
    id          BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    document_id BIGINT NOT NULL REFERENCES knowledge_documents(id) ON DELETE CASCADE,
    ordinal     BIGINT NOT NULL,
    content     TEXT NOT NULL,
    sha256      CHAR(64) NOT NULL,
    CONSTRAINT knowledge_chunks_ordinal CHECK (ordinal >= 0),
    CONSTRAINT knowledge_chunks_content CHECK (length(content) > 0),
    CONSTRAINT knowledge_chunks_hash CHECK (sha256 ~ '^[0-9a-fA-F]{64}$'),
    CONSTRAINT knowledge_chunks_document_ordinal UNIQUE(document_id, ordinal),
    CONSTRAINT knowledge_chunks_document_id_id UNIQUE(document_id, id)
);

CREATE TABLE rag_retrievals (
    generation_id  VARCHAR(125) PRIMARY KEY,
    requester_id   BIGINT NOT NULL REFERENCES users(id),
    conversation_id BIGINT NOT NULL REFERENCES conversations(id),
    query_sha256   CHAR(64) NOT NULL,
    created_at     TIMESTAMPTZ NOT NULL,
    CONSTRAINT rag_retrievals_hash CHECK (query_sha256 ~ '^[0-9a-fA-F]{64}$')
);

CREATE TABLE rag_citations (
    generation_id VARCHAR(125) NOT NULL REFERENCES rag_retrievals(generation_id)
        ON DELETE CASCADE,
    rank          BIGINT NOT NULL,
    document_id   BIGINT NOT NULL,
    chunk_id      BIGINT NOT NULL,
    score         DOUBLE PRECISION NOT NULL,
    PRIMARY KEY(generation_id, rank),
    CONSTRAINT rag_citations_rank CHECK (rank > 0),
    CONSTRAINT rag_citations_score CHECK (score > 0.0 AND score <= 1.0),
    CONSTRAINT rag_citations_document_chunk FOREIGN KEY(document_id, chunk_id)
        REFERENCES knowledge_chunks(document_id, id)
);

INSERT INTO phase11_schema_migrations(version, description)
VALUES (9, 'rag knowledge documents chunks retrievals and citations');

COMMIT;
