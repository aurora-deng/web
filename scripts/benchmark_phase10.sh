#!/usr/bin/env bash
# Compatibility entrypoint; implementation: scripts/benchmarks/benchmark_phase10.sh
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec "${BASH:-bash}" "${script_dir}/benchmarks/benchmark_phase10.sh" "$@"
