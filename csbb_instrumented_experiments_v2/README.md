# MOSP — Modernized Chu–Stuckey B&B, parallel full-relaxation proofs

This package implements the **full exact graph-contraction relaxation cycle** as an opt-in feature of the previously modernized Chu–Stuckey customer-search solver.

**Default compatibility:** `--relaxation off` (same as previous modern release). `root-merge-probe` remains available. `full` performs exact infeasibility proofs on contracted graphs, reverses contractions, lifts and verifies witnesses, and falls back to original-customer exact search when necessary. Full mode is **experimental for performance**: real large-scale 2-second pilots did not establish a speed advantage over `off`.

## Build

```sh
./scripts/build.sh
```

Requires a modern C++20 compiler (GCC or Clang), Python 3.8+ for automation, a POSIX shell. `-march=native` is for the local CPU only; remove it to build portable binaries.

## Basic full-relaxation execution

```sh
./csbb_modern --input instances/A_AP-9.cs --input-format cs \
  --relaxation full --relax-merge-policy original \
  --relax-unmerge-policy original --time-limit 3600 \
  --threads 1 --profile 1 --json
```

New optional parameters:

- `--relaxation off|root-merge-probe|full`
- `--relax-merge-policy original` — original 2009 danger score for selecting adjacent customers to contract
- `--relax-unmerge-policy original|lifo` — original-inspired candidate-extension heuristic or LIFO undo
- `--relax-max-merges N` — 0 = merge toward current decision target, while resources allow
- `--relax-max-time-share P` — fraction of the **overall** finite B&B time budget allocated to exact relaxations, default 0.25. The unused time remains available for original-graph search

The `full` mode uses only a **verified upper bound**: it constructs and independently checks a customer-close order; it never trusts an arbitrary `value` field or `--upper` alone. During full mode, the initial lower bound is limited to the proven customer-simultaneity bound. The legacy `model` / `graph` options are compatibility aliases which now apply only the mathematically valid customer-stack lower bound in **all** modes; their old values are diagnostics. See `docs/TECHNICAL_REPORT.md` for the counterexample exposing a false optimality claim in the preceding release.

`--branching customer`, `close-stack`, and `auto` are accepted. A tentative close-stack witness is independently validated in the original customer graph before full mode accepts it. Exact contracted-graph proofs now use an independent bounded parallel frontier when more than one proof worker is requested. Each worker has its own dominance table. The customer-search fallback preserves its original parallel settings. Both serial and parallel proof modes remain available.

## Batch execution (three original presets preserved)

```sh
python3 scripts/run_batch.py --instances-dir instances/frinhani_carvalho_soma \
  --file-glob '**/*.txt' --solver ./csbb_modern \
  --preset original-modernized --relaxation full \
  --use-mcnh 0 --threads auto --reserve-cores 1 \
  --time-limit 3600 --output-dir results/campaign_full
```

The script still supports `original-modernized`, `low-risk` and `close-stack` presets. The `--relaxation full` override is intentionally opt-in. `--describe-parameters` prints parameters, supported combinations and cautions.

## Testing

```sh
./scripts/test.sh
```

The tests compare against the original `mosp.c`, the previous modern C++ solver, the new `off` and `full` modes, and an independent subset-DP oracle; they check contraction/rollback invariants. See `docs/TECHNICAL_REPORT.md` and `results/` for traceable evidence and limitations.

## Important reproducibility limitation

The provided eight 400–1000 instances were exercised under a **2-second per-instance pilot**, not a long production benchmark. All eight timed out in that pilot. Do not interpret the presence of a complete implementation as evidence of faster optimality proofs on large instances. The original full relaxation strategy can still benefit from further tuning and the incomplete-search heuristics of the 2009 code.

## Step 2 — verified MCNh warm-start certificates (2026-10-28)

This version closes the unsafe value-only warm-start path in **all** B&B modes
(`off`, `root-merge-probe`, `full`). A verified warm start is a **permutation of
original CS products (matrix columns)**. Customer stacks are the matrix rows.
The historical MCNh standalone representation instead sequences rows and
minimizes simultaneous column stacks; therefore **for the B&B warm start**
MCNh must be executed with `--objective-orientation customer`, which transposes
its internal heuristic representation before running its algorithm. The default
`--objective-orientation piece` retains standalone MCNh compatibility but its
warm-start output is intentionally rejected by the B&B.

The wrapper `scripts/run_batch.py` now passes the required orientation automatically.
The B&B re-evaluates each supplied original-instance product permutation twice:
using inclusive first/last occurrence intervals and a separate streaming
open/close counter. The declared `value`, if required by policy, must agree.
Nonpermutations, mismatched 0/1-based sequences, incompatible dimensions,
wrong sequence/objective domains, and unverified numeric-only values fail closed.

Policies:

- `--warm-start-policy value` (default): declared `value` must exist and match both
  verified objective evaluations.
- `--warm-start-policy strict`: same strict certificate checks as `value`.
- `--warm-start-policy sequence`: accept the *recalculated* objective of the
  verified product permutation and ignore any inconsistent declared scalar.

An imported product sequence is **not** silently repurposed as an internal
Chu–Stuckey customer-closure sequence. `--sequence-evaluator native|faggioli|both`
does not disable mandatory dual verification. Scalar `--upper` is ignored without
an original-instance witness in all modes. The old fields remain for compatibility;
new JSON/text fields include `warm_start_verified`,
`warm_start_validation_status`, `warm_start_declared_value`,
`warm_start_verified_value`, `warm_start_sequence_domain`, and
`unverified_upper_ignored`.

```bash
./mcnh_modern --input instances/A_AP-9.cs --input-format cs \
  --presence legacy-one --preprocess 1 \
  --objective-orientation customer --warm-start-output /tmp/mcnh.warm

./csbb_modern --input instances/A_AP-9.cs --input-format cs \
  --presence legacy-one --warm-start-file /tmp/mcnh.warm \
  --warm-start-policy value --relaxation full --json
```

Run regression and certificate checks using `./scripts/test.sh`. Details are in
`docs/STEP2_VALIDATED_WARM_START_REPORT.md`.

## Step 3 — certified customer-state residual lower bounds (2026-10-28)

Two provably admissible lower bounds on the **unclosed customer** subproblem
are available: the minimum unavoidable peak at the next customer closure and
the degeneracy+1 of the residual **customer** co-occurrence graph, implemented
with a target-directed k-core test. Both can safely prune the original and
contracted DFS, and work in parallel. The `graph` automatic root lower bound
now uses the CUSTOMER graph, whereas historical PRODUCT graph metrics remain
informational and are not used to certify optimality.

Backward compatibility: `--residual-lb off` remains the default. Options:

- `--residual-lb off|next|core` (default `off`)
- `--residual-core-max-vertices N` (default 192, 0 = no cap)
- `--auto-lower-mode graph` (safe customer-graph root bound, opt-in)

New output counters include `residual_lb_calls`, `residual_lb_core_tests`,
`residual_lb_prunes`, `profile_residual_lb_seconds` and
`customer_graph_core_lower_bound`. In keeping with certification safeguards,
numeric-only `--lower` may not exceed a separately proven root lower bound;
unknown external assertions are rejected.

Read `docs/STEP3_RESIDUAL_LOWER_BOUNDS_REPORT.md` for mathematical proofs,
independent oracles, compatibility, validation data and performance cautions.

```sh
./csbb_modern --input instances/A_AP-9.cs --input-format cs \
  --auto-lower-mode graph --residual-lb core --residual-core-max-vertices 192 \
  --relaxation full --time-limit 3600 --json
```


## Step 4 — parallel exact proofs on contracted graphs (2026-10-28)

Use the same global thread count for contracted proofs and original-graph fallback:

```bash
./csbb_modern --input instances/A_AP-9.cs --input-format cs \
  --relaxation full --threads 8 --parallel-depth 1 \
  --time-limit 3600 --profile 1 --json
```

Or override independently:

```bash
./csbb_modern --input instances/A_AP-9.cs --input-format cs \
  --relaxation full --threads 8 --parallel-depth 1 \
  --relax-proof-threads 7 --relax-proof-depth 2 \
  --relax-proof-frontier-cap 8192 --time-limit 3600 --json
```

- `--relax-proof-threads N`: 0 (default) inherits `--threads`; 1 forces serial proof.
- `--relax-proof-depth D`: -1 (default) inherits `--parallel-depth`; 0 forces serial proof.
- `--relax-proof-frontier-cap N`: maximum number of frontier leaves per contracted graph (default 8192). When a split would overflow the cap, the **entire subtree remains a frontier leaf**, so the proof remains complete.
- `relaxed_proof_parallel_calls` counts contracted-graph proofs that actually launched at least two workers.
- `relaxed_proof_frontier_states` sums the generated frontier leaves.
- `relaxed_proof_max_workers` is the maximum worker count actually launched during a contracted proof (0 if no relaxed proof was attempted).
- `relax_proof_threads`, `relax_proof_depth`, and `relax_proof_frontier_cap` report the resolved configuration.

**Correctness contract:** a contracted graph is immutable for the lifetime of its worker threads; no unmerge/graph swap can occur until all threads join. Worker-local dominance tables and stack variables prevent shared-state races. The global node counter and the common deadline still apply. `TIME_LIMIT`, `NODE_LIMIT` and relaxation-budget expiration never establish infeasibility. A feasible contracted witness is lifted and validated against the original graph; only a completed infeasibility proof can raise a certified lower bound.

**Validation:** `python3 tests/test_parallel_relaxed_proofs.py` compares serial and parallel relaxed proofs against independent subset-DP on random small instances and known examples; tests multiple depths, residual bounds, invalid parameters, finite resource budgets, and a deliberately tiny frontier cap. `python3 tests/pilot_parallel_relaxed_proofs.py` executes a short non-conclusive diagnostic on four real instances (400–1000). See `docs/PARALLEL_RELAXED_PROOFS_REPORT.md`.

**Performance:** parallelism improves resource utilization where a contracted proof has multiple viable frontier states. The size of each frontier, rather than `--threads` alone, bounds effective concurrency. Speedups and better incumbent values on large instances are not guaranteed; wall-clock evaluations must use sufficiently long, repeated experiments on the target hardware.

## v5 — parallel close-stack branching (2026-10-28)

`--branching close-stack` (and the current `auto` alias) can now run with multiple workers through the **same bounded frontier task dispatcher** used for parallel customer search. The experimental close-stack layer is still a *feasible-witness* search; failure **always** falls back to the certified customer-search backbone, so it never fabricates an infeasibility certificate. `--branching customer` remains the default.

New optional switches (also in `scripts/run_batch.py`):

- `--closure-threads N`: `0` inherits `--threads` (default), `1` enforces serial close-stack.
- `--closure-parallel-depth D`: `-1` inherits `--parallel-depth` (default), `0` enforces serial close-stack.
- `--closure-frontier-cap N`: `8192` by default; a cap does not discard branches, it retains unsplit subtrees as jobs.

The new output metrics are `closure_parallel_calls`, `closure_frontier_states`, `closure_max_workers`, and the effective closure settings. These settings are ignored when using `--branching customer`.

For a description of implementation, correctness, regressions, and pilot limitations see [docs/PARALLEL_CLOSE_STACK_REPORT.md](docs/PARALLEL_CLOSE_STACK_REPORT.md). New regression suite: `python3 tests/test_parallel_close_stack.py`. Short smoke pilot: `python3 tests/pilot_parallel_close_stack.py`.

## Controlled ablation experiments (v5)

Reproducible paired experiments on one real FCS instance each of 400x400,
600x600, 800x800, and 1000x1000; common validated MCNh certificates,
automatic physical-core allocation, fully sequential execution and generated
comparison report:

```bash
python3 scripts/controlled_experiments.py --list-comparisons
python3 scripts/controlled_experiments.py --suite core --time-limit 3600 --repeats 3 --threads auto --output-dir results/controlled_1h
python3 scripts/controlled_report.py --output-dir results/controlled_1h
```

Outputs: `reports/report.md`, `reports/comparisons.csv`, `reports/by_configuration.csv`,
`reports/runs.csv`, individual logs, JSON runs, manifest with SHA-256 hashes.
See `docs/CONTROLLED_EXPERIMENTS_PROTOCOL.md`. Start with `--dry-run` and a
short pilot before long campaigns. `--suite extended` includes low-level ablations.

Convenience wrapper (time is per B&B run; a full 1h campaign can take many hours):

```sh
TIME_LIMIT=3600 REPEATS=1 SUITE=core ./scripts/run_controlled_1h.sh results/controlled_1h
```

A completely executed, deliberately short **72-run pilot** (16 comparisons × 4 dimensions)
with generated reports is included in `results/controlled_full_smoke/` as an automation
validation only, not a performance benchmark.

## v6 — Instrumentação de eficiência e progresso

O solver inclui contadores de nós por fase, tempo de CPU, RSS de pico e estatísticas por slot de trabalhador. O monitor `--progress-interval 30 --progress-file saida.csv` registra UB/LB/gap e progresso da prova ao longo do tempo. Os scripts de experimentos controlados e de execução em lote habilitam snapshots a cada 30 segundos por padrão e consolidam as métricas no relatório. Ver `docs/TELEMETRY_METRICS_REPORT.md` para definições, limitações e uso.
