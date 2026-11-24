"""Verify migration 027 against existing Inbox, Outbox and failure history."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from repro_runtime import clean_environment, disposable_database, require


ROOT = Path(__file__).resolve().parents[2]


def run(prefix, output):
    env = clean_environment()
    with tempfile.TemporaryDirectory(prefix="nebula-inbox-upgrade-") as directory:
        with disposable_database(prefix, ROOT, Path(directory), through_version=26) as (connection, old):
            psql = [prefix / "bin/psql", connection, "-XqAtw", "-v", "ON_ERROR_STOP=1"]

            def query(sql):
                return subprocess.check_output(psql, input=sql, text=True, env=env, timeout=15).strip()

            query("""
                INSERT INTO function3_outbox (id,event_type,aggregate_type,aggregate_id,aggregate_version,payload,created_ms,state)
                OVERRIDING SYSTEM VALUE VALUES
                    (21,'tree.post.created','fruit',21,1,'{}',0,'published'),
                    (22,'chat.legacy','chat',22,1,'{}',0,'published'),
                    (23,'tree.post.created','fruit',23,1,'{}',0,'published'),
                    (24,'tree.post.created','fruit',24,1,'{}',0,'pending'),
                    (25,'tree.post.created','fruit',25,1,'{}',0,'published');
                INSERT INTO function3_inbox VALUES
                    ('function3.delivery',21,statement_timestamp()-interval '16 days'),
                    ('chat.realtime',22,statement_timestamp()-interval '15 days'),
                    ('function3.delivery',23,statement_timestamp()-interval '13 days'),
                    ('function3.delivery',24,statement_timestamp()-interval '15 days'),
                    ('function3.delivery',25,statement_timestamp()-interval '15 days'),
                    ('unknown.consumer',26,statement_timestamp()-interval '15 days');
                INSERT INTO function3_delivery_failures(topic,broker_key,event_id,last_error)
                VALUES('fixture','unresolved',25,'failed_fixture');
            """)
            migration = ROOT / "backend/application/storage/postgres/migrations/027_inbox_retention.sql"
            subprocess.run([*psql, "-f", migration], env=env, check=True, timeout=15)
            new = int(query("SELECT max(version) FROM phase11_schema_migrations;"))
            require(old == 26 and new == 27, "migration ledger did not advance from 026 to 027")
            require(query("SELECT count(*) FROM function3_inbox;") == "6", "upgrade deleted history before pruning")
            require(query("SELECT count(*) FROM pg_indexes WHERE indexname='function3_inbox_processed_idx';") == "1",
                    "Inbox cleanup index missing after upgrade")
            batches = [int(query("SELECT function3_prune_inbox(1);")) for _ in range(3)]
            kept = query("SELECT string_agg(event_id::text,',' ORDER BY event_id) FROM function3_inbox;")
            retired = query("SELECT string_agg(id::text,',' ORDER BY id) FROM function3_outbox "
                            "WHERE inbox_retired_at IS NOT NULL;")
            require(batches == [1, 1, 0] and kept == "23,24,25,26" and retired == "21,22",
                    "Inbox cleanup lost history or its durable deduplication marker")
    output.write_text(json.dumps({"passed": True, "fromMigration": old, "toMigration": new,
                                 "batchSizes": batches, "protectedInboxRows": 4,
                                 "durableOutboxMarkers": 2}, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--postgres-prefix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    run(args.postgres_prefix.resolve(), args.output)


if __name__ == "__main__":
    main()
