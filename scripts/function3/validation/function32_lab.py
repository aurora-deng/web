"""Fault drills, ACK profiling and COMMIT observation against a disposable Linux runtime."""
import argparse
from contextlib import nullcontext
import hashlib
import json
from pathlib import Path
import secrets
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT / "scripts/project"), str(ROOT / "benchmarks")]
from phase15_business_benchmark import BusinessBenchmark, WebSocketClient, percentile
from postgres_commit_observer import CommitObserver, summarize_observation
from repro_runtime import clean_environment, disposable_database, get_json, owned_server_process, require


def sql(prefix, connection, statement):
    return subprocess.check_output(
        [str(prefix / "bin/psql"), "-X", "-w", "-d", connection,
         "-v", "ON_ERROR_STOP=1", "-qAtc", statement],
        env=clean_environment(), timeout=12, text=True).strip()


def send(ws, conversation, key):
    ws.send_json({"v": 1, "type": "chat.send", "id": key,
                  "conversationId": str(conversation), "content": "Isolated fault drill"})
    for _ in range(100):
        reply = ws.read_json()
        if reply.get("replyTo") == key and reply.get("type") in ("chat.accepted", "chat.error"):
            return reply
    raise RuntimeError("No correlated send reply")


def faults(binary, env, out, prefix, connection):
    key = "fault-" + secrets.token_hex(12)
    seed_key = key + "-seed"
    faults_result = {}
    with owned_server_process(binary, ROOT, env, out / "fault-before.log",
                              expected_exit=-signal.SIGKILL) as (port, child):
        bench = BusinessBenchmark(SimpleNamespace(host="127.0.0.1", port=port, timeout=3))
        pair = bench.create_pair("fault")
        ws = WebSocketClient(pair.sender.api)
        try:
            ws.connect()
            require(send(ws, pair.conversation, seed_key).get("status") == "inserted",
                    "Warm send did not commit")
            started = time.monotonic()
            killed = int(sql(prefix, connection,
                "SELECT count(*) FROM (SELECT pg_terminate_backend(pid) AS ok "
                "FROM pg_stat_activity WHERE datname=current_database() "
                "AND pid<>pg_backend_pid()) AS terminated WHERE ok"))
            require(killed > 0, "No disposable PostgreSQL sessions terminated")
            require(get_json(port, "/health/live") == {"status": "alive"},
                    "App stopped after DB connection termination")
            failures = 0
            for _ in range(10):
                response = send(ws, pair.conversation, key)
                if response["type"] == "chat.accepted":
                    require(response["status"] == "inserted", "Unexpected first send result")
                    message_id = str(response["id"])
                    require(message_id.isdecimal(), "Invalid persisted message ID")
                    break
                failures += 1
                time.sleep(.05)
            else:
                raise RuntimeError("Pool did not recover within the retry budget")
            faults_result["postgresConnectionTermination"] = {
                "terminatedSessions": killed, "errorRepliesBeforeAck": failures,
                "liveAfterTermination": True, "msToSuccessfulAck": ms_since(started)}
            # This is after a known durable ACK. It does not simulate a crash during COMMIT.
            started = time.monotonic()
            child.kill()
            require(child.wait(timeout=10) == -signal.SIGKILL, "Owned process was not killed")
        finally:
            ws.close()
            for user in bench.users:
                user.api.close()

    with owned_server_process(binary, ROOT, env, out / "fault-restart.log") as (port, _):
        pair.sender.api.port = port
        ws = WebSocketClient(pair.sender.api)
        try:
            ws.connect()
            repeated = send(ws, pair.conversation, key)
            require(repeated.get("status") == "duplicate" and str(repeated.get("id")) == message_id,
                    "Same key did not return the same persisted message after restart")
            status, history = pair.sender.api.request(
                "GET", f"/api/conversations/{pair.conversation}/messages?after=0&limit=20")
            require(status == 200 and sum(str(item["id"]) == message_id for item in history) == 1,
                    "History did not contain the exact persisted message once")
            rows = sql(prefix, connection, f"SELECT count(*) FROM messages WHERE client_message_id='{key}'")
            require(rows == "1", "Database contains more or fewer than one matching row")
            faults_result["crashAfterDurableAck"] = {
                "sameMessageId": True, "duplicateReply": True, "matchingRows": int(rows),
                "historyRecovered": True, "msFromKillToVerifiedHistory": ms_since(started)}
        finally:
            ws.close()
            pair.sender.api.close()
    return faults_result


def ms_since(start):
    return round(1000 * (time.monotonic() - start), 3)


def quantiles(values):
    require(bool(values), "No trace samples")
    return {f"p{int(q * 100)}": round(percentile(values, q), 3) for q in (.5, .95, .99)}


def inject_commit_delay(prefix, connection, delay_ms):
    # The deferred trigger affects only the benchmark's first measured send.
    # The caller owns this database and discards the entire cluster afterwards.
    sql(prefix, connection, f"""
        CREATE FUNCTION nebula_repro_commit_delay() RETURNS trigger LANGUAGE plpgsql AS $body$
        BEGIN
            PERFORM pg_catalog.pg_sleep({delay_ms} / 1000.0);
            RETURN NEW;
        END;
        $body$;
        CREATE CONSTRAINT TRIGGER nebula_repro_commit_delay
        AFTER INSERT ON messages DEFERRABLE INITIALLY DEFERRED
        FOR EACH ROW WHEN (NEW.client_message_id LIKE 'f3mixed-%-7')
        EXECUTE FUNCTION nebula_repro_commit_delay();
    """)


def benchmark(binary, env, out, workers, operations, name, traced=False, observation=None):
    output = out / (name + ".json")
    log = out / (name + "-server.log")
    trace_budget = (operations * 3 + 9) // 10 + workers
    trace_env = {**env, **({"WEB_OBSERVABILITY": "1"} if traced else {})}
    with (observation or nullcontext()), owned_server_process(binary, ROOT, {**trace_env, "WEB_CHAT_ACK_TRACE_LIMIT":
                              str(trace_budget if traced else 0)}, log) as (port, child):
        command = [sys.executable, str(ROOT / "benchmarks/function3_mixed.py"), "--port", str(port),
                   "--server-pid", str(child.pid), "--operations", str(operations),
                   "--workers", str(workers), "--output", str(output)]
        if traced:
            command.append("--capture-acks")
        with (out / (name + "-driver.log")).open("wb") as runlog:
            subprocess.run(command, check=True, timeout=240, env=clean_environment(),
                           stdout=runlog, stderr=subprocess.STDOUT)
    full = json.loads(output.read_text(encoding="utf-8"))
    summary = {k: full[k] for k in ("configuration", "durableChatAckMs", "durableChatAckSamples", "resources")}
    summary["mixed"] = {k: full["mixed"][k] for k in ("operations", "operationsPerSecond", "latencyMs", "errors")}
    expected = sum(i % 10 >= 7 for i in range(operations))
    require(full["durableChatAckSamples"] == expected, "Incomplete ACK sample set")
    if traced:
        marker = "NEBULA_CHAT_ACK_TRACE "
        traces = {}
        for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
            if marker in line:
                trace = json.loads(line.split(marker, 1)[1])
                traces[str(trace["messageId"])] = trace
        paired = [(sample, traces[sample["messageId"]]) for sample in full["ackSamples"]
                  if sample["messageId"] in traces]
        require(len(paired) == expected, f"ACK/trace pairing incomplete: {len(paired)} of {expected}")
        slow = sorted(paired, key=lambda pair: pair[0]["rttMs"], reverse=True)
        tail = slow[:max(1, (len(paired) + 99) // 100)]
        summary["trace"] = {"paired": len(paired), "serverUnits": "microseconds",
            "serverQuantiles": {field: quantiles([row[field] for _, row in paired if row[field] >= 0])
                                for field in ("queueUs", "poolAcquireUs", "commitStartUs", "commitUs", "ackReadyUs")
                                if all(field in row for _, row in paired) and any(row[field] >= 0 for _, row in paired)},
            "slowestOnePercent": [{"rttMs": round(sample["rttMs"], 3), **{
                k: row[k] for k in ("poolAcquireUs", "commitStartUs", "commitUs", "ackReadyUs")}}
                                  | ({"queueUs": row["queueUs"]} if "queueUs" in row else {})
                                  for sample, row in tail]}
        if observation:
            (out / "commit-samples.json").write_text(
                json.dumps({"polls": observation.samples, "wal": observation.wal}, separators=(",", ":")) + "\n",
                encoding="utf-8")
            summary["commitObservation"] = summarize_observation(observation, paired, full["workloadMonotonicNs"])
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("faults", "profile", "commit"))
    for name in ("binary", "postgres-prefix", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--operations", type=int, default=1200)
    parser.add_argument("--workers", type=int, nargs="+")
    parser.add_argument("--commit-delay-ms", type=int, default=0,
                        help="Positive control, delaying the first measured COMMIT by 0–250 ms in its private DB")
    args = parser.parse_args()
    require(sys.platform.startswith("linux"), "Linux only")
    commit_mode = args.mode == "commit"
    args.workers = args.workers or ([16] if commit_mode else [1, 4, 16])
    require(10 <= args.operations <= 9900, "Operations must be 10–9900")
    require(len(args.workers) <= 8 and all(1 <= n <= 64 for n in args.workers),
            "Use at most eight points, each with 1–64 workers")
    require(not commit_mode or len(args.workers) == 1, "Use one worker count per COMMIT observation")
    require(0 <= args.commit_delay_ms <= 250 and (commit_mode or args.commit_delay_ms == 0),
            "The 0–250 ms delay is exclusive to the COMMIT observation")
    binary, prefix, out = (p.resolve() for p in (args.binary, args.postgres_prefix, args.output))
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    result = {"mode": args.mode, "binarySha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "existingDatabaseUsed": False, "passed": False}
    try:
        with disposable_database(prefix, ROOT, out, track_wal_io=commit_mode) as (connection, migration), tempfile.TemporaryDirectory(
                prefix="nebula-function32-") as scratch:
            key = Path(scratch) / "token.key"
            key.touch(mode=0o600, exist_ok=False)
            key.write_text(secrets.token_hex(32), encoding="ascii")
            env = {**clean_environment(), "WEB_PHASE11_REQUIRED": "1", "WEB_FUNCTION3_ENABLE": "1",
                   "WEB_PHASE11_DATABASE": connection, "WEB_PHASE11_TOKEN_KEY_FILE": str(key),
                   "WEB_PHASE11_FILE_ROOT": str(Path(scratch) / "files"),
                   "WEB_FUNCTION3_MEDIA_ROOT": str(Path(scratch) / "media")}
            result["migration"] = migration
            if args.mode == "faults":
                result["checks"] = faults(binary, env, out, prefix, connection)
            elif commit_mode:
                if args.commit_delay_ms:
                    inject_commit_delay(prefix, connection, args.commit_delay_ms)
                result["commitControlDelayMs"] = args.commit_delay_ms
                observation = CommitObserver(prefix, connection)
                result["instrumented"] = benchmark(binary, env, out, args.workers[0], args.operations,
                                                   "instrumented", traced=True, observation=observation)
                if args.commit_delay_ms:
                    longest = result["instrumented"]["commitObservation"]["longestCommits"]
                    require(any(item["commitUs"] >= args.commit_delay_ms * 1000 and
                                item["backendWaitSamples"].get("Timeout:PgSleep", 0) > 0 for item in longest),
                            "The sampler missed the positive control in its COMMIT window")
            else:
                result["curve"] = []
                for index, workers in enumerate(args.workers):
                    result["curve"].append(benchmark(binary, env, out, workers, args.operations, f"point-{index}"))
                result["instrumented"] = benchmark(binary, env, out, args.workers[-1],
                                                  args.operations, "instrumented", traced=True)
        result["passed"] = True
    finally:
        (out / "report.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
