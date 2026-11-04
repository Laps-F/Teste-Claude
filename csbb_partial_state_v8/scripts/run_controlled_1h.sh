#!/usr/bin/env bash
# Run the predeclared paired comparison campaign using local, sequential B&B invocations.
# Usage: TIME_LIMIT=3600 REPEATS=1 SUITE=core ./scripts/run_controlled_1h.sh [output-dir]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
if [[ ! -x ./csbb_modern || ! -x ./mcnh_modern ]]; then
  ./scripts/build.sh
fi
SUITE="${SUITE:-core}"
TIME_LIMIT="${TIME_LIMIT:-3600}"
REPEATS="${REPEATS:-1}"
THREADS="${THREADS:-auto}"
RESERVE_CORES="${RESERVE_CORES:-1}"
OUT="${1:-results/controlled_${SUITE}_${TIME_LIMIT}s}"
ARGS=( --suite "$SUITE" --time-limit "$TIME_LIMIT" --repeats "$REPEATS" --threads "$THREADS" --reserve-cores "$RESERVE_CORES" --output-dir "$OUT" )
if [[ "${RESUME:-0}" == "1" ]]; then ARGS+=( --resume ); fi
python3 scripts/controlled_experiments.py "${ARGS[@]}"
python3 scripts/controlled_report.py --output-dir "$OUT"
