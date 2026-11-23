"""Train, evaluate and deploy a small CPU policy for the synthetic grid fixture."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from hashlib import sha256
import json
import math
import os
from pathlib import Path
from random import Random
import sqlite3
from statistics import mean
import subprocess
import sys
from threading import BoundedSemaphore
from time import monotonic, perf_counter_ns, sleep

from tools.agent.episode_recording import Observation, STATIONARY, STEP, encoded, fixture, replay
from tools.agent.local_process_metrics import sample


SCHEMA = "nebula.grid-policy.v1"
SEEDS = (701, 1701, 2701)


def features(remaining, clearance):
    return (1.0, (clearance - 60) / 50.0, float(clearance >= 60), remaining / 22.0)


def _logistic(weights, values):
    score = max(-30, min(30, sum(weight * value for weight, value in zip(weights, values))))
    return 1.0 / (1.0 + math.exp(-score))


def fit(records, dataset_sha, iterations):
    samples = [(features(row.remaining, row.clearance), row.expert()) for row in records if row.remaining > 0]
    if not samples or not 1 <= iterations <= 200:
        raise ValueError("invalid training inputs")
    weights = [0.0] * 4
    for _ in range(iterations):
        gradient = [0.0] * 4
        for values, expected in samples:
            difference = _logistic(weights, values) - expected
            for index, value in enumerate(values):
                gradient[index] += difference * value
        weights = [weight - 0.35 * change / len(samples) for weight, change in zip(weights, gradient)]
    return {"format": SCHEMA, "datasetSha256": dataset_sha, "iterations": iterations, "weights": weights,
            "scope": "synthetic-grid", "examples": len(samples)}


def action(model, remaining, clearance):
    if not remaining:
        return STATIONARY
    if model is None:
        return Observation("simulation", 0, remaining, clearance).expert()
    return int(_logistic(model["weights"], features(remaining, clearance)) >= 0.5)


def checked_action(candidate, remaining, clearance, age_ms):
    if (type(candidate) is not int or candidate not in (STATIONARY, STEP) or
            not 0 <= age_ms <= 100 or (candidate == STEP and (clearance < 60 or not remaining))):
        return STATIONARY, True
    return candidate, False


def percentile(values, rank):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * rank) - 1)]


def evaluate(model, validation):
    rounds = []
    for seed in SEEDS:
        rng = Random(seed)
        done = refused = 0
        durations = []
        for _ in range(24):
            remaining = rng.randint(6, 10)
            for _ in range(36):
                clearance, age = rng.randrange(100), 180 if rng.randrange(25) == 0 else 15
                started = perf_counter_ns()
                move, rejected = checked_action(action(model, remaining, clearance), remaining, clearance, age)
                durations.append((perf_counter_ns() - started) / 1_000_000)
                refused += rejected
                remaining -= move
                if not remaining:
                    done += 1
                    break
        rounds.append({"seed": seed, "tasks": 24, "completed": done, "safetyRefusals": refused,
                       "calls": len(durations), "p50Ms": percentile(durations, 0.5),
                       "p95Ms": percentile(durations, 0.95), "p99Ms": percentile(durations, 0.99),
                       "residentBytes": sample(os.getpid())["rssBytes"]})
    score = mean(action(model, row.remaining, row.clearance) == row.expert() for row in validation)
    return {"actionAgreement": score, "rollouts": rounds}


class ModelRegistry:
    """Change the active model and record its audit row in the same SQLite transaction."""

    def __init__(self, path):
        self.db = sqlite3.connect(path)
        self.db.executescript("""
            PRAGMA synchronous=FULL;
            CREATE TABLE IF NOT EXISTS models(version TEXT PRIMARY KEY, sha TEXT NOT NULL, artifact BLOB NOT NULL);
            CREATE TABLE IF NOT EXISTS audit(sequence INTEGER PRIMARY KEY, operation TEXT NOT NULL, version TEXT NOT NULL);
        """)

    def register(self, version, model):
        payload = encoded(model)
        digest = sha256(payload).hexdigest()
        with self.db:
            self.db.execute("INSERT OR IGNORE INTO models VALUES(?,?,?)", (version, digest, payload))
            if self.db.execute("SELECT sha FROM models WHERE version=?", (version,)).fetchone()[0] != digest:
                raise ValueError("model version already binds a different artifact")

    def _load(self, version):
        row = self.db.execute("SELECT sha,artifact FROM models WHERE version=?", (version,)).fetchone()
        if row is None or sha256(row[1]).hexdigest() != row[0]:
            raise ValueError("unavailable model version or checksum mismatch")
        model = json.loads(row[1])
        if (model.get("format") != SCHEMA or model.get("scope") != "synthetic-grid" or
                len(model.get("weights", [])) != 4 or any(type(value) not in (float, int) or not math.isfinite(value)
                                                        for value in model["weights"])):
            raise ValueError("unsupported simulation model")
        return model

    def select(self, version, *, rollback=False):
        with self.db:
            if rollback and self.db.execute("SELECT 1 FROM audit WHERE version=?", (version,)).fetchone() is None:
                raise ValueError("rollback requires previously active version")
            model = self._load(version)
            self.db.execute("INSERT INTO audit(operation,version) VALUES (?,?)", ("rollback" if rollback else "activate", version))
        return model

    def current(self):
        row = self.db.execute("SELECT version FROM audit ORDER BY sequence DESC LIMIT 1").fetchone()
        if row is None:
            raise ValueError("no active simulation model")
        return row[0], self._load(row[0])


def train_fixture(directory):
    root = Path(directory)
    if not (root / "episodes/manifest.json").is_file():
        fixture(root / "episodes")
    manifest, observations, quality = replay(root / "episodes")
    groups = {kind: [row for session in manifest["sessions"] if session["split"] == kind
                     for row in observations[session["id"]]] for kind in ("train", "eval")}
    if {row.session for row in groups["train"]} & {row.session for row in groups["eval"]}:
        raise ValueError("training sessions overlap evaluation")
    registry = ModelRegistry(root / "models.sqlite")
    try:
        report = {"data": {"recordingId": manifest["recordingId"], "sha256": manifest["sha256"],
                           "trainFrames": len(groups["train"]), "evalFrames": len(groups["eval"]),
                           "qualityRejected": quality["rejected"]},
                  "rules": evaluate(None, groups["eval"]), "models": {}}
        for version, iterations in (("grid-1", 25), ("grid-2", 65)):
            model = fit(groups["train"], manifest["sha256"], iterations)
            report["models"][version] = {"artifactSha256": sha256(encoded(model)).hexdigest(),
                                         **evaluate(model, groups["eval"])}
            registry.register(version, model)
        original = registry.select("grid-1")
        registry.select("grid-2")
        restored = registry.select("grid-1", rollback=True)
        report["rollbackReproduced"] = original == restored and all(
            action(original, row.remaining, row.clearance) == action(restored, row.remaining, row.clearance)
            for row in groups["eval"])
        report["audit"] = list(registry.db.execute("SELECT operation,version FROM audit ORDER BY sequence"))
        return report
    finally:
        registry.db.close()


def worker(database):
    message = sys.stdin.buffer.read(4097)
    if len(message) > 4096:
        raise ValueError("oversized inference request")
    doc = json.loads(message)
    remaining, clearance, age = (doc.get(key) for key in ("remaining", "clearance", "ageMs"))
    if (not all(type(value) is int for value in (remaining, clearance, age)) or
            not 0 <= remaining <= 22 or not 0 <= clearance <= 100 or not 0 <= age <= 60_000):
        raise ValueError("invalid grid observation")
    # Delay is available only in the offline fault fixture, never from an HTTP request.
    delay = doc.get("fixtureDelayMs", 0)
    if type(delay) is not int or not 0 <= delay <= 2000:
        raise ValueError("invalid simulation delay")
    sleep(delay / 1000)
    registry = ModelRegistry(database)
    try:
        version, model = registry.current()
        step, refused = checked_action(action(model, remaining, clearance), remaining, clearance, age)
        return {"state": "ready", "version": version, "step": step, "refused": refused}
    finally:
        registry.db.close()


class InferenceLane:
    def __init__(self, database):
        self.database = Path(database).resolve()
        self.admission = BoundedSemaphore(2)
        self.executor = ThreadPoolExecutor(max_workers=1)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.executor.shutdown()

    def offer(self, observation, timeout):
        if not 0.05 <= timeout <= 5:
            raise ValueError("invalid simulation time budget")
        if not self.admission.acquire(blocking=False):
            return None
        try:
            future = self.executor.submit(self._execute, observation.copy(), monotonic() + timeout)
        except BaseException:
            self.admission.release()
            raise
        future.add_done_callback(lambda _: self.admission.release())
        return future

    def _execute(self, observation, deadline):
        remaining = deadline - monotonic()
        if remaining <= 0:
            return {"state": "timeout", "step": STATIONARY}
        try:
            response = subprocess.run([sys.executable, "-B", "-m", "tools.agent.simulation_models", "worker", self.database],
                                      cwd=Path(__file__).resolve().parents[2], input=encoded(observation),
                                      capture_output=True, timeout=remaining, check=False)
        except subprocess.TimeoutExpired:
            return {"state": "timeout", "step": STATIONARY}
        if response.returncode:
            return {"state": "unavailable", "step": STATIONARY}
        return json.loads(response.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("train", "worker"))
    parser.add_argument("path", type=Path)
    args = parser.parse_args()
    result = train_fixture(args.path) if args.operation == "train" else worker(args.path)
    print(json.dumps(result, ensure_ascii=True, indent=2))


if __name__ == "__main__":
    main()
