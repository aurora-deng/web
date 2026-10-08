#!/usr/bin/env bash

# Phase 26 容量矩阵：长驻 WebSocket、真实 AI 并发、AI 与普通聊天混合负载。
# 它复用 Phase 15 的隔离数据库和独占服务驱动，不安装或下载任何依赖。
set -euo pipefail

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

# Phase 15 驱动会暂时停止 18090 正式实例。显式开关可避免误伤正在收集的
# 24 小时稳定性证据；开关只表达“允许重启”，不跳过任何测试校验。
if [[ "${PHASE26_ALLOW_SERVICE_RESTART:-0}" != 1 ]]; then
    echo "Refusing to restart the formal service." >&2
    echo "Run after the active soak has finished, then set PHASE26_ALLOW_SERVICE_RESTART=1." >&2
    exit 2
fi

for required in PHASE15_AI_MODEL_ARTIFACT PHASE15_AI_MODEL_CHECKSUM; do
    [[ -n "${!required:-}" ]] || {
        echo "${required} is required for the real AI capacity matrix." >&2
        exit 2
    }
done

export PHASE15_DATABASE="${PHASE15_DATABASE:-phase15_benchmark_phase26}"
export PHASE15_RESULT_ROOT="${PHASE15_RESULT_ROOT:-${project_root}/artifacts/benchmarks/phase26}"
export PHASE15_RUNS="${PHASE15_RUNS:-3}"
export PHASE15_WORKERS="${PHASE15_WORKERS:-4}"
export PHASE15_OPERATIONS="${PHASE15_OPERATIONS:-80}"
export PHASE15_WEBSOCKET_CONNECTION_STEPS="${PHASE15_WEBSOCKET_CONNECTION_STEPS:-50,200,500}"
export PHASE15_WEBSOCKET_OPEN_WORKERS="${PHASE15_WEBSOCKET_OPEN_WORKERS:-32}"
export PHASE15_WEBSOCKET_SETTLE_SECONDS="${PHASE15_WEBSOCKET_SETTLE_SECONDS:-2}"
export PHASE15_AI_JOBS="${PHASE15_AI_JOBS:-8}"
export PHASE15_AI_CONCURRENCY="${PHASE15_AI_CONCURRENCY:-4}"
export PHASE15_AI_MIXED_JOBS="${PHASE15_AI_MIXED_JOBS:-4}"
export PHASE15_AI_MIXED_CONCURRENCY="${PHASE15_AI_MIXED_CONCURRENCY:-4}"
export PHASE15_AI_MIXED_CHAT_OPERATIONS="${PHASE15_AI_MIXED_CHAT_OPERATIONS:-80}"

exec "${project_root}/scripts/phase15-business-benchmark.sh"
