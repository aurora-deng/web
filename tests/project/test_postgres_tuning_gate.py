"""Synthetic offline evidence only: these tests do not show real performance improvements."""
import copy
from pathlib import Path
import sys
import unittest
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.function3.validation.measurement_noise import digest
from scripts.function3.validation.postgres_tuning_gate import evaluate, validate_tuning_context
from scripts.function3.validation.postgres_settings import SETTINGS, parse_snapshot, record_round_settings


def fixture():
    binary = "a" * 64
    config, workload = {"durable": True, "ai": False}, {"operations": 4000}
    def row(n, ack, fast):
        return {"round": n, "ackP99Ms": ack, "fastQps": fast, "errors": 0, "ackSamples": 1200,
                "binarySha256": binary, "fixtureSha256": "b" * 64,
                "configurationSha256": digest(config), "workloadSha256": digest(workload)}
    calibration = {"mode": "calibration", "roundsRequested": 10, "calibrationCompleted": True,
                   "gateEvaluated": False, "baselineBinarySha256": binary, "currentBinarySha256": binary,
                   "configuration": config, "workload": workload,
                   "baseline": [row(n, 10, 1000) for n in range(1, 11)],
                   "current": [row(n, 10.01, 1000) for n in range(1, 11)]}
    old = {name: "1" for name in SETTINGS}
    old.update(fsync="on", synchronous_commit="on", full_page_writes="on", commit_delay="0", commit_siblings="5")
    performance = {"mode": "postgres-tuning", "roundsRequested": 3, "baselineBinarySha256": binary, "currentBinarySha256": binary,
                   "configuration": config, "workload": workload,
                   "baseline": [row(n, 10, 1000) for n in range(1, 4)],
                   "current": [row(n, 7.9, 950) for n in range(1, 4)],
                   "postgresSettings": {"baseline": old, "current": dict(old, commit_delay="1000")}}
    calibration["postgresSettings"] = {"baseline": dict(old), "current": dict(old)}
    for report in (calibration, performance):
        for variant in ("baseline", "current"):
            values = report["postgresSettings"][variant]
            text = "\n".join("\t".join(("function3_baseline_test", name, value, "", "user", "default", "false"))
                             for name, value in sorted(values.items()))
            observed = parse_snapshot(text, "function3_baseline_test")
            for item in report[variant]:
                item["database"] = "function3_baseline_test"
                record_round_settings(report, variant, copy.deepcopy(observed), copy.deepcopy(observed), item)
    return {"calibration": calibration, "performance": performance}


class TuningGate(unittest.TestCase):
    def test_synthetic_good_evidence_passes_without_changing_input(self):
        data = fixture()
        before = copy.deepcopy(data)
        result = evaluate(data)
        self.assertTrue(result["passed"])
        self.assertFalse(result["settingsApplied"])
        self.assertEqual(data, before)

    def test_ack_below_20_percent_fails_without_discarding_rounds(self):
        data = fixture(); data["performance"]["current"][1]["ackP99Ms"] = 9
        self.assertFalse(evaluate(data)["passed"])

    def test_one_fast_regression_over_10_percent_fails(self):
        data = fixture(); data["performance"]["current"][2]["fastQps"] = 899
        self.assertFalse(evaluate(data)["passed"])

    def test_durability_changes_are_rejected(self):
        for variant in ("baseline", "current"):
            for key in ("fsync", "synchronous_commit", "full_page_writes"):
                data = fixture(); data["performance"]["postgresSettings"][variant][key] = "off"
                with self.subTest(variant=variant, key=key), self.assertRaises(ValueError): evaluate(data)

    def test_errors_missing_rounds_and_relabelled_acceptance_are_rejected(self):
        for kind in ("errors", "rounds", "requested", "mode"):
            data = fixture()
            if kind == "errors": data["performance"]["current"][0]["errors"] = 1
            elif kind == "rounds": data["performance"]["baseline"].pop()
            elif kind == "requested": data["performance"]["roundsRequested"] = 4
            else: data["performance"]["mode"] = "acceptance"
            with self.subTest(kind=kind), self.assertRaises(ValueError): evaluate(data)

    def test_noise_workload_and_binary_mismatch_are_rejected(self):
        for kind in ("noise", "workload", "binary", "fixture", "samples", "unchangedSettings", "settingsIdentity"):
            data = fixture()
            if kind == "noise": data["calibration"]["current"][0]["ackP99Ms"] = 100
            elif kind == "workload": data["performance"]["workload"] = {"operations": 5000}
            elif kind == "binary": data["performance"]["currentBinarySha256"] = "c" * 64
            elif kind == "fixture": data["performance"]["current"][0]["fixtureSha256"] = "c" * 64
            elif kind == "samples": data["performance"]["current"][0]["ackSamples"] = 10
            elif kind == "settingsIdentity": data["performance"]["current"][0]["postgresSettingsSha256"] = "c" * 64
            else: data["performance"]["postgresSettings"]["current"] = data["performance"]["postgresSettings"]["baseline"].copy()
            with self.subTest(kind=kind), self.assertRaises(ValueError): evaluate(data)

    def test_round_provenance_cannot_be_missing_forged_or_unstable(self):
        for kind in ("missing", "afterDigest", "stable", "source", "fresh", "pending", "time", "db", "metadata", "extraSetting"):
            data = fixture()
            row = data["performance"]["current"][0]
            snapshot = row["postgresSettingsObservation"]["after"]
            if kind == "missing": row.pop("postgresSettingsObservation")
            elif kind == "afterDigest": row["postgresSettingsAfterSha256"] = "c" * 64
            elif kind == "stable": row["postgresSettingsStable"] = 1
            elif kind == "source": snapshot["source"] = "invented"
            elif kind == "fresh": snapshot["fresh"] = False
            elif kind == "pending": snapshot["metadata"]["fsync"]["pendingRestart"] = True
            elif kind == "time": snapshot["readAt"] = "2000-01-01T00:00:00+00:00"
            elif kind == "db": row["database"] = "phase11"
            elif kind == "metadata": snapshot["metadata"]["fsync"]["unit"] = "changed"
            else: snapshot["settings"]["unobserved"] = "on"
            with self.subTest(kind=kind), self.assertRaises(ValueError): evaluate(data)

    def test_calibration_observation_and_baseline_settings_are_required(self):
        for kind in ("calibrationObservation", "baselineSettings", "globalTuning"):
            data = fixture()
            if kind == "calibrationObservation": data["calibration"]["baseline"][0].pop("postgresSettingsObservation")
            elif kind == "baselineSettings": data["calibration"]["postgresSettings"]["current"]["commit_delay"] = "1"
            else: data["performance"]["postgresSettings"]["current"]["shared_buffers"] = "2"
            with self.subTest(kind=kind), self.assertRaises((ValueError, KeyError)): evaluate(data)

    def test_context_preflight_fails_before_any_database_work(self):
        data = fixture()
        report = data["performance"]
        self.assertIs(validate_tuning_context(data["calibration"], report["baselineBinarySha256"],
                                             report["configuration"], report["workload"]), data["calibration"])
        for key in ("configuration", "workload"):
            wrong = copy.deepcopy(report)
            wrong[key]["mismatched"] = True
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_tuning_context(data["calibration"], report["baselineBinarySha256"],
                                        wrong["configuration"], wrong["workload"])


if __name__ == "__main__": unittest.main()
