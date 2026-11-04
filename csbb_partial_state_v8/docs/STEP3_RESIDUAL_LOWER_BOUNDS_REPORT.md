# Step 3 — Certified residual-state lower bounds for Chu–Stuckey customer search

Date: 2026-10-28. Baseline: `csbb_validated_warmstart_v2`. This release does **not** change the default customer-search recursion (`--residual-lb off`); the bounds below are opt-in and work for original and contracted graphs.

## 1. Domain and state

The CS input has **customers/stacks** on rows and **products** on columns. Let `G=(C,E)` be the undirected **customer co-occurrence graph**: `c--d` iff customers `c,d` need a common product. Every `N[c]` in the implementation is a *closed neighborhood* containing `c`. This graph is different from the product graph, whose previously computed cardinality/degeneracy figures are **not** certified customer-stack bounds.

At a customer-closure state, `R` denotes customers not yet closed and `O` those ever opened by earlier closures. `A=O ∩ R` is the set of currently open stacks. Before any bound is evaluated, the existing `force_close` logic has removed all customers `c∈R` with `N[c] ⊆ O`; thus, the next non-free closure must choose some `c∈R` and incurs a peak

    |(O ∩ R) ∪ (N[c] \ O)| = |O ∩ R| + |N[c] \ O|.

The final equality follows because `N[c] \ O` contains only not-yet-opened customers, disjoint from `O ∩ R`.

## 2. Exact mathematical arguments

### Bound A — minimum compulsory first-closure peak

    L_next(R,O) = |O ∩ R| + min_{c∈R} |N[c] \ O|,

with `L_next(∅,O)=0`. Every completion must make its next non-forced closure by some `c∈R`. At that moment at least the indicated number of customer stacks is simultaneously open. Therefore `L_next ≤ OPT_RESIDUAL(R,O)`. The bound is valid for every reachable state and remains valid if the past trajectory already had a larger peak: the branch decision is safe whenever `L_next > k`.

### Bound B — induced-customer-graph degeneracy

Let `G[R]` denote the graph induced by unclosed customers `R`. Define

    d(G[R]) = max_{nonempty S⊆R} min_{c∈S} deg_{G[S]}(c).

For any `S`, take the *first customer in S to close* in a completion. Before it closes, every neighbor in `S` shares a product already processed with it, and none of those neighbors has closed yet. Hence all such neighbors and the customer itself are simultaneously open, giving

    OPT_RESIDUAL(R,O) ≥ d(G[R]) + 1.

This does not depend on the order in which other customers outside `S` close. Self-loops are ignored in the degree calculation. **This proof is for the CUSTOMER co-occurrence graph; substituting the PRODUCT graph is invalid.**

The implementation uses a target-directed equivalent: `G[R]` has a nonempty `k`-core iff `d(G[R]) ≥ k`. Therefore, if a nonempty `k`-core exists, `OPT_RESIDUAL ≥ k+1` and the current decision target `k` is infeasible. A queue-based peeling algorithm tests this efficiently without computing the exact degeneracy on every node.

The independent root bound `d(G[C])+1` is also certified. `--auto-lower-mode graph` now uses the maximum of this root bound and the historical `max(number of customers using a single product)`. The latter remains valid. `--auto-lower-mode model` continues to use the safe legacy value, retaining old product-graph fields strictly as diagnostics.

## 3. Implementation and flags

- `--residual-lb off` (default): original search, no residual pruning.
- `--residual-lb next`: evaluate `L_next` after forced closures and before hash lookup / move generation.
- `--residual-lb core`: use `L_next` and, if needed, the target-directed `k`-core test.
- `--residual-core-max-vertices 192` (default): apply the expensive core test only when `|R|≤192`; `0` disables this cap. The first-closure bound is **not** disabled by this threshold.
- `--auto-lower-mode graph`: enable the independent safe root CUSTOMER-graph core bound. Legacy default remains `--auto-lower-mode legacy`.

The residual bound is used by serial DFS, parallel frontier construction, worker DFS and the exact solver of the contracted graph in `--relaxation full`. It is **not** used to justify a failed experimental `close-stack` search: infeasibility continues to be certified by the customer-search fallback.

The previously supported `--lower` numeric assertion is now allowed only up to an internally proven root lower bound, preventing a user-supplied arbitrary number from becoming an invalid optimality certificate. Unverified scalar `--upper` and MCNh warm-start safeguards remain as in Step 2.

**Thread safety:** worker-bound counters are thread-local and merged after the worker joins. Per-node bound computation uses local arrays and does not mutate the graph. On a time/node limit, any unfinished check is inconclusive and never used as a global infeasibility certificate; the established global `lower_bound` remains the previously certified one.

New JSON/text fields:

    customer_graph_core_lower_bound
    residual_lb_mode
    residual_lb_calls
    residual_lb_core_tests
    residual_lb_prunes
    profile_residual_lb_seconds

The batch runner forwards flags and retains these fields in `results.csv` and instance-level JSON.

## 4. Validation and compatibility

1. `tests/test_bound_operators.cpp`: **240** randomized co-occurrence graphs; **47,132** distinct residual states; **419,200** target-specific k-core checks, compared with independent exact customer-closure DP. All passed.
2. `tests/test_residual_bounds.py`: **129** matrices including adversarial and rectangular cases, **13,545** residual states assessed independently, **516** end-to-end B&B runs with modes `off`, `next`, `core`, and safe root `graph`. All passed.
3. `tests/run_regression.py`: **76** cases against the original Chu–Stuckey code and the modernized code in `off`, `full`, parallel, and `close-stack` modes; no objective or proof discrepancies.
4. `tests/test_warm_start_certificates.py`: **151** MCNh certificate assertions passed.
5. `tests/test_contractions.cpp`: **1,875** reversible contraction checks plus **1,875** minor-bound monotonicity checks passed.
6. 400/600 real instance pilot with pre-validated MCNh warm starts and 800/1000 smoke runs: output stayed structured and reported `TIME_LIMIT` and certified UB/LB/gap. Results are in `results/residual_scalability_pilot.csv` and test logs.
7. An invalid `--lower` asserted beyond the proven bound exits with status 2, preventing a false certificate.

Tests show absence of discrepancies in the tested conditions, **not** a mathematical proof that the entire solver implementation is bug-free. Bounds themselves have independent mathematical justifications above.

## 5. Performance caveat and recommendation

The real 400/600 pilot allotted **0.65 s per run**, too short to make statistically defensible conclusions about speed. Under this short budget, `next`/`core` sometimes pruned states but reduced the number of expanded nodes per second because every node pays for bound computation. In particular, the core test was often executed without producing extra pruning when the target `k` was much larger than the residual graph degeneracy.

**Default remains `--residual-lb off` for backward comparability.** To evaluate the new mechanisms, use:

    --auto-lower-mode graph --residual-lb next

and compare against:

    --auto-lower-mode graph --residual-lb core --residual-core-max-vertices 192

For 400–1000 instances, begin with wall-clock-limited A/B experiments and record node expansions, `residual_lb_prunes`, `residual_lb_core_tests`, `profile_residual_lb_seconds`, time to incumbent and time to proof. If the k-core check rarely prunes, keep it disabled or reduce its vertex threshold. The presence of a stronger *valid* bound does not automatically imply faster search.

## 6. Examples

```bash
./scripts/build.sh
./csbb_modern --input instances/A_AP-9.cs --input-format cs \
  --auto-lower-mode graph --residual-lb core --residual-core-max-vertices 192 \
  --relaxation full --threads 1 --time-limit 3600 --profile 1 --json
```

For batch mode, retain the three presets and override only the new flags:

```bash
python3 scripts/run_batch.py \
  --instances-dir instances/frinhani_carvalho_soma/400 --file-glob '*.txt' \
  --solver ./csbb_modern --mcnh ./mcnh_modern --preset low-risk \
  --auto-lower-mode graph --residual-lb next \
  --time-limit 3600 --threads auto --reserve-cores 1 \
  --output-dir results/step3_400
```
