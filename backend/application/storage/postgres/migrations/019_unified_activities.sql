BEGIN;
-- tree_posts 是旧版本的规范正文表，继续保留原名/原 ID/外键。
-- 此处加频道而不复制正文：旧链接、评论、点赞、榜单引用不变。
ALTER TABLE tree_posts ADD COLUMN tree_channel BOOLEAN NOT NULL DEFAULT TRUE;
ALTER TABLE tree_posts ADD COLUMN profile_channel BOOLEAN NOT NULL DEFAULT TRUE;
ALTER TABLE tree_posts ADD COLUMN visibility TEXT NOT NULL DEFAULT 'members'
 CHECK(visibility IN ('members','friends','private'));
ALTER TABLE tree_posts ALTER COLUMN category_id DROP NOT NULL;
ALTER TABLE tree_posts ADD CONSTRAINT activity_has_channel CHECK(tree_channel OR profile_channel);
ALTER TABLE tree_posts ADD CONSTRAINT activity_tree_is_public
 CHECK(NOT tree_channel OR (visibility='members' AND category_id IS NOT NULL));
CREATE INDEX activity_profile_page_idx ON tree_posts(author_id,id DESC)
 WHERE state='published' AND profile_channel;
CREATE INDEX activity_tree_page_idx ON tree_posts(id DESC)
 WHERE state='published' AND tree_channel;
CREATE TABLE activity_favorites (
 user_id BIGINT NOT NULL REFERENCES users(id),
 post_id BIGINT NOT NULL REFERENCES tree_posts(id),
 created_ms BIGINT NOT NULL,
 PRIMARY KEY(user_id,post_id)
);
CREATE TABLE profile_display_settings (
 user_id BIGINT PRIMARY KEY REFERENCES users(id),
 show_biography BOOLEAN NOT NULL DEFAULT TRUE,
 show_avatar BOOLEAN NOT NULL DEFAULT TRUE
);
INSERT INTO phase11_schema_migrations(version,description)
 VALUES(19,'unified activity channels audiences favorites profile display');
COMMIT;
