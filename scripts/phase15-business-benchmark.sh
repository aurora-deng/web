#!/usr/bin/env bash

# Phase 15 真实业务基准驱动：重建专用数据库、独占启动服务并连续运行多轮。
# 只使用项目已有的 PostgreSQL、Python 与 webserver，不下载任何依赖。
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
phase11_home="${PHASE11_HOME:-/home/pikachu}"
postgres_root="${PHASE11_POSTGRES_ROOT:-${phase11_home}/phase11-deps/postgresql/18.6}"
data_root="${PHASE11_DATA_ROOT:-${phase11_home}/phase11-data}"
build_dir="${PHASE15_BUILD_DIR:-${project_root}/build-phase11-grpc-onnx-release}"
binary="${PHASE15_SERVER_BINARY:-${build_dir}/webserver}"
database="${PHASE15_DATABASE:-phase15_benchmark}"
port="${PHASE15_PORT:-18091}"
runs="${PHASE15_RUNS:-3}"
result_root="${PHASE15_RESULT_ROOT:-${project_root}/artifacts/benchmarks/phase15}"
socket_dir="${data_root}/postgresql/run"
service_script="${project_root}/scripts/phase11/webserver-user-service.sh"
migration_dir="${project_root}/server/phase11/storage/postgres/migrations"

if [[ "${database}" != phase15_benchmark* ]]; then
    echo "Refusing to reset a database outside the phase15_benchmark namespace." >&2
    exit 2
fi
for required in "${binary}" "${postgres_root}/bin/psql" \
                "${postgres_root}/bin/createdb" "${postgres_root}/bin/dropdb" \
                "${project_root}/benchmarks/phase15_business_benchmark.py" \
                "${project_root}/benchmarks/phase15_aggregate.py"; do
    [[ -e "${required}" ]] || { echo "Missing required file: ${required}" >&2; exit 1; }
done
[[ -d "${migration_dir}" ]] || { echo "Missing migrations: ${migration_dir}" >&2; exit 1; }

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
result_dir="${result_root}/${stamp}"
mkdir -p "${result_dir}"
benchmark_pid=""
formal_was_running=0

cleanup() {
    if [[ -n "${benchmark_pid}" ]] && kill -0 "${benchmark_pid}" 2>/dev/null; then
        kill -TERM "${benchmark_pid}" 2>/dev/null || true
        wait "${benchmark_pid}" 2>/dev/null || true
    fi
    benchmark_pid=""
    if [[ "${formal_was_running}" -eq 1 ]]; then
        WEB_SERVER_PORT=18090 \
        WEB_GRPC_ADDRESS=127.0.0.1:50051 \
        WEB_PHASE11_DATABASE="host=${data_root}/postgresql/run dbname=phase11 user=pikachu connect_timeout=3" \
            "${service_script}" start || true
        formal_was_running=0
    fi
}
trap cleanup EXIT INT TERM

{
    printf 'generated_utc=%s\n' "${stamp}"
    printf 'database=%s\nport=%s\nruns=%s\n' "${database}" "${port}" "${runs}"
    printf 'binary=%s\n' "${binary}"
    printf 'outbox_poll_only=%s\n' "${WEB_PHASE11_OUTBOX_POLL_ONLY:-0}"
    sha256sum "${binary}"
    uname -a
    "${postgres_root}/bin/postgres" --version
    python3 --version
    # 虚拟机可能使用中文 locale；固定 C 输出，且精简 CPU 信息失败不能中止测试。
    LC_ALL=C lscpu | grep -E \
        '^(Architecture|CPU\(s\)|Model name|Thread|Core|Socket)' || true
} >"${result_dir}/environment.txt"

if "${service_script}" status >/dev/null 2>&1; then
    formal_was_running=1
    "${service_script}" stop
fi

# 每次活动使用全新的专用库，避免旧消息数量影响历史查询结果。
"${postgres_root}/bin/dropdb" -h "${socket_dir}" -U pikachu \
    --if-exists "${database}"
"${postgres_root}/bin/createdb" -h "${socket_dir}" -U pikachu \
    -O pikachu "${database}"
for migration in "${migration_dir}"/[0-9][0-9][0-9]_*.sql; do
    [[ -e "${migration}" ]] || continue
    "${postgres_root}/bin/psql" -h "${socket_dir}" -U pikachu \
        -d "${database}" -v ON_ERROR_STOP=1 -f "${migration}" >/dev/null
done

export LD_LIBRARY_PATH="${PHASE11_LIBRARY_PATH:-/home/pikachu/.local/lib:/home/pikachu/.local/lib64}:${LD_LIBRARY_PATH:-}"
export WEB_SERVER_PORT="${port}"
export WEB_GRPC_ADDRESS=off
export WEB_PHASE11_DATABASE="host=${socket_dir} dbname=${database} user=pikachu connect_timeout=3"
export WEB_PHASE11_TOKEN_KEY_FILE="${data_root}/secrets/phase11-token-hash.key"
export WEB_PHASE11_ONNX_MODEL_ROOT="${WEB_PHASE11_ONNX_MODEL_ROOT:-/home/pikachu/phase11-models}"
export WEB_PHASE11_ONNX_WORKERS="${WEB_PHASE11_ONNX_WORKERS:-1}"
export WEB_PHASE11_ONNX_QUEUE="${WEB_PHASE11_ONNX_QUEUE:-8}"

original_dir="${PWD}"
cd "${build_dir}"
"${binary}" >"${result_dir}/webserver.log" 2>&1 &
benchmark_pid=$!
cd "${original_dir}"
for _ in {1..100}; do
    if ! kill -0 "${benchmark_pid}" 2>/dev/null; then
        echo "Benchmark webserver exited during startup." >&2
        tail -n 60 "${result_dir}/webserver.log" >&2 || true
        exit 1
    fi
    if (echo > "/dev/tcp/127.0.0.1/${port}") 2>/dev/null; then
        break
    fi
    sleep 0.1
done
(echo > "/dev/tcp/127.0.0.1/${port}") 2>/dev/null || {
    echo "Benchmark webserver readiness timed out." >&2
    exit 1
}

reports=()
for ((run=1; run<=runs; run++)); do
    report="${result_dir}/run-${run}.json"
    python3 "${project_root}/benchmarks/phase15_business_benchmark.py" \
        --host 127.0.0.1 --port "${port}" --server-pid "${benchmark_pid}" \
        --workers "${PHASE15_WORKERS:-4}" \
        --operations "${PHASE15_OPERATIONS:-80}" \
        --group-size "${PHASE15_GROUP_SIZE:-8}" \
        --group-operations "${PHASE15_GROUP_OPERATIONS:-20}" \
        --history-messages "${PHASE15_HISTORY_MESSAGES:-50}" \
        --history-reads "${PHASE15_HISTORY_READS:-30}" \
        --connection-operations "${PHASE15_CONNECTION_OPERATIONS:-30}" \
        --output "${report}" >/dev/null
    reports+=("${report}")
done

python3 "${project_root}/benchmarks/phase15_aggregate.py" \
    "${reports[@]}" --output "${result_dir}/summary.json"
printf 'Phase 15 business benchmark artifacts: %s\n' "${result_dir}"
