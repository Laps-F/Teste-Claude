#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"${CXX:-g++}" -std=c++20 -O2 -pthread -I"$ROOT/include" \
  -I"$ROOT/third_party/mosp_evaluator/include" \
  "$ROOT/src/csbb_modern.cpp" "$ROOT/third_party/mosp_evaluator/src/evaluator.cpp" \
  -o "$ROOT/csbb_modern"
python3 "$ROOT/tests/test_partial_state_eval.py"
python3 "$ROOT/tests/run_regression.py"
python3 "$ROOT/tests/test_warm_start_certificates.py"
python3 "$ROOT/tests/test_standalone_integration.py"
python3 "$ROOT/tests/test_incumbent_refinement.py"
echo 'Partial state regression suite passed.'
