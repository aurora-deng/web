#!/usr/bin/env bash

# Compare repository migrations with the versions recorded by PostgreSQL.
# This is read-only: it never applies SQL and never edits the database.
set -euo pipefail

phase11_home="${PHASE11_HOME:-/home/pikachu}"
postgres_root="${PHASE11_POSTGRES_ROOT:-${phase11_home}/phase11-deps/postgresql/18.6}"
database="${1:-phase11}"
pgpass_file="${2:-${phase11_home}/phase11-data/secrets/phase11.pgpass}"
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
migration_dir="${project_root}/server/phase11/storage/postgres/migrations"
psql_bin="${postgres_root}/bin/psql"

[[ -x "${psql_bin}" ]] || { echo "psql is missing below ${postgres_root}." >&2; exit 1; }
[[ -r "${pgpass_file}" ]] || { echo "pgpass file is missing: ${pgpass_file}" >&2; exit 1; }
[[ -d "${migration_dir}" ]] || { echo "migration directory is missing: ${migration_dir}" >&2; exit 1; }

expected=()
for migration in "${migration_dir}"/[0-9][0-9][0-9]_*.sql; do
    [[ -e "${migration}" ]] || continue
    filename="$(basename "${migration}")"
    version_text="${filename%%_*}"
    expected+=("$((10#${version_text}))")
done
[[ "${#expected[@]}" -gt 0 ]] || { echo "No repository migrations found." >&2; exit 1; }

psql=("${psql_bin}" -h 127.0.0.1 -p 5432 -U phase11_app -d "${database}" -v ON_ERROR_STOP=1)
has_table="$(PGPASSFILE="${pgpass_file}" "${psql[@]}" -Atqc \
    "SELECT to_regclass('public.phase11_schema_migrations') IS NOT NULL")"
[[ "${has_table}" == "t" ]] || { echo "phase11_schema_migrations is missing in ${database}." >&2; exit 1; }

mapfile -t applied < <(PGPASSFILE="${pgpass_file}" "${psql[@]}" -Atqc \
    "SELECT version FROM phase11_schema_migrations ORDER BY version")

missing=()
unexpected=()
for version in "${expected[@]}"; do
    [[ " ${applied[*]} " == *" ${version} "* ]] || missing+=("${version}")
done
for version in "${applied[@]}"; do
    [[ " ${expected[*]} " == *" ${version} "* ]] || unexpected+=("${version}")
done

printf 'database=%s expected=%s applied=%s latest=%s\n' \
    "${database}" "${#expected[@]}" "${#applied[@]}" "${expected[-1]}"
printf 'applied_versions=%s\n' "${applied[*]:-none}"
if [[ "${#missing[@]}" -ne 0 || "${#unexpected[@]}" -ne 0 ]]; then
    printf 'missing_versions=%s\n' "${missing[*]:-none}" >&2
    printf 'unexpected_versions=%s\n' "${unexpected[*]:-none}" >&2
    exit 1
fi
echo "migration_status=ready"
