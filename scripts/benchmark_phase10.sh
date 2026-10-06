#!/usr/bin/env bash
set -euo pipefail

# Phase 10 同机复现脚本：HTTP 和 gRPC 分开启动，避免两个入口争抢 CPU 后把
# “代码改进”与“资源竞争”混在一个数字里。每个场景跑 5 次，原始输出全部留档。
BUILD_DIR="${BUILD_DIR:-build-release}"
RESULT_ROOT="${RESULT_ROOT:-artifacts/benchmarks}"
HTTP_URL="${HTTP_URL:-http://127.0.0.1:8080/fast}"
HTTP_CONNECTIONS="${HTTP_CONNECTIONS:-128}"
HTTP_THREADS="${HTTP_THREADS:-4}"
RUN_SECONDS="${RUN_SECONDS:-15}"
GRPC_TARGET="${GRPC_TARGET:-127.0.0.1:50051}"
GRPC_CONCURRENCY="${GRPC_CONCURRENCY:-32}"
GRPC_TOKEN="${GRPC_TOKEN:-}"
GRPC_TLS_CA="${GRPC_TLS_CA:-}"

for tool in wrk curl python3; do
    command -v "$tool" >/dev/null || {
        echo "missing required tool: $tool" >&2
        exit 1
    }
done

WEB_BIN="${WEB_BIN:-$BUILD_DIR/webserver}"
GRPC_BIN="${GRPC_BIN:-$BUILD_DIR/webserver-grpc}"
GRPC_BENCH_BIN="${GRPC_BENCH_BIN:-$BUILD_DIR/grpc_benchmark}"
for binary in "$WEB_BIN" "$GRPC_BIN" "$GRPC_BENCH_BIN"; do
    [[ -x "$binary" ]] || {
        echo "executable not found: $binary" >&2
        exit 1
    }
done

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
result_dir="$RESULT_ROOT/phase10-$stamp"
mkdir -p "$result_dir"
child_pid=""
cleanup() {
    if [[ -n "$child_pid" ]] && kill -0 "$child_pid" 2>/dev/null; then
        kill -TERM "$child_pid" 2>/dev/null || true
        wait "$child_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

WEB_GRPC_ADDRESS=off "$WEB_BIN" >"$result_dir/webserver.log" 2>&1 &
child_pid=$!
for _ in {1..100}; do
    curl -fsS "$HTTP_URL" >/dev/null 2>&1 && break
    sleep 0.05
done
curl -fsS "$HTTP_URL" >/dev/null
for run in {1..5}; do
    wrk -t"$HTTP_THREADS" -c"$HTTP_CONNECTIONS" -d"${RUN_SECONDS}s" \
        --latency "$HTTP_URL" | tee "$result_dir/http-$run.txt"
done
cleanup
child_pid=""

WEB_GRPC_ADDRESS="$GRPC_TARGET" "$GRPC_BIN" >"$result_dir/grpc-server.log" 2>&1 &
child_pid=$!
sleep 1
if ! kill -0 "$child_pid" 2>/dev/null; then
    echo "gRPC server exited before the benchmark started" >&2
    cat "$result_dir/grpc-server.log" >&2
    exit 1
fi
token_args=()
if [[ -n "$GRPC_TOKEN" ]]; then
    token_args=(--token "$GRPC_TOKEN")
fi
tls_args=()
if [[ -n "$GRPC_TLS_CA" ]]; then
    tls_args=(--tls-ca "$GRPC_TLS_CA")
fi
grpc_run_failed=0
for mode in echo count upload chat; do
    for run in {1..5}; do
        # grpc_benchmark 在存在 RPC 错误时返回 2。这里仍保留 JSON 并继续其余
        # 轮次，最后统一失败；否则 set -e/pipefail 会让最需要诊断的数据丢失。
        if ! "$GRPC_BENCH_BIN" \
                --target "$GRPC_TARGET" \
                --mode "$mode" \
                --concurrency "$GRPC_CONCURRENCY" \
                --warmup 2 \
                --duration "$RUN_SECONDS" \
                "${token_args[@]}" \
                "${tls_args[@]}" | tee "$result_dir/grpc-$mode-$run.json"; then
            grpc_run_failed=1
        fi
    done
done

python3 - "$result_dir" <<'PY'
import json
import os
import pathlib
import re
import statistics
import sys

root = pathlib.Path(sys.argv[1])
http = []
http_p99_us = []
http_errors = 0

def duration_us(text):
    match = re.fullmatch(r"([0-9.]+)(us|ms|s)", text)
    value, unit = float(match.group(1)), match.group(2)
    return value * {"us": 1.0, "ms": 1000.0, "s": 1_000_000.0}[unit]

for path in sorted(root.glob("http-*.txt")):
    content = path.read_text()
    match = re.search(r"Requests/sec:\s+([0-9.]+)", content)
    if match:
        http.append(float(match.group(1)))
    match = re.search(r"^\s*99%\s+([0-9.]+(?:us|ms|s))", content, re.MULTILINE)
    if match:
        http_p99_us.append(duration_us(match.group(1)))
    match = re.search(r"Non-2xx or 3xx responses:\s+(\d+)", content)
    if match:
        http_errors += int(match.group(1))
    # wrk reports transport failures separately from HTTP status failures. A
    # timeout/read/write/connect error is still a failed request and must not be
    # hidden by a zero Non-2xx count.
    match = re.search(
        r"Socket errors:\s+connect\s+(\d+),\s+read\s+(\d+),\s+write\s+(\d+),\s+timeout\s+(\d+)",
        content,
    )
    if match:
        http_errors += sum(int(value) for value in match.groups())
grpc = {}
grpc_p99 = {}
grpc_errors = 0
for path in sorted(root.glob("grpc-*.json")):
    record = json.loads(path.read_text())
    grpc.setdefault(record["mode"], []).append(record["qps"])
    grpc_p99.setdefault(record["mode"], []).append(record["p99_us"])
    grpc_errors += record["errors"]
summary = {
    "http_profile": {
        "url": os.environ.get("HTTP_URL", "http://127.0.0.1:8080/fast"),
        "reactors": os.environ.get("WEB_SERVER_REACTORS", "auto"),
        "threads": int(os.environ.get("HTTP_THREADS", "4")),
        "connections": int(os.environ.get("HTTP_CONNECTIONS", "128")),
        "duration_seconds": int(os.environ.get("RUN_SECONDS", "15")),
        "runs": 5,
    },
    "http_qps_runs": http,
    "http_qps_median": statistics.median(http) if http else None,
    "http_p99_us_runs": http_p99_us,
    "http_p99_us_median": statistics.median(http_p99_us) if http_p99_us else None,
    "http_errors": http_errors,
    "grpc_qps_runs": grpc,
    "grpc_qps_median": {key: statistics.median(value) for key, value in grpc.items()},
    "grpc_p99_us_median": {key: statistics.median(value) for key, value in grpc_p99.items()},
    "grpc_errors": grpc_errors,
    "grpc_profile": {
        "target": os.environ.get("GRPC_TARGET", "127.0.0.1:50051"),
        "concurrency": int(os.environ.get("GRPC_CONCURRENCY", "32")),
        "duration_seconds": int(os.environ.get("RUN_SECONDS", "15")),
        "warmup_seconds": 2,
        "runs_per_mode": 5,
    },
}
(root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, indent=2))
PY

echo "Phase 10 benchmark artifacts: $result_dir"
if [[ "$grpc_run_failed" -ne 0 ]]; then
    echo "one or more gRPC benchmark rounds reported errors" >&2
    exit 2
fi
