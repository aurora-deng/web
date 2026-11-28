#!/usr/bin/env python3
"""Contract tests for the offline synthetic dataset generator."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import uuid
import unittest


ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "scripts" / "ai" / "data" / "generate_synthetic_dataset.py"
SPEC = importlib.util.spec_from_file_location("phase11_generator", GENERATOR)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load generator module: {GENERATOR}")
GENERATOR_MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR_MODULE)


class TrainingDataGeneratorTests(unittest.TestCase):
    def test_seeded_output_is_reproducible_and_manifest_matches(self) -> None:
        records_a = GENERATOR_MODULE.build_records(17, 41)
        records_b = GENERATOR_MODULE.build_records(17, 41)
        payload_a = GENERATOR_MODULE.render_jsonl(records_a)
        payload_b = GENERATOR_MODULE.render_jsonl(records_b)
        self.assertEqual(payload_a, payload_b)
        self.assertEqual(len(records_a), 17)
        self.assertTrue(
            all(record["schema"] == "phase22-chat-jsonl-v1" for record in records_a)
        )
        self.assertTrue(
            all(record["consent_version"] == "pipeline-smoke-only" for record in records_a)
        )
        self.assertEqual(
            len({
                (record["messages"][0]["content"], record["messages"][1]["content"])
                for record in records_a
            }),
            17,
        )
        self.assertEqual(hashlib.sha256(payload_a).hexdigest(), hashlib.sha256(payload_b).hexdigest())

    def test_existing_output_is_not_overwritten_without_force(self) -> None:
        output = ROOT / f".phase11-generator-test-{uuid.uuid4().hex}.jsonl"
        try:
            GENERATOR_MODULE.atomic_write(output, b"first\n", force=False)
            with self.assertRaises(FileExistsError):
                GENERATOR_MODULE.atomic_write(output, b"second\n", force=False)
            self.assertEqual(output.read_bytes(), b"first\n")
        finally:
            output.unlink(missing_ok=True)

    def test_rendered_jsonl_is_valid(self) -> None:
        records = GENERATOR_MODULE.build_records(3, 7)
        payload = GENERATOR_MODULE.render_jsonl(records)
        decoded = [json.loads(line) for line in payload.decode("utf-8").splitlines()]
        self.assertEqual(decoded, records)


if __name__ == "__main__":
    unittest.main()