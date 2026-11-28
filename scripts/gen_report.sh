#!/usr/bin/env bash
# Compatibility entrypoint; implementation: scripts/reports/gen_report.sh
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec "${BASH:-bash}" "${script_dir}/reports/gen_report.sh" "$@"
