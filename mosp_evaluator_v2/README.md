# MOSP Evaluator 2.0.0

A standalone C++17 library for exact evaluation of a pattern permutation in
the Minimization of Open Stacks Problem. It provides full and incremental
evaluation, an automatically calibrated dispatcher, MMOSP energy, and an
optional item-order decoder. The library has no dependency on a search
algorithm, PT API, OpenMP, external solver, or fixed bitset capacity.

Portuguese usage, parameters and design notes: `docs/GUIA_PT_BR.md`.
Executed checks and limits: `docs/VALIDACAO.md`.
Optional integration with the supplied PT-MOSP repository:
`adapters/pt_mosp/README.md`.

## Minimal use

```cpp
#include "mosp/evaluator.hpp"

auto instance = mosp::Instance::from_patterns(2, {{0}, {0}, {1}});
mosp::Evaluator evaluator(instance);
int mosp_value = evaluator.evaluate({0, 2, 1}); // 2; v1 API retained
auto result = evaluator.evaluate_metrics({0, 2, 1});
// result.max_open == 2, result.total_open == 4
double energy = result.energy(mosp::Energy::MMOSP); // 2 + 4/(3*2)
```

Each outer list is a sequenced pattern; its elements are item/stack IDs.
The instance is immutable and shared. IDs and positions are zero based.
A solution contains every pattern ID exactly once. Duplicate incidences
within a pattern are normalized to binary presence. Empty patterns,
absent items, singleton items and empty instances are supported.

For a flat row-major matrix, use `Instance::from_dense(rows, columns,
entries, MatrixAxis::PatternsInRows/PatternsInColumns,
Presence::EqualToOne/Positive)`. Matrix axis and presence are explicit;
there is no orientation inference from dataset names.

## A host-owned incremental/adaptive reference

```cpp
#include "mosp/adaptive.hpp"

mosp::Permutation current = {0, 1, 2};
auto plan = mosp::SelectionPlan::calibrate(instance, current);
mosp::AdaptiveEvaluator state(instance, current, mosp::Backend::Auto, plan);

auto move = mosp::Move::insert(1, 2);
auto proposed = state.evaluate(move); // Reference is unchanged.
double delta = proposed.energy(mosp::Energy::MMOSP)
             - state.metrics().energy(mosp::Energy::MMOSP);
if (host_accepts(delta)) {
    state.apply(move); // Unconditional, including worsening moves.
}
```

`evaluate(candidate_solution)` and `apply(candidate_solution)` accept a
complete candidate instead of a move. `Insert`, `Swap` and `Reverse` are
supported. For insertion, `to` is the final position of the removed
pattern. Reversal includes both endpoints, in either order.

`Backend::Full`, `Backend::Incremental` and `Backend::Auto` affect the
calculation strategy. `Energy::MOSP` and `Energy::MMOSP` affect the
scalar supplied to a search method. These choices are independent.
The returned `Metrics` always retains the integer MOSP objective and
the 64-bit profile sum. MMOSP is `M + S/(n*M)` for M>0 and zero for M=0.

`Evaluator` and `IncrementalEvaluator` retain their v1 integer methods;
new `evaluate_metrics`/`apply_metrics` methods expose both statistics.
Consumers must rebuild with the new headers and source/static library.

## Automatic choice

Calibration uses the incidence count/density, dimensions and bitset length
for a work estimate, followed by timed trials on the current hardware.
The frozen table distinguishes complete-solution input and each move
type, with calibrated interval lengths. Costs between measured lengths are
linearly interpolated; explicit tables with zero timings use literal bucket
choices. Timing includes prescribed accepts
and maintenance of the incremental reference, and full-candidate
materialization for move input. It uses a private RNG and alternates
the order of timed backends. Marginal gains below 5% favor the full path.

Calibration is a performance heuristic; it does not guarantee the
fastest backend for every workload. Decisions and timings are available
through `plan->decisions()`. Recorded decisions can be restored using
`SelectionPlan::from_decisions` for reproducible dispatch. No timing or
policy changes take place during the search.

Calibrate once and share the immutable plan among replicas of the same
`Instance::Ptr`. Use one mutable evaluator/decoder per concurrent caller.
The adaptive evaluator keeps its incremental state synchronized even
when a proposal was evaluated by the full path. If a plan selects only
full evaluation, no incremental replica state is allocated.

`examples/adaptive_pt.cpp` runs four replicas with all six backend/energy
combinations and checks identical trajectories across backends for a
fixed energy and seed. A PT can swap complete `AdaptiveEvaluator`
objects with configurations while leaving temperatures at their slots.
`AdaptiveEvaluator` is movable and not copyable. Create a new object for a
new replica; solution snapshots can store just the permutation and metrics.

## Optional item-order representation

```cpp
auto decoding = mosp::ItemOrderDecoder::Data::create(instance);
mosp::ItemOrderDecoder decoder(decoding);
const auto& patterns = decoder.decode(item_permutation);
auto proposed = state.evaluate(patterns);
```

The decoder emits empty patterns first, followed by nonempty patterns as
their final item is visited. Simultaneously completed patterns have
ascending IDs. An item-order move can induce a nonlocal pattern change;
it must be decoded before evaluation. Shared decoder data holds the
inverse incidence lists; each decoder owns only its reusable work buffers.

## Build and verify

```sh
make -j2
make check
make sanitize
```

GNU Make, `ar` and a C++17 compiler build the static library and examples.
Python 3.9+ is used only by scripts and CLI tests. The tested environment
is GCC 13.3.0 on Linux x86-64. On the current instrumented host, use
`ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 make sanitize`
because LeakSanitizer cannot enumerate process threads there.

Link `build/libmosp_evaluator.a` after your program objects, or compile
`src/evaluator.cpp` and `src/adaptive.cpp` with `-Iinclude`. Neither
threading libraries nor PT headers are needed by the core implementation.
The test programs use threads and the Makefile links them with `-pthread`.

## Standalone CLI

```sh
build/mosp-evaluate --matrix examples/matrix.txt \
  --solution examples/solution.txt --axis rows --presence one \
  --evaluator auto --energy mmosp --profile
```

The matrix file contains row count, column count and the integer entries
in row-major order. The solution file contains only IDs, without a size
header. `--index-base 1` converts input IDs to zero based indices.
`--sequence items` accepts an item order. Defaults are pattern order,
zero based indices, full evaluation and MOSP energy. Output JSON includes
raw MOSP, profile sum, MMOSP, selected energy, density and calibration time.

The CLI initializes its reference from the identity permutation. A single
CLI call includes initialization/calibration and is not an isolated
measurement of incremental evaluation speed.

## Reproduce performance pilots

```sh
python3 scripts/benchmark_v2.py
```

This compares all three backends with both input interfaces, three move
types and prescribed acceptance, using synthetic matrices. Raw timings,
medians and environment metadata are included under `results/`. Results
do not establish speed or solution-quality gains in a full PT run.

## Distribution and attribution

The newly written library and tooling are supplied under the MIT license
in `LICENSE`. Optional adapted PT-MOSP/PTAPI/MCNH source retains the
original authors' terms. The core does not incorporate the legacy
four-category delta-evaluation code or the PT implementation.

MMOSP reference: J. F. Gonçalves, M. G. C. Resende and M. D. Costa (2016),
*A biased random-key genetic algorithm for the minimization of open stacks
problem*, International Transactions in Operational Research 23, 25–46.
DOI: https://doi.org/10.1111/itor.12109.

The prefix/suffix core follows the independent simplification documented
in this project's earlier analysis. The library evaluates solutions; it
does not issue optimality certificates.
