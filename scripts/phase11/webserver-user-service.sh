#!/usr/bin/env bash

# Phase 11 用户态启停脚本。
# 它只组合已经安装好的 PostgreSQL、libsodium、ONNX Runtime GenAI 和模型目录，
# 不下载依赖、不修改系统服务，也不把数据库口令或 Token 密钥写进仓库。
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_root="${PHASE11_PROJECT_ROOT:-$(cd -- "${script_dir}/../.." && pwd)}"
build_dir="${PHASE11_BUILD_DIR:-${project_root}/build-phase11-grpc-onnx-release}"
binary="${PHASE11_SERVER_BINARY:-${build_dir}/webserver}"
data_root="${PHASE11_DATA_ROOT:-/home/pikachu/phase11-data}"
model_root="${WEB_PHASE11_ONNX_MODEL_ROOT:-/home/pikachu/phase11-models}"
pid_file="${data_root}/webserver.pid"
start_ticks_file="${pid_file}.start_ticks"
log_file="${data_root}/webserver.log"
operations_log="${data_root}/operations.jsonl"
max_log_bytes="${WEB_PHASE11_MAX_LOG_BYTES:-104857600}"
min_free_kib="${WEB_PHASE11_MIN_FREE_KIB:-524288}"

audit_event() {
    local event="$1" pid="${2:-0}"
    printf '{"time":"%s","component":"phase11-webserver-service","event":"%s","pid":%s}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${event}" "${pid}" >> "${operations_log}"
}

rotate_stopped_log() {
    [[ -f "${log_file}" ]] || return 0
    local size stamp rotated
    size="$(wc -c < "${log_file}")"
    [[ "${size}" -ge "${max_log_bytes}" ]] || return 0
    stamp="$(date -u +%Y%m%dT%H%M%SZ)"
    rotated="${log_file}.${stamp}"
    mv -- "${log_file}" "${rotated}"
    if command -v gzip >/dev/null 2>&1; then
        gzip -- "${rotated}"
        rotated="${rotated}.gz"
    fi
    printf 'Rotated stopped webserver log to %s.\n' "${rotated}"
}

require_disk_budget() {
    local free_kib
    free_kib="$(df -Pk "${data_root}" | awk 'NR==2 {print $4}')"
    [[ "${free_kib}" =~ ^[0-9]+$ ]] || {
        printf 'Cannot determine free disk space below %s.\n' "${data_root}" >&2
        return 1
    }
    if [[ "${free_kib}" -lt "${min_free_kib}" ]]; then
        printf 'Refusing startup: only %s KiB free below %s (minimum %s KiB).\n' \
            "${free_kib}" "${data_root}" "${min_free_kib}" >&2
        return 1
    fi
}

running_pid() {
    [[ -r "${pid_file}" ]] || return 1
    local pid
    read -r pid < "${pid_file}"
    [[ "${pid}" =~ ^[0-9]+$ ]] && kill -0 "${pid}" 2>/dev/null || return 1
    # PID 会被 Linux 复用。新启动的服务额外保存 /proc start ticks；若数字相同但
    # start ticks 不同，就不能把后来出现的无关进程当成旧 webserver。
    if [[ -r "${start_ticks_file}" ]]; then
        local expected_ticks actual_ticks process_state
        read -r expected_ticks < "${start_ticks_file}"
        process_state="$(awk '{print $3}' "/proc/${pid}/stat" 2>/dev/null || true)"
        actual_ticks="$(awk '{print $22}' "/proc/${pid}/stat" 2>/dev/null || true)"
        [[ "${expected_ticks}" =~ ^[0-9]+$ &&
           "${process_state}" != "Z" && "${process_state}" != "X" &&
           "${actual_ticks}" == "${expected_ticks}" ]] || return 1
    fi
    printf '%s\n' "${pid}"
}

start_server() {
    local pid
    if pid="$(running_pid)"; then
        printf 'Phase 11 webserver is already running (pid=%s).\n' "${pid}"
        return 0
    fi

    [[ -x "${binary}" ]] || {
        printf 'webserver binary is missing: %s\n' "${binary}" >&2
        return 1
    }
    [[ -r "${WEB_PHASE11_TOKEN_KEY_FILE:-${data_root}/secrets/phase11-token-hash.key}" ]] || {
        printf 'Phase 11 token key is missing or unreadable.\n' >&2
        return 1
    }
    [[ -d "${model_root}" ]] || {
        printf 'ONNX model root is missing: %s\n' "${model_root}" >&2
        return 1
    }

    mkdir -p "${data_root}"
    require_disk_budget
    rotate_stopped_log
    export LD_LIBRARY_PATH="${PHASE11_LIBRARY_PATH:-/home/pikachu/.local/lib:/home/pikachu/.local/lib64}:${LD_LIBRARY_PATH:-}"
    export WEB_SERVER_PORT="${WEB_SERVER_PORT:-18090}"
    export WEB_GRPC_ADDRESS="${WEB_GRPC_ADDRESS:-127.0.0.1:50051}"
    export WEB_PHASE11_DATABASE="${WEB_PHASE11_DATABASE:-host=${data_root}/postgresql/run dbname=phase11 user=pikachu connect_timeout=3}"
    export WEB_PHASE11_TOKEN_KEY_FILE="${WEB_PHASE11_TOKEN_KEY_FILE:-${data_root}/secrets/phase11-token-hash.key}"
    # 文件正文与数据库元数据分开保存；服务脚本显式放入数据目录，避免二进制
    # 因为从不同工作目录启动而把附件散落到 build 或源码目录。
    export WEB_PHASE11_FILE_ROOT="${WEB_PHASE11_FILE_ROOT:-${data_root}/files}"
    export WEB_PHASE11_ONNX_MODEL_ROOT="${model_root}"
    export WEB_PHASE11_ONNX_WORKERS="${WEB_PHASE11_ONNX_WORKERS:-1}"
    export WEB_PHASE11_ONNX_QUEUE="${WEB_PHASE11_ONNX_QUEUE:-8}"
    # AI 生成共有三层保险：Provider 自身超时、统一 Provider 看门线程、业务总期限。
    # 业务期限略长，先让 Provider 完成熔断/备用切换并返回准确故障原因。
    export WEB_PHASE11_AI_PROVIDER_TIMEOUT_MS="${WEB_PHASE11_AI_PROVIDER_TIMEOUT_MS:-300000}"
    export WEB_PHASE11_AI_DEADLINE_MS="${WEB_PHASE11_AI_DEADLINE_MS:-330000}"
    export WEB_PHASE11_AI_MAX_ACTIVE="${WEB_PHASE11_AI_MAX_ACTIVE:-64}"
    export WEB_PHASE11_AI_MAX_PER_USER="${WEB_PHASE11_AI_MAX_PER_USER:-2}"
    export WEB_PHASE11_AI_MAX_PER_ACCOUNT="${WEB_PHASE11_AI_MAX_PER_ACCOUNT:-16}"
    export WEB_PHASE11_AI_CIRCUIT_FAILURES="${WEB_PHASE11_AI_CIRCUIT_FAILURES:-3}"
    export WEB_PHASE11_AI_CIRCUIT_OPEN_MS="${WEB_PHASE11_AI_CIRCUIT_OPEN_MS:-30000}"

    cd "${build_dir}"
    nohup "${binary}" > "${log_file}" 2>&1 &
    pid=$!
    printf '%s\n' "${pid}" > "${pid_file}"
    awk '{print $22}' "/proc/${pid}/stat" > "${start_ticks_file}"

    for _ in {1..50}; do
        if ! kill -0 "${pid}" 2>/dev/null; then
            printf 'Phase 11 webserver exited during startup.\n' >&2
            tail -n 30 "${log_file}" >&2 || true
            rm -f "${pid_file}" "${start_ticks_file}"
            return 1
        fi
        if curl --fail --silent --show-error --max-time 1 \
            "http://127.0.0.1:${WEB_SERVER_PORT}/health/ready" >/dev/null 2>&1; then
            printf 'Phase 11 webserver started (pid=%s, port=%s, ONNX enabled).\n' \
                "${pid}" "${WEB_SERVER_PORT}"
            audit_event "started" "${pid}"
            return 0
        fi
        sleep 0.1
    done

    printf 'Phase 11 webserver is running but readiness timed out; inspect %s.\n' \
        "${log_file}" >&2
    return 1
}

stop_server() {
    local pid
    if ! pid="$(running_pid)"; then
        rm -f "${pid_file}" "${start_ticks_file}"
        printf 'Phase 11 webserver is not running.\n'
        return 0
    fi
    kill -TERM "${pid}"
    for _ in {1..100}; do
        if ! kill -0 "${pid}" 2>/dev/null; then
            rm -f "${pid_file}" "${start_ticks_file}"
            printf 'Phase 11 webserver stopped.\n'
            audit_event "stopped" "${pid}"
            return 0
        fi
        sleep 0.1
    done
    printf 'Phase 11 webserver did not stop within 10 seconds (pid=%s).\n' "${pid}" >&2
    return 1
}

status_server() {
    local pid
    if pid="$(running_pid)"; then
        printf 'Phase 11 webserver is running (pid=%s, http=%s, modelRoot=%s).\n' \
            "${pid}" "${WEB_SERVER_PORT:-18090}" "${model_root}"
    else
        printf 'Phase 11 webserver is not running.\n'
        return 1
    fi
}

rotate_log() {
    local was_running=0
    if running_pid >/dev/null; then
        was_running=1
        stop_server
    fi
    # Explicit rotate always archives a non-empty stopped log, regardless of
    # the automatic size threshold.
    if [[ -s "${log_file}" ]]; then
        local stamp rotated
        stamp="$(date -u +%Y%m%dT%H%M%SZ)"
        rotated="${log_file}.${stamp}"
        mv -- "${log_file}" "${rotated}"
        if command -v gzip >/dev/null 2>&1; then
            gzip -- "${rotated}"
            rotated="${rotated}.gz"
        fi
        printf 'Rotated webserver log to %s.\n' "${rotated}"
        audit_event "log_rotated" 0
    else
        printf 'No non-empty webserver log to rotate.\n'
    fi
    if [[ "${was_running}" -eq 1 ]]; then
        start_server
    fi
}

case "${1:-status}" in
    start) start_server ;;
    stop) stop_server ;;
    restart) stop_server; start_server ;;
    status) status_server ;;
    logs) tail -f "${log_file}" ;;
    rotate) rotate_log ;;
    *)
        printf 'Usage: %s {start|stop|restart|status|logs|rotate}\n' "$0" >&2
        exit 2
        ;;
esac
