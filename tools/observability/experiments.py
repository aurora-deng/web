"""Portable A/A calibration and paired monitoring overhead experiments.

Every leg owns a new database and NEBULA process. The existing comparison
tool still decides whether its historical acceptance criteria were met.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import secrets
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "scripts/project"), str(ROOT / "scripts/function3/validation")]
from repro_runtime import clean_environment, disposable_database, get_json, owned_server_process, require
from measurement_noise import acceptance_result, analyse, digest
from function32_lab import benchmark
from .observer.store import Store, encode


def instant():
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def minimum_fixture(prefix, connection, highest):
    # Verify the equivalent reset fixture instead of hashing its random socket path.
    sql = "SELECT (SELECT count(*) FROM users),(SELECT count(*) FROM conversations),(SELECT count(*) FROM messages),(SELECT max(version) FROM phase11_schema_migrations)"
    fields = subprocess.check_output([str(prefix / "bin/psql"), "-XqAt", "-F", "|", "-d", connection,
                                      "-v", "ON_ERROR_STOP=1", "-c", sql], text=True, env=clean_environment()).strip()
    require(fields == f"0|0|0|{highest}", "Reset fixture differs from the empty migrated source")
    migrations = ROOT / "backend/application/storage/postgres/migrations"
    return digest({"observed":fields,"migrations":[hashlib.sha256(x.read_bytes()).hexdigest()
                    for x in sorted(migrations.glob("[0-9][0-9][0-9]_*.sql"))]})


def fast(port, wrk):
    command = [wrk, "-t2", "-c16", "-d3s", f"http://127.0.0.1:{port}/fast"]
    output = subprocess.check_output(command, env=clean_environment(), text=True, timeout=15)
    match = re.search(r"Requests/sec:\s+([\d.]+)", output)
    require(match is not None and ("Socket errors:" not in output or
            re.search(r"connect 0, read 0, write 0, timeout 0", output)), "wrk did not finish cleanly")
    require("Non-2xx" not in output, "wrk saw rejected requests")
    return float(match[1])


def leg(binary, root, prefix, workers, operations, enabled, wrk, paired):
    root.mkdir()
    with disposable_database(prefix, ROOT, root) as (connection, migration):
        key = root / "token.key"
        key.touch(mode=0o600, exist_ok=False)
        key.write_text(secrets.token_hex(32), encoding="ascii")
        fixture = minimum_fixture(prefix, connection, migration)
        env = {**clean_environment(), "WEB_PHASE11_REQUIRED":"1", "WEB_FUNCTION3_ENABLE":"1",
               "WEB_PHASE11_DATABASE":connection, "WEB_PHASE11_TOKEN_KEY_FILE":str(key),
               "WEB_PHASE11_FILE_ROOT":str(root/"files"), "WEB_FUNCTION3_MEDIA_ROOT":str(root/"media"),
               "WEB_OBSERVABILITY":str(int(enabled))}
        with owned_server_process(binary, ROOT, env, root / "identity.log") as (port, _):
            stamp = get_json(port, "/health/version")
            qps = fast(port, wrk) if paired else None
        data = benchmark(binary, env, root, workers, operations, "mixed")
        return qps, data, fixture, stamp


def scaled(report):
    return {
        "workers":report["configuration"]["workers"], "operations":report["mixed"]["operations"],
        "ackSamples":report["durableChatAckSamples"], "errors":report["mixed"]["errors"],
        "ackP50Ms":report["durableChatAckMs"]["p50"], "ackP95Ms":report["durableChatAckMs"]["p95"],
        "ackP99Ms":report["durableChatAckMs"]["p99"], "operationsPerSecond":report["mixed"]["operationsPerSecond"],
        "rssKiB":report["resources"]["rssKiB"]["peak"], "fileDescriptors":report["resources"]["fileDescriptors"]["peak"],
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("mode", choices=("calibrate","overhead","capacity"))
    for name in ("binary","postgres-prefix","output"):
        p.add_argument("--"+name, type=Path, required=True)
    p.add_argument("--rounds", type=int)
    p.add_argument("--operations", type=int, default=4000)
    args = p.parse_args()
    require(sys.platform.startswith("linux"), "Linux only")
    require(4000 <= args.operations <= 9900, "Use 4000–9900 operations per leg")
    rounds = args.rounds if args.rounds is not None else (10 if args.mode=="calibrate" else 3)
    require((10 if args.mode=="calibrate" else 1) <= rounds <= 24, "Invalid round count")
    wrk = shutil.which("wrk")
    require(wrk or args.mode == "capacity", "wrk must already be installed")
    binary, output, prefix = args.binary.resolve(), args.output.resolve(), args.postgres_prefix.resolve()
    output.mkdir(parents=True, exist_ok=False)
    hash_binary = hashlib.sha256(binary.read_bytes()).hexdigest()
    environment = {"platform":platform.platform(), "cpu":os.cpu_count(),
                   "architecture":platform.machine(),
                   "pg":subprocess.check_output([str(prefix/"bin/postgres"),"--version"],text=True).strip()}
    config = {"httpWorkers":2,"wsWorkers":2,"reactors":2,"storage":"temporary-postgres","delivery":"local"}
    rows = {"baseline":[],"current":[]}
    public = Store(output / "experiments.sqlite")
    targets = (1,4,16) if args.mode=="capacity" else (4,)
    for workers in targets:
        workload = {"mixedWorkers":workers,"operations":args.operations,"fastWorkers":2,"fastClients":16,"fastSeconds":3,
                    "toolSha256":hashlib.sha256((ROOT/"benchmarks/function3_mixed.py").read_bytes()).hexdigest()}
        for round_no in range(1,rounds+1):
            variants = ["baseline","current"] if args.mode!="capacity" else ["current"]
            if round_no % 2 == 0:
                variants.reverse()
            for variant in variants:
                start = instant()
                qps, report, fixture, stamp = leg(binary, output/f"w{workers}-{round_no}-{variant}", prefix, workers,
                                           args.operations, args.mode!="calibrate" and variant=="current", wrk,
                                           args.mode!="capacity")
                row = scaled(report)
                if qps is not None:
                    row["fastQps"] = qps
                commit = stamp["revision"]
                identity = {"commit":commit if re.fullmatch("[0-9a-f]{40}",commit) else None,
                            "sourceFingerprint":stamp.get("sourceFingerprint"),
                            "binarySha256":hash_binary,"configSha256":digest(config),
                            "workloadSha256":digest(workload),"fixtureSha256":fixture,
                            "environmentSha256":digest(environment)}
                record = {"schemaVersion":1,"scenario":"mixed","startedAt":start,"finishedAt":instant(),
                          "identity":identity,"result":row}
                path = output/f"w{workers}-{round_no}-{variant}.json"
                path.write_text(encode(record)+"\n",encoding="utf-8")
                public.import_run(record)
                if args.mode!="capacity":
                    rows[variant].append({"round":round_no, **identity, "configurationSha256":identity["configSha256"], **row})
                print(encode({"workers":workers,"round":round_no,"variant":variant,**row}), flush=True)
    if args.mode!="capacity":
        summary = {**rows,"configuration":config,"workload":workload,"roundsRequested":rounds,
                   "baselineBinarySha256":hash_binary,"currentBinarySha256":hash_binary,
                   "calibrationCompleted":args.mode=="calibrate","gateEvaluated":args.mode=="overhead",
                   "mode":"calibration" if args.mode=="calibrate" else "acceptance"}
        if args.mode == "calibrate":
            summary["calibration"] = analyse(summary, 20)
        else:
            summary["gateEvaluated"], summary["passed"] = acceptance_result(summary)
        (output/"pairs.json").write_text(encode(summary)+"\n", encoding="utf-8")
        print(encode({k:summary.get(k) for k in ("mode","gateEvaluated","passed","calibration")}))


if __name__ == "__main__":
    main()
