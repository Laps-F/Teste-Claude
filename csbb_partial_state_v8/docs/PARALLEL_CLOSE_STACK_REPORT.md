# Parallel close-stack branching — technical report (v5)

## Scope and compatibility

Base release: `csbb_parallel_relaxed_proofs_v4` (2026-10-28). This patch parallelizes the existing *experimental* `--branching close-stack` (and the existing `auto` alias), leaving `--branching customer` as the default. This patch does not introduce a new mathematical closure decomposition; it parallelizes the preexisting complete close-stack feasibility search and retains the mandatory exact customer-search fallback on closure failure.

The parallel search is an acceleration mechanism for finding original-instance witnesses. An unsuccessful close-stack search, whether serial or parallel, **never establishes an infeasibility certificate**. Only the original customer-search decision solver or the validated contracted-graph exact proof can establish infeasibility. The returned upper bound is accepted only following the preexisting complete-permutation/original-instance evaluator checks. The original customer/DP recurrence, certified lower bounds, and verified MCNh warm start are unchanged.

## Implementation

- Introduced a common exception-safe `dispatch_frontier()` used by the preexisting customer parallel decision solver and the new close-stack parallel decision solver. Each worker obtains the next index from an atomic job counter; thread creation failures stop and join workers before propagating an exception.
- Added `ClosureFrontierState` with full continuation data: processed patterns, closed items, active open items, and accumulated pattern permutation. `closure_successor` implements the **same** closure/block transition used by both serial DFS and frontier expansion.
- `expand_closure_frontier` partitions the first D closure decisions, obeying a configurable frontier cap. If splitting would exceed the cap, the *entire* parent subtree is kept as one job. No remaining search subtree is discarded.
- Each worker owns its own DFS memoization set (`dead`) and counters. Shared state is limited to the atomic stop signal, atomic total node counter, atomic result status, a mutex-protected feasible witness, and read-only instance data. A verified feasible witness takes precedence over a simultaneous timeout/node-limit status.
- Main thread aggregates per-worker counters after all workers join. `closure_branch_nodes`, `closure_branch_successes`, and peak dead-table size therefore have no concurrent writes.
- Both `--branching close-stack` and `--branching auto` use the same closure search, as in the prior release. `auto` does not yet implement an adaptive selector.

### New options

- `--closure-threads N`: `0` (default) inherits `--threads`; `1` forces serial closure search.
- `--closure-parallel-depth D`: `-1` (default) inherits `--parallel-depth`; `0` forces serial closure search.
- `--closure-frontier-cap N`: maximum number of frontier states, default 8192; `N>=1`; a small cap retains unsplit subtrees rather than truncating them.

All are available directly in the solver and through `scripts/run_batch.py` and `--describe-parameters`.

### Added JSON/text metrics

- `closure_parallel_calls`: count of actual closure parallel dispatcher invocations with >1 worker.
- `closure_frontier_states`: total frontier tasks generated across targets.
- `closure_max_workers`: maximum number of workers launched by a close-stack pass.
- `closure_threads`, `closure_parallel_depth`, `closure_frontier_cap`: effective configuration.

Note that `closure_max_workers` is the number launched; it is *not* measured instantaneous CPU occupancy.

## Validation

- Original code regression: **76/76** small cases agree (from `tests/run_regression.py`).
- Prior parallel contracted-graph proofs: **232** variant runs (58 cases × 4), **102** parallel proof calls; all pass.
- Parallel close-stack tests: **20** deterministic random instances plus 2 adversarial/benchmark cases; **110** serial/parallel/auto variant runs. **185** parallel closure calls, with >1 worker reported in 42 runs. Every tested optimum matches the exact customer-search result.
- Close-stack sequential backward comparison: **35/35** previously generated cases match the v4 serial close-stack status and UB/LB.
- Other independently run tests retained: **151** MCNh warm start certificate checks, **516** residual bound CLI runs, **47,132** exact residual states, **419,200** k-core checks, and **1,875** reversible contraction checks.
- Explicit tests of frontier caps 1, 2, 3; thread inheritance/override and serial opt-out; stop due to node and time limits; `relaxation full` combined with customer, close-stack and auto; and rejected invalid CLI parameters.

`./scripts/test.sh` includes the new tests. Some all-in-one executions exceeded the tool's execution window although each major suite was also executed successfully as an individual command; the above numbers are from successful, completed suites, not from an unfinished run.

## Large-scale smoke pilot

`results/parallel_closure_scalability_pilot.csv` contains one real Frinhani–Carvalho–Soma instance each at 400, 600, 800, and 1000, with close-stack, `--relaxation off`, 1 vs 4 threads, 1.25-second limits, without MCNh. All eight runs stopped at `TIME_LIMIT`, with correctly reported bounds; parallel calls occurred in all four 4-worker trials. An isolated 400×400 instance produced UB 387 serially vs 388 with 4 workers in this short pilot. Both were far from a proven optimum. These results establish only that the pipeline runs and interrupts correctly, **not** competitive performance or a guaranteed improvement in the UB. Runs without MCNh start from weak incumbents.

## Recommended production use / caution

Keep `--branching customer` as the proven-performance default while comparing experimental close-stack configurations at a realistic time budget. To test close-stack parallelism:

```sh
./csbb_modern --input instances/frinhani_carvalho_soma/400/Random-400-400-2-1.txt \
  --input-format cs --branching close-stack \
  --threads 8 --parallel-depth 1 \
  --closure-threads 0 --closure-parallel-depth -1 \
  --closure-frontier-cap 8192 --time-limit 3600 --json
```

To isolate its effect, repeat with `--closure-threads 1`, using identical MCNh warm start, target, time limit and all other switches. Note that `--relaxation full` executes its own contracted-graph proof phase first, governed separately by `--relax-proof-threads`, and only invokes close-stack branching if that phase returns inconclusive. Thus, two parallel phases may run *sequentially* but not simultaneously within one decision search.

More extensive runtime/memory benchmarking, and an actual adaptive `auto` branch selector, remain future work.
