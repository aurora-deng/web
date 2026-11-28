#!/usr/bin/env bash

# Phase 11 logical backup and isolated restore drill.
# Formal data is read by pg_dump only. Restore drills are restricted to a
# database name beginning with phase11_restore_drill and are removed on exit.
set -euo pipefail

phase11_home="${PHASE11_HOME:-/home/pikachu}"
postgres_root="${PHASE11_POSTGRES_ROOT:-${phase11_home}/phase11-deps/postgresql/18.6}"
postgres_run="${PHASE11_POSTGRES_RUN:-${phase11_home}/phase11-data/postgresql/run}"
backup_root="${PHASE11_BACKUP_ROOT:-${phase11_home}/phase11-data/backups}"
database="${PHASE11_BACKUP_DATABASE:-phase11}"
app_user="${PHASE11_POSTGRES_APP_USER:-phase11_app}"
admin_user="${PHASE11_POSTGRES_ADMIN_USER:-pikachu}"
pgpass_file="${PHASE11_PGPASS_FILE:-${phase11_home}/phase11-data/secrets/phase11.pgpass}"

pg_dump="${postgres_root}/bin/pg_dump"
pg_restore="${postgres_root}/bin/pg_restore"
psql="${postgres_root}/bin/psql"
createdb="${postgres_root}/bin/createdb"
dropdb="${postgres_root}/bin/dropdb"

for tool in "${pg_dump}" "${pg_restore}" "${psql}" "${createdb}" "${dropdb}"; do
    [[ -x "${tool}" ]] || { echo "PostgreSQL tool is missing: ${tool}" >&2; exit 1; }
done

verify_backup() {
    local archive="$1"
    [[ -r "${archive}" ]] || { echo "Backup is unreadable: ${archive}" >&2; return 1; }
    [[ -r "${archive}.sha256" ]] || { echo "Checksum is missing: ${archive}.sha256" >&2; return 1; }
    (cd "$(dirname "${archive}")" && sha256sum -c "$(basename "${archive}.sha256")")
    "${pg_restore}" --list "${archive}" >/dev/null
    echo "backup_status=verified archive=${archive}"
}

create_backup() {
    [[ -r "${pgpass_file}" ]] || { echo "pgpass file is missing: ${pgpass_file}" >&2; return 1; }
    mkdir -p "${backup_root}"
    chmod 700 "${backup_root}"
    local stamp final partial
    stamp="$(date -u +%Y%m%dT%H%M%SZ)"
    final="${backup_root}/${database}-${stamp}.dump"
    partial="${final}.partial"
    trap 'rm -f -- "${partial}"' RETURN
    PGPASSFILE="${pgpass_file}" "${pg_dump}" -h 127.0.0.1 -p 5432 \
        -U "${app_user}" -d "${database}" --format=custom --compress=6 \
        --no-owner --no-privileges --file="${partial}"
    "${pg_restore}" --list "${partial}" >/dev/null
    mv -- "${partial}" "${final}"
    (cd "${backup_root}" && sha256sum "$(basename "${final}")" > "$(basename "${final}").sha256")
    chmod 600 "${final}" "${final}.sha256"
    verify_backup "${final}"
    printf '%s\n' "${final}"
}

restore_drill() {
    local archive="$1"
    local drill_db="${PHASE11_RESTORE_DRILL_DATABASE:-phase11_restore_drill}"
    [[ "${drill_db}" == phase11_restore_drill* ]] || {
        echo "Unsafe restore target: ${drill_db}" >&2
        return 1
    }
    verify_backup "${archive}"
    cleanup_drill() {
        "${dropdb}" -h "${postgres_run}" -p 5432 -U "${admin_user}" \
            --if-exists "${drill_db}" >/dev/null 2>&1 || true
    }
    trap cleanup_drill RETURN
    cleanup_drill
    "${createdb}" -h "${postgres_run}" -p 5432 -U "${admin_user}" "${drill_db}"
    "${pg_restore}" -h "${postgres_run}" -p 5432 -U "${admin_user}" \
        -d "${drill_db}" --no-owner --no-privileges --exit-on-error "${archive}"
    local migration_count user_count conversation_count message_count
    migration_count="$("${psql}" -h "${postgres_run}" -p 5432 -U "${admin_user}" -d "${drill_db}" -Atqc \
        'SELECT COUNT(*) FROM phase11_schema_migrations')"
    user_count="$("${psql}" -h "${postgres_run}" -p 5432 -U "${admin_user}" -d "${drill_db}" -Atqc \
        'SELECT COUNT(*) FROM users')"
    conversation_count="$("${psql}" -h "${postgres_run}" -p 5432 -U "${admin_user}" -d "${drill_db}" -Atqc \
        'SELECT COUNT(*) FROM conversations')"
    message_count="$("${psql}" -h "${postgres_run}" -p 5432 -U "${admin_user}" -d "${drill_db}" -Atqc \
        'SELECT COUNT(*) FROM messages')"
    printf 'restore_status=verified migrations=%s users=%s conversations=%s messages=%s\n' \
        "${migration_count}" "${user_count}" "${conversation_count}" "${message_count}"
}

case "${1:-}" in
    create) create_backup ;;
    verify) [[ "$#" -eq 2 ]] || { echo "verify requires an archive path" >&2; exit 2; }; verify_backup "$2" ;;
    restore-drill) [[ "$#" -eq 2 ]] || { echo "restore-drill requires an archive path" >&2; exit 2; }; restore_drill "$2" ;;
    *) echo "Usage: $0 {create|verify ARCHIVE|restore-drill ARCHIVE}" >&2; exit 2 ;;
esac
