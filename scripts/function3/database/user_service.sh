#!/usr/bin/env bash

# Phase 11 使用用户目录中的 PostgreSQL，不需要 root，也不注册 systemd 服务。
# 路径可以通过环境变量覆盖；默认值与学习虚拟机的已批准安装位置一致。
set -eu

phase11_home="${PHASE11_HOME:-/home/pikachu}"
postgres_root="${PHASE11_POSTGRES_ROOT:-${phase11_home}/phase11-deps/postgresql/18.6}"
postgres_data="${PHASE11_POSTGRES_DATA:-${phase11_home}/phase11-data/postgresql/data}"
postgres_run="${PHASE11_POSTGRES_RUN:-${phase11_home}/phase11-data/postgresql/run}"
postgres_log="${PHASE11_POSTGRES_LOG:-${phase11_home}/phase11-data/postgresql/startup.log}"

pg_ctl="${postgres_root}/bin/pg_ctl"
pg_isready="${postgres_root}/bin/pg_isready"

require_runtime() {
    if [[ ! -x "${pg_ctl}" || ! -d "${postgres_data}" ]]; then
        echo "PostgreSQL runtime or data directory is missing." >&2
        echo "root=${postgres_root}" >&2
        echo "data=${postgres_data}" >&2
        exit 1
    fi
    mkdir -p "${postgres_run}" "$(dirname "${postgres_log}")"
}

case "${1:-status}" in
    start)
        require_runtime
        if "${pg_ctl}" -D "${postgres_data}" status >/dev/null 2>&1; then
            echo "Phase 11 PostgreSQL is already running."
        else
            "${pg_ctl}" -D "${postgres_data}" -l "${postgres_log}" start
        fi
        "${pg_isready}" -h "${postgres_run}" -p 5432
        ;;
    stop)
        require_runtime
        if "${pg_ctl}" -D "${postgres_data}" status >/dev/null 2>&1; then
            # fast 会中止活动事务并正常写盘，比 immediate 更适合日常停机。
            "${pg_ctl}" -D "${postgres_data}" stop -m fast
        else
            echo "Phase 11 PostgreSQL is not running."
        fi
        ;;
    restart)
        require_runtime
        "${pg_ctl}" -D "${postgres_data}" restart -m fast -l "${postgres_log}"
        "${pg_isready}" -h "${postgres_run}" -p 5432
        ;;
    ready)
        require_runtime
        "${pg_isready}" -h "${postgres_run}" -p 5432
        "${pg_isready}" -h 127.0.0.1 -p 5432
        ;;
    status)
        require_runtime
        "${pg_ctl}" -D "${postgres_data}" status
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|ready|status}" >&2
        exit 2
        ;;
esac
