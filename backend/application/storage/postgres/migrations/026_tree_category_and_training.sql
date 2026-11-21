BEGIN;
CREATE TABLE tree_category_rank_entries (
    version_id BIGINT NOT NULL REFERENCES tree_rank_versions(id) ON DELETE CASCADE,
    category_id BIGINT NOT NULL REFERENCES tree_categories(id),
    rank SMALLINT NOT NULL CHECK (rank BETWEEN 1 AND 10),
    post_id BIGINT NOT NULL REFERENCES tree_posts(id),
    score DOUBLE PRECISION NOT NULL,
    PRIMARY KEY (version_id, category_id, rank),
    UNIQUE (version_id, category_id, post_id)
);

CREATE TABLE tree_training_choices (
    post_id BIGINT PRIMARY KEY REFERENCES tree_posts(id) ON DELETE CASCADE,
    allowed BOOLEAN NOT NULL,
    reviewed_text TEXT CHECK (char_length(reviewed_text) BETWEEN 1 AND 2000),
    reviewer_id BIGINT REFERENCES users(id),
    CHECK ((reviewed_text IS NULL AND reviewer_id IS NULL) OR
           (allowed AND reviewed_text IS NOT NULL AND reviewer_id IS NOT NULL))
);

INSERT INTO tree_categories (name) VALUES ('考研') ON CONFLICT (name) DO NOTHING;
INSERT INTO phase11_schema_migrations (version,description) VALUES (26,'category rankings and community training consent');
COMMIT;
