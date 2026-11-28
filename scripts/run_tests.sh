#!/usr/bin/env bash
# Compatibility entrypoint; implementation: scripts/testing/run_tests.sh
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec "${BASH:-bash}" "${script_dir}/testing/run_tests.sh" "$@"
