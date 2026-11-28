"""Synthetic pg_settings responses; never launches psql or connects to a database."""
import copy
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.function3.validation import postgres_settings as settings

CONNECTION = f"host={settings.SOCKET} port=55434 user=function3_test_admin dbname=function3_baseline_test"


def response(database="function3_baseline_test", **changes):
    values = {name: "1" for name in settings.SETTINGS}
    values.update({name: "on" for name in settings.DURABILITY})
    values.update(server_version_num="180006", commit_delay="0", commit_siblings="5")
    values.update(changes)
    return "\n".join("\t".join((database, name, value, "", "user", "default", "false"))
                     for name, value in sorted(values.items())) + "\n"


class SettingsSnapshots(unittest.TestCase):
    def test_snapshot_is_bounded_complete_and_has_provenance(self):
        observed = settings.parse_snapshot(response(), "function3_baseline_test")
        self.assertEqual(set(observed["settings"]), set(settings.SETTINGS))
        self.assertEqual(observed["settingsSha256"], settings.digest(observed["settings"]))
        self.assertEqual(observed["source"], "isolated.pg_settings")
        self.assertTrue(observed["fresh"])
        self.assertIn("unit", observed["metadata"]["shared_buffers"])

    def test_missing_duplicate_oversize_or_other_database_is_rejected(self):
        original = response()
        for data in ("\n".join(original.splitlines()[1:]), original + original.splitlines()[0] + "\n",
                     "x" * 16385, response(database="phase11"), original.replace("\tfalse", "\tunknown", 1)):
            with self.subTest(data=data[:40]), self.assertRaises(ValueError):
                settings.parse_snapshot(data, "function3_baseline_test")
        with self.assertRaises(ValueError): settings.parse_snapshot(original, "phase11")

    def test_capture_uses_only_fixed_query_and_disables_credential_files(self):
        calls = []
        def fake_run(command, **kwargs):
            calls.append((command, kwargs))
            return SimpleNamespace(returncode=0, stdout=response())
        observed = settings.capture_settings("/approved/psql", CONNECTION, "function3_baseline_test", run=fake_run)
        command, kwargs = calls[0]
        self.assertIn("--no-password", command)
        self.assertIn("--no-psqlrc", command)
        self.assertIn(settings.SQL, command[-1])
        self.assertIn("BEGIN READ ONLY", command[-1])
        self.assertIn("statement_timeout", command[-1])
        self.assertLessEqual(kwargs["timeout"], 4)
        self.assertNotIn("PGPASSWORD", kwargs["env"])
        self.assertNotIn("PGOPTIONS", kwargs["env"])
        self.assertEqual(kwargs["env"]["PGPASSFILE"], settings.os.devnull)
        self.assertEqual(observed["database"], "function3_baseline_test")

    def test_connection_and_option_overrides_cannot_escape_test_cluster(self):
        invalid = [CONNECTION.replace("55434", "5432"), CONNECTION + " password=forbidden-fixture",
                   CONNECTION + " user=another", CONNECTION.replace(settings.SOCKET, "127.0.0.1"),
                   CONNECTION + " options='-c fsync=off'", CONNECTION + " options='-c commit_delay=10001'",
                   CONNECTION + " options='-c commit_delay=1;DROP'", CONNECTION + " options='-c unknown=1'",
                   CONNECTION + " options='-c commit_delay=1 -c commit_delay=2'"]
        for connection in invalid:
            with self.subTest(connection=connection), self.assertRaises(ValueError):
                settings.validate_connection(connection, "function3_baseline_test")
        self.assertEqual(settings.validate_connection(CONNECTION + " options='-c commit_delay=1000 -c commit_siblings=5'",
                                                      "function3_baseline_test").split()[0], CONNECTION.split()[0])

    def test_query_failure_does_not_disclose_stderr(self):
        def failure(*a, **kw): return SimpleNamespace(returncode=1, stdout="", stderr="private-fixture-do-not-echo")
        with self.assertRaises(RuntimeError) as caught:
            settings.capture_settings("psql", CONNECTION, "function3_baseline_test", run=failure)
        self.assertNotIn("private-fixture", str(caught.exception))

    def test_round_records_matching_observed_settings_and_rejects_drift(self):
        before = settings.parse_snapshot(response(), "function3_baseline_test")
        after = copy.deepcopy(before)
        summary, entry = {}, {}
        settings.record_round_settings(summary, "baseline", before, after, entry)
        self.assertTrue(entry["postgresSettingsStable"])
        self.assertEqual(entry["postgresSettingsSha256"], before["settingsSha256"])
        changed = settings.parse_snapshot(response(commit_delay="1000"), "function3_baseline_test")
        with self.assertRaises(ValueError): settings.record_round_settings(summary, "baseline", before, changed, {})
        with self.assertRaises(ValueError): settings.record_round_settings(summary, "baseline", changed, changed, {})
        settings.record_round_settings(summary, "current", changed, changed, {})

    def test_durability_and_pending_restart_rejected(self):
        for key in settings.DURABILITY:
            snapshot = settings.parse_snapshot(response(**{key: "off"}), "function3_baseline_test")
            with self.subTest(key=key), self.assertRaises(ValueError):
                settings.record_round_settings({}, "baseline", snapshot, snapshot, {})
        snapshot = settings.parse_snapshot(response(), "function3_baseline_test")
        snapshot["metadata"]["shared_buffers"]["pendingRestart"] = True
        with self.assertRaises(ValueError): settings.record_round_settings({}, "baseline", snapshot, snapshot, {})

    def test_snapshot_digest_tampering_is_rejected(self):
        snapshot = settings.parse_snapshot(response(), "function3_baseline_test")
        snapshot["settings"]["commit_delay"] = "1000"
        with self.assertRaises(ValueError): settings.record_round_settings({}, "baseline", snapshot, snapshot, {})

    def test_marked_instance_requires_exact_server_identity_before_writes(self):
        identity = "\t".join(("function3_baseline_test", "function3_test_admin",
                               str(settings.CLUSTER / "data"), "55434", settings.SOCKET, "")) + "\n"
        calls = []
        def run(command, **kw):
            calls.append(command)
            return SimpleNamespace(returncode=0, stdout=identity)
        with patch.object(Path, "resolve", lambda self: self), patch.object(Path, "is_symlink", return_value=False), \
             patch.object(Path, "is_file", return_value=True), patch.object(Path, "stat", return_value=SimpleNamespace(st_size=77)), \
             patch.object(Path, "read_text", return_value=settings.MARKER):
            self.assertTrue(settings.verify_test_cluster("psql", CONNECTION, "function3_baseline_test", run=run)["verified"])
            for field in ("function3_baseline_test", "function3_test_admin", "55434", settings.SOCKET):
                def wrong(*a, **kw): return SimpleNamespace(returncode=0, stdout=identity.replace(field, "wrong", 1))
                with self.subTest(field=field), self.assertRaises(ValueError):
                    settings.verify_test_cluster("psql", CONNECTION, "function3_baseline_test", run=wrong)
            with patch.object(Path, "read_text", return_value="not a disposable cluster"):
                before = len(calls)
                with self.assertRaises(ValueError): settings.verify_test_cluster("psql", CONNECTION, "function3_baseline_test", run=run)
                self.assertEqual(len(calls), before)
            with patch.object(Path, "is_symlink", return_value=True):
                with self.assertRaises(ValueError): settings.verify_test_cluster("psql", CONNECTION, "function3_baseline_test", run=run)

    def test_observation_requires_timezone_bool_metadata_and_matching_database(self):
        for kind in ("timezone", "fresh", "pendingType", "database"):
            snapshot = settings.parse_snapshot(response(), "function3_baseline_test")
            entry = {"database": "function3_baseline_test"}
            if kind == "timezone": snapshot["readAt"] = "2026-11-27T00:00:00"
            elif kind == "fresh": snapshot["fresh"] = 1
            elif kind == "pendingType": snapshot["metadata"]["fsync"]["pendingRestart"] = 0
            else: entry["database"] = "function3_phase11_test"
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                settings.record_round_settings({}, "baseline", snapshot, snapshot, entry)


if __name__ == "__main__": unittest.main()
