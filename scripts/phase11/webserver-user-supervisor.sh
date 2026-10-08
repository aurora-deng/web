#!/usr/bin/env bash

# Phase 11 用户态进程监督器。
#
# 它只组合 webserver-user-service.sh，不安装 systemd、不修改系统配置。
# 监督器关注进程身份与 /health/live；数据库故障由 /health/ready 表达，不会因为
# readiness=503 就反复重启仍然健康的 Web 进程。
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
script_path="${script_dir}/$(basename -- "${BASH_SOURCE[0]}")"
service_script="${PHASE11_SERVICE_SCRIPT:-${script_dir}/webserver-user-service.sh}"
data_root="${PHASE11_DATA_ROOT:-/home/pikachu/phase11-data}"
managed_pid_file="${data_root}/webserver.pid"
managed_ticks_file="${managed_pid_file}.start_ticks"
supervisor_pid_file="${data_root}/webserver-supervisor.pid"
supervisor_ticks_file="${supervisor_pid_file}.start_ticks"
supervisor_lock="${data_root}/webserver-supervisor.lock"
supervisor_log="${data_root}/webserver-supervisor.log"
operations_log="${data_root}/operations.jsonl"
port="${WEB_SERVER_PORT:-18090}"
health_url="${PHASE11_SUPERVISOR_HEALTH_URL:-http://127.0.0.1:${port}/health/live}"
monitor_seconds="${PHASE11_SUPERVISOR_MONITOR_SECONDS:-1}"
health_failures_limit="${PHASE11_SUPERVISOR_HEALTH_FAILURES:-3}"
restart_limit="${PHASE11_SUPERVISOR_RESTART_LIMIT:-5}"
restart_window_seconds="${PHASE11_SUPERVISOR_RESTART_WINDOW_SECONDS:-60}"
stable_seconds="${PHASE11_SUPERVISOR_STABLE_SECONDS:-60}"
initial_backoff="${PHASE11_SUPERVISOR_INITIAL_BACKOFF_SECONDS:-1}"
max_backoff="${PHASE11_SUPERVISOR_MAX_BACKOFF_SECONDS:-30}"
stopping=0
sleep_pid=0

require_positive_integer() {
    local name="$1" value="$2"
    [[ "${value}" =~ ^[1-9][0-9]*$ ]] || {
        printf '%s must be a positive integer (got %s).\n' "${name}" "${value}" >&2
        exit 2
    }
}

process_start_ticks() {
    local pid="$1"
    [[ "${pid}" =~ ^[0-9]+$ && -r "/proc/${pid}/stat" ]] || return 1
    awk '{print $22}' "/proc/${pid}/stat"
}

same_process() {
    local pid="$1" expected_ticks="$2" actual_ticks process_state
    kill -0 "${pid}" 2>/dev/null || return 1
    process_state="$(awk '{print $3}' "/proc/${pid}/stat" 2>/dev/null || true)"
    [[ "${process_state}" != "Z" && "${process_state}" != "X" ]] || return 1
    actual_ticks="$(process_start_ticks "${pid}")" || return 1
    [[ "${actual_ticks}" == "${expected_ticks}" ]]
}

read_identity() {
    local pid_file="$1" ticks_file="$2" pid ticks
    [[ -r "${pid_file}" && -r "${ticks_file}" ]] || return 1
    read -r pid < "${pid_file}"
    read -r ticks < "${ticks_file}"
    [[ "${pid}" =~ ^[1-9][0-9]*$ && "${ticks}" =~ ^[0-9]+$ ]] || return 1
    same_process "${pid}" "${ticks}" || return 1
    printf '%s %s\n' "${pid}" "${ticks}"
}

audit_event() {
    local event="$1" managed_pid="${2:-0}" detail="${3:-none}"
    # event/detail 都只来自本脚本中的固定 ASCII 常量，避免把外部输入拼进 JSON。
    printf '{"time":"%s","component":"phase11-webserver-supervisor","event":"%s","supervisor_pid":%s,"managed_pid":%s,"detail":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${event}" "$$" \
        "${managed_pid}" "${detail}" >> "${operations_log}"
}

service() {
    bash "${service_script}" "$@"
}

request_stop() {
    stopping=1
    if [[ "${sleep_pid}" =~ ^[1-9][0-9]*$ ]]; then
        kill -TERM "${sleep_pid}" 2>/dev/null || true
    fi
}

sleep_interruptible() {
    local duration="$1"
    sleep "${duration}" &
    sleep_pid=$!
    wait "${sleep_pid}" 2>/dev/null || true
    sleep_pid=0
}

record_restart() {
    local now="$1"
    restart_times+=("${now}")
    local -a kept=()
    local item
    for item in "${restart_times[@]}"; do
        if (( now - item < restart_window_seconds )); then
            kept+=("${item}")
        fi
    done
    restart_times=("${kept[@]}")
    (( ${#restart_times[@]} <= restart_limit ))
}

run_supervisor() {
    require_positive_integer PHASE11_SUPERVISOR_HEALTH_FAILURES "${health_failures_limit}"
    require_positive_integer PHASE11_SUPERVISOR_RESTART_LIMIT "${restart_limit}"
    require_positive_integer PHASE11_SUPERVISOR_RESTART_WINDOW_SECONDS "${restart_window_seconds}"
    require_positive_integer PHASE11_SUPERVISOR_STABLE_SECONDS "${stable_seconds}"
    [[ -x "${service_script}" || -r "${service_script}" ]] || {
        printf 'Service script is missing: %s\n' "${service_script}" >&2
        return 1
    }
    mkdir -p "${data_root}"
    if ! mkdir "${supervisor_lock}" 2>/dev/null; then
        if read_identity "${supervisor_pid_file}" "${supervisor_ticks_file}" >/dev/null; then
            printf 'Phase 11 supervisor is already running.\n' >&2
            return 1
        fi
        rm -rf -- "${supervisor_lock}"
        mkdir "${supervisor_lock}"
    fi

    printf '%s\n' "$$" > "${supervisor_pid_file}"
    process_start_ticks "$$" > "${supervisor_ticks_file}"
    trap request_stop INT TERM
    trap 'rm -f -- "${supervisor_pid_file}" "${supervisor_ticks_file}"; rmdir -- "${supervisor_lock}" 2>/dev/null || true' EXIT
    audit_event supervisor_started

    local -a restart_times=()
    local backoff="${initial_backoff}"
    local managed_pid=0 managed_ticks=0 started_at now health_failures stable_reset
    while (( stopping == 0 )); do
        if ! service start; then
            now="$(date +%s)"
            audit_event start_failed 0 service_start_failed
            if ! record_restart "${now}"; then
                audit_event restart_budget_exhausted 0 start_failed
                printf 'Restart budget exhausted while starting the service.\n' >&2
                return 1
            fi
            sleep_interruptible "${backoff}"
            backoff="$(awk -v current="${backoff}" -v maximum="${max_backoff}" \
                'BEGIN { candidate=current*2; if (candidate>maximum) candidate=maximum; print candidate }')"
            continue
        fi

        if ! read -r managed_pid managed_ticks < <(
            read_identity "${managed_pid_file}" "${managed_ticks_file}"); then
            now="$(date +%s)"
            audit_event identity_missing 0 service_identity_missing
            if ! record_restart "${now}"; then
                audit_event restart_budget_exhausted 0 identity_missing
                return 1
            fi
            sleep_interruptible "${backoff}"
            continue
        fi

        audit_event service_observed "${managed_pid}" healthy
        started_at="$(date +%s)"
        health_failures=0
        stable_reset=0
        while (( stopping == 0 )); do
            sleep_interruptible "${monitor_seconds}"
            (( stopping == 0 )) || break
            if ! same_process "${managed_pid}" "${managed_ticks}"; then
                audit_event crash_detected "${managed_pid}" process_gone
                break
            fi
            if [[ "${health_url}" != "-" ]]; then
                if curl --fail --silent --show-error --max-time 1 \
                    "${health_url}" >/dev/null 2>&1; then
                    health_failures=0
                else
                    health_failures=$((health_failures + 1))
                    if (( health_failures >= health_failures_limit )); then
                        audit_event liveness_failed "${managed_pid}" threshold_reached
                        service stop || true
                        break
                    fi
                fi
            fi
            now="$(date +%s)"
            if (( stable_reset == 0 && now - started_at >= stable_seconds )); then
                restart_times=()
                backoff="${initial_backoff}"
                stable_reset=1
                audit_event stable "${managed_pid}" restart_budget_reset
            fi
        done

        (( stopping == 0 )) || break
        now="$(date +%s)"
        if ! record_restart "${now}"; then
            audit_event restart_budget_exhausted "${managed_pid}" crash_loop
            printf 'Restart budget exhausted after repeated failures.\n' >&2
            return 1
        fi
        audit_event restart_scheduled "${managed_pid}" bounded_backoff
        sleep_interruptible "${backoff}"
        backoff="$(awk -v current="${backoff}" -v maximum="${max_backoff}" \
            'BEGIN { candidate=current*2; if (candidate>maximum) candidate=maximum; print candidate }')"
    done

    service stop || true
    audit_event supervisor_stopped "${managed_pid}" requested
}

start_supervisor() {
    local identity
    if identity="$(read_identity "${supervisor_pid_file}" "${supervisor_ticks_file}")"; then
        printf 'Phase 11 supervisor is already running (pid=%s).\n' "${identity%% *}"
        return 0
    fi
    mkdir -p "${data_root}"
    # 仓库在 Windows 上可能不保留 executable 位；显式交给 Bash 后，用户只需
    # `bash ... start`，后台自启动不会因为 0644 文件模式失败。
    nohup bash "${script_path}" run >> "${supervisor_log}" 2>&1 &
    local launcher=$!
    for _ in {1..50}; do
        if identity="$(read_identity "${supervisor_pid_file}" "${supervisor_ticks_file}")"; then
            printf 'Phase 11 supervisor started (pid=%s).\n' "${identity%% *}"
            return 0
        fi
        if ! kill -0 "${launcher}" 2>/dev/null; then
            printf 'Phase 11 supervisor exited during startup; inspect %s.\n' \
                "${supervisor_log}" >&2
            return 1
        fi
        sleep 0.1
    done
    printf 'Phase 11 supervisor startup timed out.\n' >&2
    return 1
}

stop_supervisor() {
    local identity pid
    if ! identity="$(read_identity "${supervisor_pid_file}" "${supervisor_ticks_file}")"; then
        printf 'Phase 11 supervisor is not running.\n'
        return 0
    fi
    pid="${identity%% *}"
    kill -TERM "${pid}"
    for _ in {1..100}; do
        if ! same_process "${pid}" "${identity#* }"; then
            printf 'Phase 11 supervisor stopped.\n'
            return 0
        fi
        sleep 0.1
    done
    printf 'Supervisor did not stop within 10 seconds (pid=%s).\n' "${pid}" >&2
    return 1
}

status_supervisor() {
    local identity supervisor_pid managed="stopped"
    if ! identity="$(read_identity "${supervisor_pid_file}" "${supervisor_ticks_file}")"; then
        printf 'Phase 11 supervisor is not running.\n'
        return 1
    fi
    supervisor_pid="${identity%% *}"
    if identity="$(read_identity "${managed_pid_file}" "${managed_ticks_file}")"; then
        managed="running(pid=${identity%% *})"
    fi
    printf 'Phase 11 supervisor is running (pid=%s, managed=%s).\n' \
        "${supervisor_pid}" "${managed}"
}

case "${1:-status}" in
    run) run_supervisor ;;
    start) start_supervisor ;;
    stop) stop_supervisor ;;
    restart) stop_supervisor; start_supervisor ;;
    status) status_supervisor ;;
    logs) tail -f "${supervisor_log}" ;;
    *)
        printf 'Usage: %s {run|start|stop|restart|status|logs}\n' "$0" >&2
        exit 2
        ;;
esac
