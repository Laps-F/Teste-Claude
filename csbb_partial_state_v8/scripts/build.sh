#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-g++}"
CXXFLAGS="${CXXFLAGS:--std=c++20 -O3 -march=native -flto -DNDEBUG -pthread}"
"$CXX" $CXXFLAGS -I"$ROOT/include" -I"$ROOT/third_party/mosp_evaluator/include" \
  "$ROOT/src/csbb_modern.cpp" "$ROOT/third_party/mosp_evaluator/src/evaluator.cpp" -o "$ROOT/csbb_modern"
"$CXX" -std=c++20 -O3 -pthread "$ROOT/original/previous_modern.cpp" -o "$ROOT/csbb_baseline"
"$CXX" -std=gnu++11 -O3 -w "$ROOT/original/mosp.c" -o "$ROOT/original/chu_stuckey_2009"
"$CXX" -std=c++20 -O3 -pthread "$ROOT/src/mcnh_modern.cpp" -o "$ROOT/mcnh_modern"
echo "Built csbb_modern, csbb_baseline, original/chu_stuckey_2009, mcnh_modern"
