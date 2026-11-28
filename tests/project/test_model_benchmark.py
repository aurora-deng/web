"""Offline contracts only: no model, DB, remote requests or dependencies."""
import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("synthetic_model_benchmark", ROOT/"scripts/function3/validation/model_benchmark.py")
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def evidence(suite="quality", repeats=3):
    rows = [{"kind": "metadata", "schema": "nebula-onnx-benchmark-v1", "prevalidated": False,
             "modelChecksum": "sha256-tree-v1:"+"a"*64}]
    for repeat in range(1, repeats+1):
        for case in MODULE.cases(suite):
            index = len(rows)
            label = case["id"].split("-")[1] if case["category"] == "classification" else "synthetic response"
            rows.append({"kind": "generation", "id": case["id"], "category": case["category"],
                         "repeat": repeat, "ordinal": index, "maxTokens": int(case["maxTokens"]),
                         "success": True, "cancelled": False, "timedOut": False,
                         "outputLimitExceeded": False, "validUsage": True,
                         "wallMicroseconds": 100_000, "clientFirstTokenMicroseconds": 25_000,
                         "textChunks": 2, "text": label,
                         "usage": {"promptTokens": 12, "outputTokens": 5, "decodeTokens": 4,
                                   "decodeMicroseconds": 50_000, "firstTokenMicroseconds": 20_000,
                                   "modelLoadPerformed": index == 1,
                                   "modelLoadMicroseconds": 10_000 if index == 1 else 0,
                                   "queueWaitMicroseconds": 5}})
    return rows


class ModelBenchmarkTest(unittest.TestCase):
    def test_fixture_stable_balanced_and_not_training_data(self):
        payload = MODULE.fixture("quality")
        self.assertEqual(payload, MODULE.fixture("quality"))
        cases = [json.loads(line) for line in payload.splitlines()]
        self.assertEqual(len(cases), 30)
        self.assertEqual(len({case["id"] for case in cases}), 30)
        self.assertTrue(all(set(c) == {"id", "category", "system", "prompt", "maxTokens"} for c in cases))
        for label in MODULE.CLASSIFICATION:
            self.assertEqual(sum(c["id"].startswith("sentiment-"+label+"-") for c in cases), 8)
        self.assertEqual(MODULE.cases("long")[0]["maxTokens"], "512")
        self.assertNotIn(b"candidate_id", payload)

    def test_actual_ids_not_text_chunks_and_open_questions_not_auto_approved(self):
        report = MODULE.summarize(evidence(), "quality", 3)
        self.assertEqual(report["classification"], {"correct": 72, "total": 72, "accuracy": 1})
        self.assertEqual(report["categories"]["classification"]["medianDecodeTokensPerSecond"], 80)
        self.assertTrue(report["manualReviewRequired"])
        self.assertFalse(report["activationAllowed"])
        self.assertEqual(report["warmRequestCount"], 89)
        self.assertFalse(report["coldProcess"]["osPageCacheDropped"])

    def test_wrong_label_does_not_pass_by_substring(self):
        rows = evidence()
        rows[1]["text"] = "positive, because this sounds good"
        self.assertEqual(MODULE.summarize(rows, "quality", 3)["classification"]["correct"], 71)

    def test_long_output_does_not_claim_512_when_eos_is_early(self):
        report = MODULE.summarize(evidence("long"), "long", 3)
        self.assertEqual(report["categories"]["long"]["reachedRequestedLimit"], 0)
        self.assertEqual(report["categories"]["long"]["outputTokenCounts"], [5, 5, 5])
        self.assertIsNone(report["classification"]["accuracy"])

    def test_missing_duplicate_or_reordered_evidence_rejected(self):
        rows = evidence()
        variants = [rows[:-1], rows+[rows[-1]], [rows[0], rows[2], rows[1]]+rows[3:]]
        for variant in variants:
            with self.subTest(size=len(variant)), self.assertRaises(ValueError):
                MODULE.summarize(variant, "quality", 3)

    def test_corrupt_or_fabricated_metrics_rejected(self):
        for key, value in [("decodeTokens", 2), ("decodeMicroseconds", 0),
                           ("outputTokens", 17), ("outputTokens", True),
                           ("modelLoadPerformed", False), ("modelLoadMicroseconds", -1)]:
            rows = evidence(); rows[1]["usage"][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                MODULE.summarize(rows, "quality", 3)
        for key, value in [("success", False), ("success", 1), ("timedOut", True),
                           ("validUsage", False), ("clientFirstTokenMicroseconds", None)]:
            rows = evidence(); rows[1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                MODULE.summarize(rows, "quality", 3)
        rows = evidence(); rows[2]["usage"]["modelLoadPerformed"] = True
        with self.assertRaises(ValueError): MODULE.summarize(rows, "quality", 3)

    def test_whitelist_does_not_read_unrelated_environment(self):
        class Restricted(dict):
            def items(self): raise AssertionError("do not enumerate secrets")
            def __getitem__(self, key):
                if key == "DATABASE_PASSWORD": raise AssertionError("secret read")
                return super().__getitem__(key)
        env = Restricted(PATH="toolchain", LANG="C.UTF-8", LC_ALL="C.UTF-8", DATABASE_PASSWORD="not-a-real-secret")
        self.assertEqual(MODULE.safe_environment(env), {"PATH": "toolchain", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8"})

    def test_affinity_rejects_unsupported_cpus_and_duplicates(self):
        self.assertEqual(MODULE.cpu_affinity("3,2", {0,1,2,3}), "2,3")
        for value in ("", "2,2", "4", "-1", "2;true", "２", "0,1,2,3,4"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                MODULE.cpu_affinity(value, {0,1,2,3})

    def test_source_never_preloads_or_accesses_database(self):
        source = (ROOT/"tests/phase11/smoke/onnx_model_benchmark.cpp").read_text(encoding="utf-8")
        self.assertNotIn("provider.validateVersion(", source)
        self.assertNotIn("PostgresStore", source)
        self.assertIn("computeOnnxArtifactChecksum", source)
        self.assertIn("benchmark::generate", source)


if __name__ == "__main__": unittest.main()
