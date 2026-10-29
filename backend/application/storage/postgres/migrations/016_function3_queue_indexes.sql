BEGIN;
-- 失败顺序检查只搜索尚未发布的同 aggregate 事件，不反复扫描整张历史 Outbox。
CREATE INDEX function3_outbox_aggregate_pending_idx
 ON function3_outbox(aggregate_type,aggregate_id,id) WHERE state<>'published';
-- 完整外键索引也覆盖已删除回复和历史榜单，避免维护时逐行扫描子表。
CREATE INDEX tree_replies_post_fk_idx ON tree_replies(post_id);
CREATE INDEX tree_rank_entries_post_fk_idx ON tree_rank_entries(post_id);
CREATE INDEX announcement_playback_announcement_idx ON announcement_playback_claims(announcement_id);
INSERT INTO phase11_schema_migrations(version,description)
 VALUES(16,'Function3 pending aggregate ordering and complete child-key indexes');
COMMIT;
