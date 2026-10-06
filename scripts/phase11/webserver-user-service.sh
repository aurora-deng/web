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
log_file="${data_root}/webserver.log"

running_pid() {
    [[ -r "${pid_file}" ]] || return 1
    local pid
    read -r pid < "${pid_file}"
    [[ "${pid}" =~ ^[0-9]+$ ]] && kill -0 "${pid}" 2>/dev/null || return 1
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
    export LD_LIBRARY_PATH="${PHASE11_LIBRARY_PATH:-/home/pikachu/.local/lib:/home/pikachu/.local/lib64}:${LD_LIBRARY_PATH:-}"
    export WEB_SERVER_PORT="${WEB_SERVER_PORT:-18090}"
    export WEB_GRPC_ADDRESS="${WEB_GRPC_ADDRESS:-127.0.0.1:50051}"
    export WEB_PHASE11_DATABASE="${WEB_PHASE11_DATABASE:-host=${data_root}/postgresql/run dbname=phase11 user=pikachu connect_timeout=3}"
    export WEB_PHASE11_TOKEN_KEY_FILE="${WEB_PHASE11_TOKEN_KEY_FILE:-${data_root}/secrets/phase11-token-hash.key}"
    export WEB_PHASE11_ONNX_MODEL_ROOT="${model_root}"
    export WEB_PHASE11_ONNX_WORKERS="${WEB_PHASE11_ONNX_WORKERS:-1}"
    export WEB_PHASE11_ONNX_QUEUE="${WEB_PHASE11_ONNX_QUEUE:-8}"

    cd "${build_dir}"
    nohup "${binary}" > "${log_file}" 2>&1 &
    pid=$!
    printf '%s\n' "${pid}" > "${pid_file}"

    for _ in {1..50}; do
        if ! kill -0 "${pid}" 2>/dev/null; then
            printf 'Phase 11 webserver exited during startup.\n' >&2
            tail -n 30 "${log_file}" >&2 || true
            rm -f "${pid_file}"
            return 1
        fi
        if (echo > "/dev/tcp/127.0.0.1/${WEB_SERVER_PORT}") 2>/dev/null; then
            printf 'Phase 11 webserver started (pid=%s, port=%s, ONNX enabled).\n' \
                "${pid}" "${WEB_SERVER_PORT}"
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
        rm -f "${pid_file}"
        printf 'Phase 11 webserver is not running.\n'
        return 0
    fi
    kill -TERM "${pid}"
    for _ in {1..100}; do
        if ! kill -0 "${pid}" 2>/dev/null; then
            rm -f "${pid_file}"
            printf 'Phase 11 webserver stopped.\n'
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

case "${1:-status}" in
    start) start_server ;;
    stop) stop_server ;;
    restart) stop_server; start_server ;;
    status) status_server ;;
    logs) tail -f "${log_file}" ;;
    *)
        printf 'Usage: %s {start|stop|restart|status|logs}\n' "$0" >&2
        exit 2
        ;;
esac
