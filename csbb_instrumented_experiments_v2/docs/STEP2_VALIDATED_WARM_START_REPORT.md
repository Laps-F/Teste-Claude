# Step 2 — Original-instance certification of MCNh upper bounds

Date: 2026-10-28. Project baseline: `csbb_full_relaxation_v1`.

## Critical issue: orientation of the two representations

The current Chu–Stuckey B&B interprets **CS matrix rows as customers/stacks** and
**columns as products/decisions to produce**. By contrast, the standalone MCNh
heuristic stores a matrix of **patterns x pieces** and evaluates the count of
open **column/piece stacks** when sequencing its rows. Consequently, the former
`--input-format cs` MCNh wrapper read the CS matrix with an incompatible
objective orientation. On square matrices this mismatch was easy to miss.

The revised MCNh has `--objective-orientation piece|customer`:
- `piece` (default) preserves MCNh standalone behavior;
- `customer` transposes the *internal* MCNh matrix, making its heuristic
  pattern order correspond to the original CS **product sequence**, and its
  open stacks correspond to the original CS **customer rows**.

The updated batch runner always supplies `--objective-orientation customer` to
MCNh. The B&B checks `sequence_domain products`, `objective_domain customers`,
`n_customers` and `n_products`. Legacy unlabelled warm files must be regenerated.

## Safety and validation contracts

1. Input certificates must contain all original product IDs exactly once.
2. If both `sequence_0_based` and `sequence_1_based` exist, they must coincide.
3. Two independent original-instance evaluators must compute the same MOSP
   score: first/last occurrence event sweep and per-product streaming open/close.
4. `value|strict` requires equality of declared and verified costs; `sequence`
   ignores inconsistent declared cost, using the certified sequence score.
5. Numeric-only warm starts fail with exit status 2. Legacy orientation metadata
   fails with exit status 2. Malformed or dimension-inconsistent files fail.
6. Imported product permutations never become customer-close permutations.
7. `--upper` without a witness is ignored in **all** relaxation modes. The
   trivial number-of-customers bound remains feasible. Full relaxation retains
   its independent verified greedy customer-closure fallback.
8. In the presence of a valid imported certificate, the B&B may still find
   better solutions or prove optimality, always retaining a valid UB.
9. `--lower` is still an externally asserted bound if supplied explicitly.
   A formal audit of *residual-state lower bounds* is reserved for Step 3.

## Test evidence

- 76/76 existing small original-v-modern exact regression cases passed,
  including customer and close-stack modes and relaxed-graph validation.
- 151 additional MCNh warm-start assertions passed: 30 random rectangular
  incidence matrices, MCNh with/without preprocessing, full/off B&B, DP oracle,
  value mismatches, wrong domain/dimension, duplicate/truncated permutations,
  numeric-only warm starts, 'sequence' recertification, explicit numeric upper,
  SCOOP input mapping and legacy warm format rejection.
- Reference `A_AP-9.cs`: certified MCNh product permutation cost **6**, and
  full B&B returned **UB=LB=5** (OPTIMAL), preserving the exact result.
- 400x400 synthetic smoke: MCNh certificate cost **70**, accepted with both
  evaluators agreeing. Full B&B with a 1-second time limit returned
  `TIME_LIMIT` with valid bounds, without treating the imported UB as a proof.
- Batch-runner smoke: successful run with `--preset low-risk --relaxation full`
  and correctly populated consolidated CSV/JSON warm-start fields.

These smoke checks establish software behavior and absence of observed
certificate failures in the tested cases, **not** speedup or full formal proof of
all source-code paths. The original relaxation's long-run scalability remains
an experimental question.

## Compatibility changes

- The exact B&B objective and its customer-search/relaxation/search algorithms
  were not changed by the warm-start certificate overhaul.
- Valid legacy *solutions* remain valid; **previous warm-start files without
  certified product permutation and orientation metadata are no longer imported**.
- Standalone MCNh remains compatible by default (`--objective-orientation piece`).
- The old `--warm-start-policy value` previously trusted a scalar declared by
  MCNh; it now enforces original-instance proof of feasibility before lowering
  the incumbent. This is a deliberate, safety-critical change.
- `--symmetry-break reverse-anchor` is not applied to imported product sequences.
- Imported product-order certificates are distinct from B&B customer-close
  orders; the reported `best_sequence_*` still denotes the latter when available.

## Reproducibility

Build: `./scripts/build.sh`  
Tests: `./scripts/test.sh`  
Individual: `python3 tests/test_warm_start_certificates.py`  
Batch: `python3 scripts/run_batch.py --describe-parameters`
