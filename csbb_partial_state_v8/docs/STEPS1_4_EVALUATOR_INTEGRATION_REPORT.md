# Integration of MOSP Evaluator standalone v2 into Chu–Stuckey B&B — steps 1–4

Date: 2026-10-30. Baseline: `csbb_instrumented_experiments_v2`. Imported library: `mosp_evaluator_standalone_v2.zip` (MIT evaluator components only; original PT-MOSP overlays were not copied).

## Exact scope

**Step 1 — Hotspot instrumentation**

- Added opt-in `--profile-evaluator 1` and cumulative atomic counters/nanosecond clocks for **legacy interval**, **legacy streaming**, **standalone full**, **sparse independent streaming**, **customer-order decoding**, **independent customer-witness verification**.
- Reported bridge setup time and size in incidences; output fields exist in both text and JSON. Counters permit concurrent closure branches and do not run under the default `--profile-evaluator 0` except the optional full-witness check count.
- Full B&B total-cost comparisons must continue to use `elapsed_seconds`, `cpu_seconds`, nodes per phase, UB/LB/gap; evaluator timing alone is not a solver speedup.

**Step 2 — C++ bridge**

- Added `include/csbb/mosp_bridge.hpp`, with one immutable incidence transpose from CS customer-row/product-column representation to the library's product-pattern/customer-item representation.
- `mosp::Instance::Ptr` can be shared safely by workers. Each evaluation constructs an independent `mosp::Evaluator` with private mutable state, with no locks and no corruption of B&B search state.
- Vendored only `include/mosp/evaluator.hpp`, `src/evaluator.cpp` and LICENSE; adjusted `scripts/build.sh` to compile and link this code under C++20.
- Presence-mode semantics are resolved by the B&B parser *before* bridge creation. All IDs remain 0-based internally.

**Step 3 — Verified MCNh upper bounds and complete product witnesses**

- `--witness-evaluator legacy|standalone|dual`, default `legacy`.
  - `legacy`: original *interval* and *streaming* independent verifiers.
  - `standalone`: standalone evaluator plus an independent **sparse streaming** verifier that uses original CS customer/product incidences.
  - `dual`: legacy interval, legacy streaming, standalone, sparse streaming. Any disagreement is an error, and no inconsistent upper bound is accepted.
- All modes still reject value-only files, duplicate/wrong-domain sequences, mismatched dimensions, and mismatched declared value when policy is `value` or `strict`. `sequence` policy deliberately disregards a wrong declaration but still validates the product permutation and recomputes the MOSP objective.
- `--verify-customer-witness 1` reconstructs the actual **product** permutation from an accepted **customer** permutation, checks its intrinsic decision cost and validates the product objective with the standalone evaluator. When `--witness-evaluator dual`, the independent legacy and sparse verifiers are additionally checked. If the witness is invalid, the code fails closed rather than silently asserting feasibility. Disabled by default to preserve timing/behavior until calibrated.

**Step 4 — Faster customer-order reconstruction**

- `--witness-decode legacy|sparse|dual`, default `legacy`.
- `sparse`: maintain the count of not-yet-opened prerequisite customers per product; each customer is activated at most once and only its incident products are updated. When products become available within a closing step, flush them in ascending product ID, preserving the legacy ordering exactly; handle products with no incidences consistently.
- `dual`: run both decoders and assert equality of **both customer decision cost** and the **exact reconstructed product order**. This is for regression/audit, not production speed.
- The customer-order cost, which can exceed the actual product-sequence MOSP, remains **unchanged** as a search quantity. The standalone product MOSP is never substituted into a lower bound, hash dominance relation, or relaxed-graph proof.

## CLI example

```sh
./scripts/build.sh
./csbb_modern --input instances/A_AP-9.cs --input-format cs \
 --witness-evaluator dual --witness-decode dual \
 --verify-customer-witness 1 --profile-evaluator 1 \
 --branching customer --relaxation full --threads 2 --time-limit 3600 --json
```

For performance experiments, run `legacy` versus `standalone` and `legacy` versus `sparse` in controlled pairs; *do not* use `dual` as the treatment when estimating a raw execution-time benefit.

```sh
python3 scripts/controlled_experiments.py --comparisons witness_evaluator,witness_decoder \
 --threads auto --time-limit 3600 --output-dir results/evaluator_1h
```

`run_batch.py` accepts the four parameters explicitly and includes their description in `--describe-parameters`. The controlled-experiment report includes evaluation-specific counters and seconds in `runs.csv` and `by_configuration.csv`.

## Regression and validation actually executed

- Original-vs-modern customer solver: **76 cases, 0 failures**, including `relaxation=full` and parallel/close-stack variants.
- Previously established MCNh certificate regression: **151 checks passed**.
- New bridge regression: **152 checks passed** across 24 randomized (binary) matrices and four real dimensions (400, 600, 800, 1000); checks include legacy/standalone/dual, false declared upper bounds, full relaxation and parallel close-stack variants.
- Additional **108 matched executions** (76 randomized instances plus variants involving parallel and full-relaxation search) comparing `--witness-decode legacy` and `sparse` with verification on. No differences in OPTIMAL / UB / LB.
- Original controlled-configuration tests: 6/6, telemetry pipeline tests: 5/5, and close-stack parallel regression pass.
- A nonbinary positive-presence instance also passed all three verifier backends, returning the same optimum.
- AddressSanitizer/UndefinedBehaviorSanitizer small mixed-mode smoke test (`full`, `close-stack`, two workers, dual verifier and decoder) passed with UB=LB=5.
- Actual campaign: `results/evaluator_pilot_final`, **16/16 valid** runs (one FCS instance per size, 0.25 s B&B limit, one repeat). Every run ended in TIME_LIMIT; no inference about proven B&B acceleration is possible.

### Pilot: warm-start certificate verification time

Observed single-call durations under `--profile-evaluator 1` (sum of the two independent validation backends, no bridge setup). They are **single observations**, not reliable speedup estimates:

| CS size | Two legacy evaluations | Standalone + sparse streaming | Ratio, validation step only |
|:--|--:|--:|--:|
| 400×400 | 442 µs | 21 µs | 21.0× |
| 600×600 | 1110 µs | 119 µs | 9.3× |
| 800×800 | 1347 µs | 74 µs | 18.2× |
| 1000×1000 | 2304 µs | 97 µs | 23.8× |

These microsecond differences affect **one certificate per instance** and can be negligible relative to seconds or hours of B&B search. The pilot's UB differences in TIME_LIMIT runs are wall-time scheduling/trajectory effects and **cannot be assigned causally to faster decoding** (the decoder was not exercised in those particular runs). The full baseline solver remains the reference for practical speedups.

## Decision and remaining work

Steps 1–4 are implemented. Defaults retain previous behavior (`legacy` backend/decoder, no extra full-customer-witness verification). The new paths are available, tested, and optional. The **incremental evaluator for local insertion/swap/reverse refinement** and the **incremental B&B state evaluator** belong to proposed steps 5–6 and were intentionally not implemented here. Neither may be used as a certified lower bound or as a replacement for exact customer-graph search without separate mathematical proof.
