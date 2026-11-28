#!/usr/bin/env bash
# Compatibility entrypoint; implementation: scripts/benchmarks/benchmark.sh
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec "${BASH:-bash}" "${script_dir}/benchmarks/benchmark.sh" "$@"
