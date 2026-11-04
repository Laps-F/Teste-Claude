#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"$ROOT/scripts/build.sh"
"${CXX:-g++}" -std=c++20 -O2 -pthread -I"$ROOT/include" -I"$ROOT/third_party/mosp_evaluator/include" "$ROOT/tests/test_contractions.cpp" "$ROOT/third_party/mosp_evaluator/src/evaluator.cpp" -o "$ROOT/tests/test_contractions"
"$ROOT/tests/test_contractions"
python3 "$ROOT/tests/run_regression.py"
python3 "$ROOT/tests/test_parallel_relaxed_proofs.py"
python3 "$ROOT/tests/test_parallel_close_stack.py"
python3 "$ROOT/tests/test_warm_start_certificates.py"
"${CXX:-g++}" -std=c++20 -O0 -pthread -I"$ROOT/include" -I"$ROOT/third_party/mosp_evaluator/include" "$ROOT/tests/test_bound_operators.cpp" "$ROOT/third_party/mosp_evaluator/src/evaluator.cpp" -o "$ROOT/tests/test_bound_operators"
"$ROOT/tests/test_bound_operators"
python3 "$ROOT/tests/test_residual_bounds.py"
python3 "$ROOT/tests/test_partial_state_eval.py"
python3 "$ROOT/scripts/run_batch.py" --describe-parameters >/dev/null
python3 -m py_compile "$ROOT/scripts/run_batch.py"
echo "All portable regression tests passed."
