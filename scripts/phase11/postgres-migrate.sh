#!/usr/bin/env bash

# 对指定 Phase 11 数据库执行迁移。口令只由权限为 0600 的 pgpass 文件提供，
# 不进入命令行、源码或日志。默认操作正式学习库；测试库必须显式传入第二个参数。
set -eu

phase11_home="${PHASE11_HOME:-/home/pikachu}"
postgres_root="${PHASE11_POSTGRES_ROOT:-${phase11_home}/phase11-deps/postgresql/18.6}"
database="${1:-phase11}"
pgpass_file="${2:-${phase11_home}/phase11-data/secrets/phase11.pgpass}"
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
migration_dir="${project_root}/server/phase11/storage/postgres/migrations"

if [[ ! -x "${postgres_root}/bin/psql" ]]; then
    echo "psql is missing below ${postgres_root}." >&2
    exit 1
fi
if [[ ! -r "${pgpass_file}" ]]; then
    echo "pgpass file is missing or unreadable: ${pgpass_file}" >&2
    exit 1
fi
if [[ ! -d "${migration_dir}" ]]; then
    echo "migration directory is missing: ${migration_dir}" >&2
    exit 1
fi

psql=("${postgres_root}/bin/psql" -h 127.0.0.1 -p 5432
      -U phase11_app -d "${database}" -v ON_ERROR_STOP=1)

found=0
for migration in "${migration_dir}"/[0-9][0-9][0-9]_*.sql; do
    [[ -e "${migration}" ]] || continue
    found=1
    filename="$(basename "${migration}")"
    version_text="${filename%%_*}"
    version="$((10#${version_text}))"

    has_migration_table="$(PGPASSFILE="${pgpass_file}" "${psql[@]}" -Atqc \
        "SELECT to_regclass('public.phase11_schema_migrations') IS NOT NULL")"
    already_applied="f"
    if [[ "${has_migration_table}" == "t" ]]; then
        already_applied="$(PGPASSFILE="${pgpass_file}" "${psql[@]}" -Atqc \
            "SELECT EXISTS(SELECT 1 FROM phase11_schema_migrations WHERE version=${version})")"
    fi
    if [[ "${already_applied}" == "t" ]]; then
        echo "Migration ${version} is already applied to ${database}."
        continue
    fi

    echo "Applying migration ${version}: ${filename}"
    PGPASSFILE="${pgpass_file}" "${psql[@]}" -f "${migration}"
done

if [[ "${found}" -eq 0 ]]; then
    echo "No migrations found below ${migration_dir}." >&2
    exit 1
fi
