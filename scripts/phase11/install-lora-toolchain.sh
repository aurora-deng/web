#!/usr/bin/env bash
# Compatibility only; explicit --install and dependency approval are still required.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec bash "${root}/scripts/ai/toolchain/install-lora-toolchain.sh" "$@"
