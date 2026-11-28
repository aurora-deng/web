"""Offline gate for explicit PostgreSQL-only A/B evidence; never connects or applies settings.

Requires the existing verified A/A format plus three or more paired same-binary
rounds. This is a new tuning gate, not a reinterpretation of historical verdicts.
"""
import argparse
import json
from pathlib import Path
import statistics

try:
    from .measurement_noise import analyse, digest, paired_rounds
    from .postgres_settings import record_round_settings
except ImportError:
    from measurement_noise import analyse, digest, paired_rounds
    from postgres_settings import record_round_settings

DURABILITY = ("fsync", "synchronous_commit", "full_page_writes")


def validate_tuning_context(calibration_document, binary, configuration, workload):
    """Reject mismatched experiments before the harness performs any test DB writes."""
    if analyse(calibration_document).get("level") != "DESCRIPTIVE_ONLY":
        raise ValueError("complete quiet same-binary A/A calibration is required first")
    calibration = calibration_document.get("performance", calibration_document)
    if calibration["baselineBinarySha256"] != binary:
        raise ValueError("calibration binary does not match tuning executable")
    if calibration["configuration"] != configuration or calibration["workload"] != workload:
        raise ValueError("workload and non-PostgreSQL configuration must match calibration")
    values = calibration["postgresSettings"]
    if values["baseline"] != values["current"]:
        raise ValueError("A/A effective PostgreSQL settings must match")
    for b, c in paired_rounds(calibration):
        verify_round_observation(calibration, "baseline", b)
        verify_round_observation(calibration, "current", c)
    return calibration


def verify_round_observation(performance, variant, row):
    observation = row.get("postgresSettingsObservation")
    if not isinstance(observation, dict) or set(observation) != {"before", "after"}:
        raise ValueError("before/after PostgreSQL observations are required for every round")
    if row.get("database") != "function3_baseline_test":
        raise ValueError("calibration and tuning must use the same resettable baseline database")
    expected = performance["postgresSettings"][variant]
    replay = {"database": row["database"]}
    record_round_settings({"postgresSettings": {variant: expected}}, variant,
                          observation["before"], observation["after"], replay)
    for field in ("postgresSettingsSha256", "postgresSettingsAfterSha256", "postgresSettingsStable"):
        if row.get(field) != replay[field] or type(row.get(field)) is not type(replay[field]):
            raise ValueError("per-round PostgreSQL observations do not match the report")


def evaluate(document):
    calibration = document["calibration"]
    performance = document["performance"]
    if performance.get("mode") != "postgres-tuning":
        raise ValueError("explicit postgres-tuning mode is required")
    noise = analyse(calibration)
    if noise.get("level") != "DESCRIPTIVE_ONLY":
        raise ValueError("complete quiet same-binary A/A calibration is required first")
    calibration = validate_tuning_context(calibration, performance["baselineBinarySha256"],
                                          performance["configuration"], performance["workload"])
    pairs = paired_rounds(performance)
    if len(pairs) < 3 or type(performance.get("roundsRequested")) is not int or performance["roundsRequested"] != len(pairs):
        raise ValueError("all requested tuning rounds must be complete, with at least three paired rounds")
    binary = calibration["baselineBinarySha256"]
    if (performance.get("baselineBinarySha256") != binary or performance.get("currentBinarySha256") != binary):
        raise ValueError("PostgreSQL-only comparison must use the same calibrated binary")
    if performance.get("workload") != calibration["workload"] or performance.get("configuration") != calibration["configuration"]:
        raise ValueError("workload and non-PostgreSQL configuration must match calibration")
    config_hash, workload_hash = digest(performance["configuration"]), digest(performance["workload"])
    fixture = paired_rounds(calibration)[0][0]["fixtureSha256"]
    settings = performance["postgresSettings"]
    calibrated = calibration["postgresSettings"]
    if calibrated["baseline"] != calibrated["current"] or settings["baseline"] != calibrated["baseline"]:
        raise ValueError("tuning baseline settings must match the A/A calibration")
    changed = {name for name in settings["baseline"] if settings["baseline"][name] != settings["current"].get(name)}
    if not changed or changed - {"commit_delay", "commit_siblings"}:
        raise ValueError("only explicit session commit-delay tuning is supported")
    for variant in ("baseline", "current"):
        for key, maximum in (("commit_delay", 10000), ("commit_siblings", 100)):
            value = settings[variant].get(key, "")
            if not isinstance(value, str) or not value.isascii() or not value.isdecimal() or not 0 <= int(value) <= maximum:
                raise ValueError("session tuning outside bounded range")
    for variant in ("baseline", "current"):
        values = settings[variant]
        if not isinstance(values, dict) or not values:
            raise ValueError("both observed PostgreSQL settings snapshots are required")
        if any(values.get(key) != "on" for key in DURABILITY):
            raise ValueError("durability must remain on in both variants")
        if any(not isinstance(key, str) or not isinstance(value, str) for key, value in values.items()):
            raise ValueError("settings snapshots must use string keys and values")
    if settings["baseline"] == settings["current"]:
        raise ValueError("identical PostgreSQL settings are not a tuning A/B")
    for b, c in pairs:
        for variant, row in (("baseline", b), ("current", c)):
            verify_round_observation(performance, variant, row)
            if row.get("postgresSettingsSha256") != digest(settings[variant]):
                raise ValueError("per-round PostgreSQL settings identity mismatch")
            if (row.get("binarySha256") != binary or row.get("configurationSha256") != config_hash
                    or row.get("workloadSha256") != workload_hash or row.get("fixtureSha256") != fixture):
                raise ValueError("per-round workload, binary, configuration or fixture identity mismatch")
            if type(row.get("ackSamples")) is not int or row["ackSamples"] < 1000:
                raise ValueError("at least 1000 ACK samples per round are required")
    improvements = [100 * (b["ackP99Ms"] - c["ackP99Ms"]) / b["ackP99Ms"] for b, c in pairs]
    fast_regressions = [100 * (b["fastQps"] - c["fastQps"]) / b["fastQps"] for b, c in pairs]
    floor = noise["absolutePairDifferenceP95Percent"]
    passed = all(value >= 20 and value > floor for value in improvements) and all(value <= 10 for value in fast_regressions)
    return {"mode": "postgres-tuning", "passed": passed, "rounds": len(pairs),
            "pairedAckP99ImprovementPercent": improvements, "pairedFastQpsRegressionPercent": fast_regressions,
            "ackImprovementMedianPercent": statistics.median(improvements), "descriptiveNoiseP95Percent": floor,
            "durabilityPreserved": True, "outliersRemoved": 0, "settingsApplied": False,
            "historicalVerdictsChanged": False, "source": "supplied offline measurement evidence",
            "limitation": "Gate over supplied evidence only; does not attest live configuration, causality or production deployment."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New report file; existing evidence is never overwritten")
    args = parser.parse_args()
    try: report = evaluate(json.loads(args.input.read_text(encoding="utf-8")))
    except (ValueError, KeyError, TypeError) as error: parser.error(str(error))
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps(report, ensure_ascii=False, allow_nan=False))
    return 0 if report["passed"] else 2


if __name__ == "__main__": raise SystemExit(main())
