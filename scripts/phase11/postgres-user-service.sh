#!/usr/bin/env bash
# Compatibility entrypoint; implementation: scripts/function3/database/user_service.sh
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec bash "${script_dir}/../function3/database/user_service.sh" "$@"
