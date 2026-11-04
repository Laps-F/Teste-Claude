# Full Chu–Stuckey relaxation: implementation, verification and performance report

Date: 2026-10-28.

## 1. Audit and source mapping

Original sources shipped verbatim in `original/mosp.c` and `original/solver.h` and hashed in `docs/BASELINE_SHA256.txt`. The previous modern implementation is preserved in `original/previous_modern.cpp`. The audit verified the following mapping:

| Original 2009 routine | Modern implementation | Notes |
|---|---|---|
| `Node::relax` | `RelaxationGraph::choose_original_pair`, `full_relaxation_decision` | Original reciprocal-degree/danger score and root merge-until-target policy |
| `Node::mergenodes` | `RelaxationGraph::merge` | Contract adjacent vertices; union neighborhoods and preserve graph symmetry |
| `Node::unmergenodes` | `RelaxationGraph::unmerge`, `rollback_to` | Undo log of rows changed by each merge, rather than global rollback copies |
| `Node::findunmerge` | `selective_unmerge` | Original-inspired backward selection of a merge whose removal breaks extension of the relaxed witness; alternate `lifo` mode |
| `Node::search` | `solve_contracted_exact` calling the modern `dfs` | New fresh hash/nogood table per contracted graph |
| `Node::extendsolution`, `getproductorder` | `extend_relaxed_order` plus original-graph `evaluate_pattern_order_as_customer_order` | Lifted witnesses are untrusted until evaluated against original graph |
| `astar` relaxation cycle | `full_relaxation_decision` within `decision_search` | Exact relaxed proof or progressive unmerge, followed by certified original-search fallback |

**Important methodological difference:** the original code uses a `close_open` first-pass search and special move pruning, followed by complete search if necessary; the new implementation uses a full exact customer-search subsolver on contracted graphs directly. `selective_unmerge` preserves the principle and search order of the original heuristic, but its reconstruction and tie behavior are not byte-for-byte identical to the 2009 code. This does not affect exactness of any certificate, and may affect speed.

## 2. Proof invariant and correctness safeguards

A connected-edge contraction of the undirected customer interaction graph produces a graph minor. The vertex separation/pathwidth parameter controlling Chu–Stuckey customer search is monotone under minor operations. For a valid contraction `G'` of original `G`:

`OPT(G') <= OPT(G)`.

For a **validated** original-graph incumbent `U`, infeasibility of target `k=U-1` on `G'` proves `OPT(G') >= U`, and thus `OPT(G)=U`. A `TIME_LIMIT`, `NODE_LIMIT`, `RelaxBudget` or resource interruption never certifies infeasibility.

Other safeguards:

- `merge(a,b)` requires an active adjacent pair; each changed adjacency row is undo-logged and restored exactly.
- All active rows maintain symmetry and self-loops. The contracted DFS counts only active customers.
- Nogood/dominance state is *never* reused between different contracted graphs.
- A witness on the contracted graph is extended in reverse merge order, then evaluated using the **original** customer graph. If infeasible, selective unmerge continues.
- Whenever no contracted proof is obtained, original-graph complete customer search is the only route to an infeasibility certificate.
- For finite `--time-limit`, `--relax-max-time-share` is a **global budget across all target values**, not a budget per target. The exact subsolver has a separate `RelaxBudget` status, which triggers fallback rather than termination.
- The full relaxation stage runs serially; requested parallelism is preserved on the fallback.
- Full mode derives a validated customer-order incumbent and rejects unverified value-only shortcuts. A separate lower bound with rigorous justification is used.

## 3. A preexisting lower-bound issue discovered during the audit

The previous modern code's `model_cardinality_lower_bound()` takes the largest **number of products ordered by one customer**. This is *not* a valid lower bound on maximum **simultaneously open customer stacks** in general. One customer ordering many products can still require only one open stack. Similarly, its `graph_degeneracy_lower_bound()` operates on a different (product) graph. Therefore neither should be silently trusted as a customer-graph optimality certificate. The large problem's dimensions being square does not make the dual representations equivalent.

This release corrects the issue **for all relaxation modes**: `--auto-lower-mode model` and `--auto-lower-mode graph` remain accepted as compatibility aliases but now apply only the proven `legacy_customer_lower_bound()` (largest number of customers that share a product). Their formerly computed metrics are logged strictly for diagnostics. The explicit `--lower N` option is still a user assertion in non-`full` modes and must come from a valid external proof; in `full`, it is ignored for certificates. This intentionally changes previously incorrect output in unsafe non-legacy modes, without altering valid solutions.

## 4. Verification performed

- **Baseline exact regression:** 76 randomized small binary CS instances, 0 mismatches versus independent subset-DP optimal values, original 2009 `mosp.c`, previous modern C++ version, modern `off`, modern `full`, parallel/close-stack full and `lifo` full.
- **Minor property test:** 320 random undirected graphs; 1,875 sequential connected contractions with independent subset-DP tests `OPT(minor) <= OPT(original)`; undo restoration of all logical adjacency rows after arbitrary contractions, 0 failures.
- **Reference example:** `A_AP-9.cs` returns `UB=LB=5` for `off`, `root-merge-probe`, and `full`; `full` obtained its final infeasibility certificate on the contracted graph.
- **Adversarial initial UB:** wrong value-only warm start (`value 1`), invalid small `--upper`, and inadmissible old `--auto-lower-mode graph` cannot cause false optimality in full mode: independently certified values remain correct.
- **Explicit LB counterexample:** `tests/invalid_graph_lb_counterexample.cs` has true optimum 2 but the previously advertised graph/model bound is 5. The previous implementation could return a false `OPTIMAL=5`. This release applies the valid lower bound 2 in **all** relaxation modes, including the `graph` compatibility alias.
- **Interruptions:** time/node limits preserve UB/LB/gap without treating unknown relaxation states as infeasible; global relax budget returns to original graph.
- **Sanitizers:** address/undefined-behavior sanitizer smoke test with full mode, parallel fallback and `branching auto` passed.
- **Batch integration:** `run_batch.py` dry-run and a real one-instance batch with `--relaxation full` completed; CSV/JSONL individual records generated.

Artifacts: `results/regression.json`, `results/real_benchmark/summary.csv`, `results/memory/`, per-instance JSON, and scripts/tests in this package.

## 5. Real instances: bounded two-second pilot

Eight supplied Frinhani–Carvalho–Soma random matrices (two per dimension: 400, 600, 800, 1000) were recovered from the prior project archive. A **shared and independently validated initial customer-order incumbent** was used in each triple of `off`, `root-merge-probe`, and `full` configurations. One thread, 2-second wall budget per instance, same hardware and executable; `full` used a global relaxation budget of 30%.

| Instance | Shared initial UB | Off UB | Root-probe UB | Full UB |
|---|---:|---:|---:|---:|
| 400-2-1 | 101 | 77 | 81 | 78 |
| 400-2-2 | 102 | 90 | 90 | 90 |
| 600-2-1 | 145 | 115 | 138 | 118 |
| 600-2-2 | 150 | 126 | 144 | 127 |
| 800-2-1 | 186 | 168 | 183 | 171 |
| 800-2-2 | 195 | 173 | 192 | 179 |
| 1000-2-1 | 259 | 245 | 257 | 251 |
| 1000-2-2 | 248 | 236 | 246 | 240 |

All runs ended with `TIME_LIMIT`; no optimality proof was obtained in the 2-second pilot and all full-mode runs performed certified-original fallback. **Full mode did not improve incumbent quality against `off`** in this short test: it tied in one instance and returned worse UB in seven. The original value of the full relaxation technique is mainly expected in *proof time*, which this short experiment cannot assess. The baseline should remain the default; full must remain opt-in until calibrated on longer proof-oriented runs.

**Memory smoke test (full, 0.5 second, one instance per dimension):** peak RSS approximately 14.7 MB (400), 38.4 MB (600), 72.3 MB (800), 136.5 MB (1000). This is a partial measurement, not an upper bound for long runs.

## 6. Remaining empirical work before setting default

The engineering stages 0–6 have executable validation; stage 7 was conducted as a **time-limited pilot**. A multi-hour, replicated experiment on actual target infrastructure remains necessary: 1h/6h budgets where feasible, identical validated incumbents, consistent CPU affinity, complete per-state proof metrics and a comparison against original Chu–Stuckey 2009 compiled on the same machine. Use the bundled batch script and consolidated result schema.

Recommended first calibration variables: `--relax-max-time-share` {0.05,0.10,0.20,0.30}, `--relax-max-merges` {16,32,64,128,0}, and `--relax-unmerge-policy` {original,lifo}. This set is a *proposed experiment*, not a proven best configuration.
