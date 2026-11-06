#!/usr/bin/env bash
# Same executable, fixed existing resettable test DB; no global environment changes.
# Requires provisioned tools. No installation, service stop, cache drop or sudo.
set -euo pipefail
SOURCE="${SOURCE:-/home/pikachu/function3-src}"
BUILD_DIR="${BUILD_DIR:-$SOURCE/build-release}"
BINARY="${BINARY:-$BUILD_DIR/webserver}"
ROUNDS="${ROUNDS:-10}"
OPERATIONS="${OPERATIONS:-12000}"
STAGE="${STAGE:-phase50}"
LABEL="${LABEL:-aa-$(date -u +%Y%m%dT%H%M%SZ)}"
OUT="/home/pikachu/phase11-data/function3/validation/$STAGE/benchmarks-$LABEL"
fail() { printf '%s\n' "$*" >&2; exit 1; }
[[ "$ROUNDS" =~ ^[0-9]+$ ]] && ((ROUNDS>=10)) || fail 'ROUNDS must be >=10'
[[ "$OPERATIONS" =~ ^[0-9]+$ ]] && ((OPERATIONS>=4000)) || fail 'OPERATIONS must be >=4000'
[[ "$LABEL" =~ ^[a-z0-9-]{1,40}$ ]] || fail 'Invalid evidence label'
[[ "$STAGE" =~ ^(phase43-47|phase48|phase49|phase50)$ ]] || fail 'Invalid stage'
[[ -x "$BINARY" ]] || fail 'Build a Linux executable using the approved toolchain first'
command -v python3 >/dev/null || fail 'Missing python3; no automatic installation'
command -v wrk >/dev/null || fail 'Missing wrk; request approval before installing tools'
# Harness locks and checks ports BEFORE touching any test DB.
python3 "$SOURCE/scripts/function3/validation/phase47_benchmark.py" \
  --stage "$STAGE" --baseline-stage "$STAGE" --rounds "$ROUNDS" \
  --operations "$OPERATIONS" --label "$LABEL" --calibration \
  --baseline-binary "$BINARY" --current-binary "$BINARY"
python3 "$SOURCE/scripts/function3/validation/measurement_noise.py" \
  --summary "$OUT/summary.json" --out-dir "$OUT/analysis"
printf 'Evidence: %s\nNo acceptance verdict or historical result was changed.\n' "$OUT"
