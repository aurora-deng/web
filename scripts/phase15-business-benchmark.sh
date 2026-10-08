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
supervisor_script="${project_root}/scripts/phase11/webserver-user-supervisor.sh"
migration_dir="${project_root}/server/phase11/storage/postgres/migrations"

if [[ "${database}" != phase15_benchmark* ]]; then
    echo "Refusing to reset a database outside the phase15_benchmark namespace." >&2
    exit 2
fi
for required in "${binary}" "${postgres_root}/bin/psql" \
                "${postgres_root}/bin/createdb" "${postgres_root}/bin/dropdb" \
                "${project_root}/benchmarks/phase15_business_benchmark.py" \
                "${project_root}/benchmarks/phase15_aggregate.py" \
                "${service_script}" "${supervisor_script}"; do
    [[ -e "${required}" ]] || { echo "Missing required file: ${required}" >&2; exit 1; }
done
[[ -d "${migration_dir}" ]] || { echo "Missing migrations: ${migration_dir}" >&2; exit 1; }

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
result_dir="${result_root}/${stamp}"
mkdir -p "${result_dir}"
benchmark_pid=""
formal_was_running=0
supervisor_was_running=0

cleanup() {
    if [[ -n "${benchmark_pid}" ]] && kill -0 "${benchmark_pid}" 2>/dev/null; then
        kill -TERM "${benchmark_pid}" 2>/dev/null || true
        wait "${benchmark_pid}" 2>/dev/null || true
    fi
    benchmark_pid=""
    if [[ "${supervisor_was_running}" -eq 1 ]]; then
        WEB_SERVER_PORT=18090 \
        WEB_GRPC_ADDRESS=127.0.0.1:50051 \
        WEB_PHASE11_DATABASE="host=${data_root}/postgresql/run dbname=phase11 user=pikachu connect_timeout=3" \
            bash "${supervisor_script}" start || true
        supervisor_was_running=0
        formal_was_running=0
    elif [[ "${formal_was_running}" -eq 1 ]]; then
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
    printf 'websocket_connection_steps=%s\n' "${PHASE15_WEBSOCKET_CONNECTION_STEPS:-disabled}"
    if [[ -n "${PHASE15_AI_MODEL_ARTIFACT:-}" ]]; then
        printf 'ai_model_runtime=%s\n' "${PHASE15_AI_MODEL_RUNTIME:-onnx-inprocess}"
        printf 'ai_model_artifact=%s\n' "${PHASE15_AI_MODEL_ARTIFACT}"
        printf 'ai_model_checksum=%s\n' "${PHASE15_AI_MODEL_CHECKSUM:-missing}"
        printf 'ai_adapter_artifact=%s\n' "${PHASE15_AI_ADAPTER_ARTIFACT:-none}"
    else
        printf 'ai_model=disabled\n'
    fi
    sha256sum "${binary}"
    uname -a
    "${postgres_root}/bin/postgres" --version
    python3 --version
    # 虚拟机可能使用中文 locale；固定 C 输出，且精简 CPU 信息失败不能中止测试。
    LC_ALL=C lscpu | grep -E \
        '^(Architecture|CPU\(s\)|Model name|Thread|Core|Socket)' || true
} >"${result_dir}/environment.txt"

if bash "${supervisor_script}" status >/dev/null 2>&1; then
    supervisor_was_running=1
    bash "${supervisor_script}" stop
elif "${service_script}" status >/dev/null 2>&1; then
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

# 只有调用者显式给出已验证的模型目录和摘要时才创建真实 AI 账号。
# 这样普通 Phase 15 基准保持零额外依赖，Phase 26 则能复用同一驱动跑真实 AI。
ai_user_id=0
if [[ -n "${PHASE15_AI_MODEL_ARTIFACT:-}" ||
      -n "${PHASE15_AI_MODEL_CHECKSUM:-}" ]]; then
    [[ -n "${PHASE15_AI_MODEL_ARTIFACT:-}" &&
       -n "${PHASE15_AI_MODEL_CHECKSUM:-}" ]] || {
        echo "Both PHASE15_AI_MODEL_ARTIFACT and PHASE15_AI_MODEL_CHECKSUM are required." >&2
        exit 2
    }
    ai_seed="${project_root}/tests/phase11/integration/seed_onnx.sql"
    [[ -f "${ai_seed}" ]] || { echo "Missing AI seed: ${ai_seed}" >&2; exit 1; }
    ai_user_id="$("${postgres_root}/bin/psql" -qAt \
        -h "${socket_dir}" -U pikachu -d "${database}" \
        -v ON_ERROR_STOP=1 \
        -v "test_suffix=${stamp}" \
        -v "model_runtime=${PHASE15_AI_MODEL_RUNTIME:-onnx-inprocess}" \
        -v "model_artifact=${PHASE15_AI_MODEL_ARTIFACT}" \
        -v "model_checksum=${PHASE15_AI_MODEL_CHECKSUM}" \
        -v "adapter_artifact=${PHASE15_AI_ADAPTER_ARTIFACT:-}" \
        -f "${ai_seed}" | tail -n 1)"
    [[ "${ai_user_id}" =~ ^[0-9]+$ ]] || {
        echo "AI seed did not return a numeric user id: ${ai_user_id}" >&2
        exit 1
    }
fi

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
    optional_args=(
        --websocket-connection-steps "${PHASE15_WEBSOCKET_CONNECTION_STEPS:-}"
        --websocket-open-workers "${PHASE15_WEBSOCKET_OPEN_WORKERS:-16}"
        --websocket-connections-per-user "${PHASE15_WEBSOCKET_CONNECTIONS_PER_USER:-4}"
        --websocket-settle-seconds "${PHASE15_WEBSOCKET_SETTLE_SECONDS:-1}"
    )
    if [[ "${ai_user_id}" -gt 0 ]]; then
        optional_args+=(
            --ai-user-id "${ai_user_id}"
            --ai-jobs "${PHASE15_AI_JOBS:-4}"
            --ai-concurrency "${PHASE15_AI_CONCURRENCY:-2}"
            --ai-mixed-jobs "${PHASE15_AI_MIXED_JOBS:-4}"
            --ai-mixed-concurrency "${PHASE15_AI_MIXED_CONCURRENCY:-2}"
            --ai-mixed-chat-operations "${PHASE15_AI_MIXED_CHAT_OPERATIONS:-40}"
            --ai-prompt "${PHASE15_AI_PROMPT:-用三句话解释 Reactor 模式。}"
        )
    fi
    python3 "${project_root}/benchmarks/phase15_business_benchmark.py" \
        --host 127.0.0.1 --port "${port}" --server-pid "${benchmark_pid}" \
        --workers "${PHASE15_WORKERS:-4}" \
        --operations "${PHASE15_OPERATIONS:-80}" \
        --group-size "${PHASE15_GROUP_SIZE:-8}" \
        --group-operations "${PHASE15_GROUP_OPERATIONS:-20}" \
        --history-messages "${PHASE15_HISTORY_MESSAGES:-50}" \
        --history-reads "${PHASE15_HISTORY_READS:-30}" \
        --connection-operations "${PHASE15_CONNECTION_OPERATIONS:-30}" \
        "${optional_args[@]}" \
        --output "${report}" >/dev/null
    reports+=("${report}")
done

python3 "${project_root}/benchmarks/phase15_aggregate.py" \
    "${reports[@]}" --output "${result_dir}/summary.json"
printf 'Phase 15 business benchmark artifacts: %s\n' "${result_dir}"
