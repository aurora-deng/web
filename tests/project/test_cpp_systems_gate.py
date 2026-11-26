"""Keep workflow capacity regression mandatory in the isolated native test gate."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("cpp_systems_gate", ROOT / "scripts/project/cpp_systems.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


class CppSystemsGate(unittest.TestCase):
    baseline = {"nebula_workflow_contract", "nebula_workflow_recovery", "nebula_fleet_contract", "nebula_agent_contract"}

    def invoke(self, directory, names):
        discovered = json.dumps({"tests": [{"name": name} for name in sorted(names)]}).encode()
        with patch.object(sys, "argv", ["cpp_systems.py", "--build-dir", str(directory)]), \
                patch.object(gate, "source_digest", return_value="0" * 64), \
                patch.object(gate.platform, "platform", return_value="isolated-unit-test"), \
                patch.object(gate.subprocess, "check_output", return_value=discovered), \
                patch.object(gate.subprocess, "run") as run:
            gate.main()
            return run.call_args_list

    def test_missing_capacity_is_failure_not_green(self):
        with tempfile.TemporaryDirectory(prefix="nebula-gate-") as directory:
            with self.assertRaisesRegex(RuntimeError, "nebula_workflow_journal_capacity"):
                self.invoke(Path(directory), self.baseline)
            evidence = json.loads((Path(directory) / "cpp-systems-evidence.json").read_text())
            self.assertFalse(evidence["passed"])
            self.assertEqual(evidence["phase"], "test-discovery")

    def test_registered_capacity_reaches_ctest(self):
        with tempfile.TemporaryDirectory(prefix="nebula-gate-") as directory:
            calls = self.invoke(Path(directory), self.baseline | {"nebula_workflow_journal_capacity"})
            self.assertEqual(len(calls), 3)
            self.assertEqual(calls[-1].args[0][0], "ctest")
            evidence = json.loads((Path(directory) / "cpp-systems-evidence.json").read_text())
            self.assertTrue(evidence["passed"])
            self.assertIn("nebula_workflow_journal_capacity", evidence["tests"])


if __name__ == "__main__":
    unittest.main()
