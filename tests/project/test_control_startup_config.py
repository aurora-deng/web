"""Offline Agent control startup configuration contracts; no service/DB/network IO."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts/function3/runtime"))
from control_config import load_control_environment


class ControlStartupConfig(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="control-startup-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root / "agent-control.json"

    def write(self, data):
        self.path.write_text(json.dumps(data), encoding="utf-8")
        self.path.chmod(0o600)

    def valid(self):
        return {"schemaVersion": 1, "enabled": True, "operators": [71], "approvers": ["71"]}

    def test_absent_preserves_legacy(self):
        self.assertIsNone(load_control_environment(self.root))

    def test_explicit_enable_has_only_three_keys(self):
        self.write(self.valid())
        self.assertEqual(load_control_environment(self.root), {
            "WEB_AGENT_CONTROL_ENABLE": "1", "WEB_AGENT_CONTROL_USER_IDS": "71",
            "WEB_AGENT_CONTROL_APPROVER_IDS": "71"})

    def test_disabled_can_have_empty_lists(self):
        data = self.valid(); data.update(enabled=False, operators=[], approvers=[])
        self.write(data)
        self.assertEqual(load_control_environment(self.root)["WEB_AGENT_CONTROL_ENABLE"], "0")

    def test_invalid_types_fields_and_ids_fail_closed(self):
        variants = [{"schemaVersion": True}, {"schemaVersion": 2}, {"enabled": "1"},
                    {"operators": []}, {"approvers": []}, {"operators": [0]},
                    {"operators": [-1]}, {"operators": [True]}, {"operators": ["071"]},
                    {"operators": ["1,71"]}, {"operators": [" 71"]},
                    {"operators": [1 << 64]}, {"operators": [71, "71"]},
                    {"operators": list(range(1, 66))}, {"unknown": "not allowed"}]
        for change in variants:
            with self.subTest(change=change):
                data = self.valid(); data.update(change); self.write(data)
                with self.assertRaises(ValueError): load_control_environment(self.root)

    def test_oversized_and_duplicate_json_fail_closed(self):
        for raw in [' ' * 4097, '{"enabled":true,"enabled":false}', '[1,2]', '{broken']:
            with self.subTest(raw_length=len(raw)):
                self.path.write_text(raw, encoding="utf-8"); self.path.chmod(0o600)
                with self.assertRaises(ValueError): load_control_environment(self.root)

    @unittest.skipIf(os.name == "nt", "POSIX permissions contract")
    def test_world_writable_rejected(self):
        self.write(self.valid()); self.path.chmod(0o666)
        with self.assertRaises(ValueError): load_control_environment(self.root)

    def test_symlink_rejected(self):
        target = self.root / "target.json"
        target.write_text(json.dumps(self.valid()), encoding="utf-8")
        try: self.path.symlink_to(target)
        except OSError: self.skipTest("symlink unavailable")
        with self.assertRaises(ValueError): load_control_environment(self.root)

    def test_daily_start_applies_before_process_launch(self):
        source = (ROOT / "scripts/function3/runtime/service.py").read_text(encoding="utf-8")
        self.assertIn("control_environment=load_control_environment(root)", source)
        self.assertLess(source.index("env.update(control_environment)"), source.index("subprocess.Popen"))


if __name__ == "__main__":
    unittest.main()
