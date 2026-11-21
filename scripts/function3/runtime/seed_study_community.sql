\set ON_ERROR_STOP on
BEGIN;
SET LOCAL lock_timeout = '5s';
SELECT pg_advisory_xact_lock(193628785, 1);

DO $check$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM phase11_schema_migrations WHERE version = 26)
        OR NOT EXISTS (SELECT 1 FROM tree_categories WHERE name = '考研') THEN
        RAISE EXCEPTION 'Apply the community migrations through 026 before loading study examples';
    END IF;
END $check$;

INSERT INTO users (username, display_name, password_hash, disabled) VALUES
    ('nebula_study_examples', '研途 · 内置示例', '!nebula-study-example-v1', TRUE),
    ('nebula_study_example_replies', '研途 · 示例讨论', '!nebula-study-example-v1', TRUE)
ON CONFLICT (username) DO NOTHING;

DO $check$
BEGIN
    IF (SELECT count(*) FROM users WHERE username IN ('nebula_study_examples', 'nebula_study_example_replies')
        AND disabled AND password_hash = '!nebula-study-example-v1') <> 2 THEN
        RAISE EXCEPTION 'Reserved example authors conflict with existing accounts';
    END IF;
END $check$;

CREATE TEMP TABLE study_example_posts (
    slug TEXT PRIMARY KEY, title TEXT NOT NULL, body TEXT NOT NULL, tag TEXT NOT NULL
) ON COMMIT DROP;

INSERT INTO study_example_posts VALUES
    ('weekly-review', '把每周复盘做成一页',
     '周日只写三行：这周哪一步见效、哪一步卡住、下周先做什么。复盘结束后选一个能在二十分钟内启动的小任务。你会在什么时候做复盘？', '每周复盘'),
    ('mistake-notes', '整理错题时，我先记录第一次卡住的位置',
     '用一行写触发错误的条件，再留一道可独立重做的小题。第二次碰到同类题时先遮住原答案。你用什么信号决定一题已经掌握？', '错题复习'),
    ('reading', '英语阅读复盘，试试把选项送回原文',
     '为每个犹豫过的选项标出对应句子，再区分偷换范围、时态和因果。第三遍只回看这些标记，省下时间重做。', '英语阅读'),
    ('school-list', '择校表先填三列，再去查细节',
     '从目标方向、初试科目和公开资料更新时间开始。来源留链接，年份单独注明。后续有新公告时能快速复核旧判断。', '择校交流'),
    ('focus', '每天只预留一段不可压缩的专注时间',
     '把最耗脑的任务放在这段时间，结束时给下一次留下一个具体起点。忙碌时我只求完成预定的小步。你怎么安排自己的专注时段？', '学习计划');

INSERT INTO tree_posts (author_id, request_key, canonical_request, body, category_id, created_ms,
                        tree_channel, profile_channel, visibility)
SELECT u.id, 'study-example:v1:' || s.slug, '{"source":"nebula-study-example-v1"}',
       '【站内示例】' || s.title || E'\n\n' || s.body, c.id,
       (extract(epoch FROM transaction_timestamp()) * 1000)::bigint, TRUE, FALSE, 'members'
FROM study_example_posts s
CROSS JOIN users u CROSS JOIN tree_categories c
WHERE u.username = 'nebula_study_examples' AND c.name = '考研'
ON CONFLICT (author_id, request_key) DO NOTHING;

INSERT INTO tree_tags (post_id, tag)
SELECT p.id, tags.tag FROM study_example_posts s
JOIN users u ON u.username = 'nebula_study_examples'
JOIN tree_posts p ON p.author_id = u.id AND p.request_key = 'study-example:v1:' || s.slug
CROSS JOIN LATERAL (VALUES ('站内示例'), (s.tag)) AS tags (tag)
ON CONFLICT DO NOTHING;

CREATE TEMP TABLE study_example_replies (
    slug TEXT NOT NULL REFERENCES study_example_posts, seq SMALLINT NOT NULL,
    parent_seq SMALLINT, body TEXT NOT NULL, PRIMARY KEY (slug, seq)
) ON COMMIT DROP;

INSERT INTO study_example_replies VALUES
    ('weekly-review', 1, NULL, '【示例评论】我会把复盘排在周日第一段学习结束后。试过晚上补写，经常想不起周初卡住的细节。'),
    ('weekly-review', 2, 1, '【示例回复】可以把零散卡点先记在任务旁，周日只回看标记过的那几条。'),
    ('weekly-review', 3, NULL, '【示例评论】下周只改一件事，这样容易看出变化来自哪里。'),
    ('mistake-notes', 1, NULL, '【示例评论】我用间隔两天的一次空白重做检验。能说出关键条件，也能说明另外一种解法为什么不适用，才记作掌握。'),
    ('mistake-notes', 2, 1, '【示例回复】我还会补一个反例，提醒自己不要把口诀套到所有题上。'),
    ('reading', 1, NULL, '【示例评论】我把定位错误和理解错误分开，定位错就重找原句，理解错再拆句。'),
    ('reading', 2, 1, '【示例回复】遇到同义改写可以在原句和选项各划一处，回看时会更快。'),
    ('school-list', 1, NULL, '【示例评论】可以多记一列公告发布单位，后续复核容易找到原始页面。'),
    ('school-list', 2, 1, '【示例回复】把待核实的条件单独标出来，下次查询就从这几条开始。'),
    ('focus', 1, NULL, '【示例评论】我会在收尾写下下一次打开资料要看的页码，早上省下不少找状态的时间。'),
    ('focus', 2, 1, '【示例回复】做套题也适用，收尾时记一条待验证的思路就够了。'),
    ('focus', 3, NULL, '【示例评论】计划断掉时我先安排十分钟重启任务，完成后再决定剩下的时间怎么用。');

CREATE TEMP VIEW study_example_reply_rows AS
SELECT p.id AS post_id, CASE WHEN r.parent_seq IS NULL THEN commenter.id ELSE article.id END AS author_id,
       commenter.id AS parent_author_id, 'study-example:v1:' || r.slug || ':' || r.seq AS request_key,
       CASE WHEN r.parent_seq IS NOT NULL THEN 'study-example:v1:' || r.slug || ':' || r.parent_seq END AS parent_key,
       r.body, p.created_ms + r.seq AS created_ms
FROM study_example_replies r
JOIN users article ON article.username = 'nebula_study_examples'
JOIN users commenter ON commenter.username = 'nebula_study_example_replies'
JOIN tree_posts p ON p.author_id = article.id AND p.request_key = 'study-example:v1:' || r.slug;

INSERT INTO tree_replies (post_id, author_id, request_key, body, created_ms)
SELECT post_id, author_id, request_key, body, created_ms FROM study_example_reply_rows WHERE parent_key IS NULL
ON CONFLICT (author_id, request_key) DO NOTHING;

INSERT INTO tree_replies (post_id, author_id, request_key, body, created_ms)
SELECT post_id, author_id, request_key, body, created_ms FROM study_example_reply_rows WHERE parent_key IS NOT NULL
ON CONFLICT (author_id, request_key) DO NOTHING;

INSERT INTO tree_reply_links (reply_id, parent_reply_id)
SELECT child.id, parent.id FROM study_example_reply_rows r
JOIN tree_replies child ON child.author_id = r.author_id AND child.request_key = r.request_key AND child.post_id = r.post_id
JOIN tree_replies parent ON parent.author_id = r.parent_author_id AND parent.request_key = r.parent_key AND parent.post_id = r.post_id
ON CONFLICT DO NOTHING;

SELECT (SELECT count(*) FROM study_example_posts) AS example_posts,
       count(*) AS example_replies, count(parent_key) AS nested_replies FROM study_example_reply_rows;
DROP VIEW study_example_reply_rows;
COMMIT;
