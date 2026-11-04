# Step 5 — Verified incremental incumbent refinement

## Scope and integration

Updated from `csbb_evaluator_integration_v6` by adding optional **post-MCNh local search** operating strictly on the complete PRODUCT permutation. It uses `mosp::IncrementalEvaluator` from the supplied standalone v2 library (insert / swap / reverse) with a private mutable evaluator and a shared immutable instance built once. This module is **executed before B&B**, serially. The solver's customer-closure order, dominance table, relaxation proofs and global lower-bound logic are unchanged.

Options (off by default):

- `--refine-incumbent 0|1`: enable after independently verified MCNh warm start.
- `--refine-moves insert|swap|reverse|mixed`: neighborhood, default `mixed`.
- `--refine-max-evals N`: evaluation budget, default 2500.
- `--refine-max-seconds S`: wall-clock preparation budget, default 0.5 s; included in the B&B global time limit.
- `--refine-seed N`: fixed pseudo-random seed, default 1.
- `--refine-mmosp-ties 0|1`: when objective MOSP ties, allow moves with strictly smaller total stack-open time; default 1. This is a secondary search criterion, never used as an upper/lower bound for MOSP.
- `--refine-output FILE`: optionally write a new **verified product-order** warm-start certificate (compatible with `--warm-start-file`). In `run_batch.py`, use `--refine-output 1` to generate separate files under `output-dir/refined_starts/`.

The search samples one move at a time and accepts only strict lexicographic improvements of `(MOSP, total_open)`, subject to the tie option. It evaluates each candidate incrementally, commits accepted moves, independently verifies every accepted move with both the standalone full evaluator and the original-CS sparse streaming evaluator, and verifies the final returned sequence again using the legacy interval evaluator. If validation fails, the program stops rather than admitting an invalid upper bound. Accepted values only decrease the **global feasible UB**, and only after independent verification. No local-search step can mark infeasibility or modify an exact proof certificate.

The output reports `refine_initial_ub`, `refine_final_ub`, `refine_evaluations`, `refine_mosp_improvements`, `refine_tie_acceptances`, `refine_seconds`, `refine_status`, `refine_enabled`. The **final B&B UB may be below the refined value**, because independent exact-search heuristics and the B&B itself can improve it later. A refined PRODUCT witness is kept separate from the B&B's CUSTOMER-closure order.

If no warm certificate is provided, refinement is skipped (`SKIPPED_NO_WARM_START`); with zero budget or an already-proven starting bound, no candidate is explored. The MCNh preprocessing remains optional and is not changed. When the global time limit is exhausted during refinement, subsequent B&B proof work remains subject to that deadline.

## Tests actually run

- 76-case original Chu–Stuckey regression: all matched.
- 151 warm-start certificate validations: all passed.
- 152 standalone/legacy integration validations: all passed.
- 31 dedicated refinement and certificate replay checks: all passed (all three neighborhoods and mixed mode, `customer`/`close-stack`, `relaxation off/full`, serial/parallel, repeatability of fixed-seed counters, malformed options, and absence of a warm start).
- 6 controlled-experiment protocol tests and 5 telemetry-pipeline tests: all passed.
- `A_AP-9.cs`: independently verified MCNh UB 6, refined UB 5 in 100 candidate evaluations under mixed moves, final B&B optimum UB=LB=5; the exported warm certificate was independently re-imported.
- Short controlled pilot: one supplied real instance per dimension (400/600/800/1000), 8 valid executions, 0.65-second B&B budget; no run completed an optimality proof. Results stored in `results/incumbent_pilot/reports/`.

### One-off controlled pilot results (not speedup evidence)

| CS dimensions | Verified MCNh UB | Refined MCNh UB | MOSP improvements | B&B final UB off/on | B&B final LB off/on |
|---|---:|---:|---:|---|---|
| 400×400 | 105 | 104 | 1 | 81 / 81 | 7 / 7 |
| 600×600 | 146 | 145 | 1 | 135 / 134 | 9 / 9 |
| 800×800 | 186 | 184 | 2 | 180 / 179 | 9 / 9 |
| 1000×1000 | 250 | 250 | 0 | 247 / 247 | 11 / 11 |

The pilot used 1500 candidate evaluations and at most 0.20 seconds of local-search wall time per treated run, with identical MCNh certificates in the pairs. It validates the method's feasibility and measurement pipeline. Changes to final B&B UB under a short time limit cannot be attributed solely to local search without additional repetitions/controlled budgets; **no proof-time acceleration was demonstrated**.

## Recommended controlled experiment

```sh
python3 scripts/controlled_experiments.py \
  --comparisons incumbent_refinement \
  --time-limit 3600 --threads 8 --repeats 3 \
  --output-dir results/refine_comparison_1h
```

One comparison changes only `--refine-incumbent 0 -> 1`; candidate budget and seed remain equal. The report includes refined UB, evaluations, improvement counts and refinement time in addition to overall B&B UB/LB/gap and proof-time metrics.

## Remaining uncertainty

Although incremental move evaluations can be substantially cheaper than full evaluations, the local search itself may not accelerate proof. This depends on whether the verified MCNh solution improves enough to outweigh preparation time and whether tighter UBs reduce subsequent nodes or target tests. Larger replicated, time-controlled studies are needed before enabling refinement by default.
