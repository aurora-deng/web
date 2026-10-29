-- 只保存音色设置和保守的应用预算；密钥留在 Linux 私有文件，不进入数据库。
BEGIN;
CREATE TABLE speech_profiles (
 voice text PRIMARY KEY CHECK(voice IN ('zf_001','zm_009')),
 provider text NOT NULL DEFAULT 'local' CHECK(provider IN ('local','azure','gemini')),
 style text NOT NULL, region text NOT NULL DEFAULT 'eastasia',
 speed double precision NOT NULL DEFAULT 0.95 CHECK(speed BETWEEN 0.75 AND 1.25),
 pitch integer NOT NULL DEFAULT -2 CHECK(pitch BETWEEN -6 AND 3),
 free_confirmed boolean NOT NULL DEFAULT false, data_consent boolean NOT NULL DEFAULT false,
 updated_at timestamptz NOT NULL DEFAULT CURRENT_TIMESTAMP
);
INSERT INTO speech_profiles(voice,style) VALUES('zf_001','calm'),('zm_009','narration-relaxed');
CREATE TABLE speech_usage (
 provider text NOT NULL, bucket text NOT NULL, units bigint NOT NULL CHECK(units>=0),
 PRIMARY KEY(provider,bucket)
);
INSERT INTO phase11_schema_migrations(version,description)
 VALUES(22,'speech profiles and persistent conservative application budgets');
COMMIT;
