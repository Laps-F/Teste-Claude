#include "mosp/evaluator.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>

using namespace mosp;
namespace {
using Clock = std::chrono::steady_clock;
volatile long long sink = 0;
template<class F> double elapsed(F function) {
    const auto begin = Clock::now(); function();
    return std::chrono::duration<double>(Clock::now() - begin).count();
}
void require(bool ok) { if (!ok) throw std::runtime_error("benchmark correctness check failed"); }
Permutation moved(Permutation p, Move move) {
    if (move.kind == MoveKind::Insert) {
        const int id = p[static_cast<std::size_t>(move.from)];
        p.erase(p.begin() + move.from); p.insert(p.begin() + move.to, id);
    } else if (move.kind == MoveKind::Swap) {
        std::swap(p[static_cast<std::size_t>(move.from)], p[static_cast<std::size_t>(move.to)]);
    } else {
        std::reverse(p.begin() + std::min(move.from, move.to), p.begin() + std::max(move.from, move.to) + 1);
    }
    return p;
}
void row(const std::string& phase, int n, double density, const std::string& move,
         int distance, double fraction, int moves, int rep, const std::string& method,
         double seconds, long long checksum) {
    sink = checksum;
    std::cout << phase << ',' << n << ',' << n << ',' << density << ',' << move << ',' << distance
              << ',' << fraction << ',' << moves << ',' << rep << ',' << method << ','
              << seconds << ',' << checksum << '\n';
}
const char* move_name(MoveKind kind) {
    return kind == MoveKind::Insert ? "insert" : kind == MoveKind::Swap ? "swap" : "reverse";
}
void trial(Instance::Ptr instance, const Permutation& initial, double density, MoveKind kind,
           int distance, double fraction, const std::string& phase, std::mt19937& rng, int count) {
    const int n = static_cast<int>(initial.size());
    std::vector<Move> moves;
    std::vector<Permutation> candidates;
    std::vector<bool> accepted;
    auto current = initial;
    for (int i = 0; i < count; ++i) {
        int from = static_cast<int>(rng() % (n - distance)), to = from + distance;
        if (rng() % 2) std::swap(from, to);
        const Move move{kind, from, to};
        const auto candidate = moved(current, move);
        const bool accept = fraction == 1.0 || (fraction > 0 && static_cast<double>(rng()) / rng.max() < fraction);
        moves.push_back(move); candidates.push_back(candidate); accepted.push_back(accept);
        if (accept) current = candidate;
    }
    for (int rep = 1; rep <= 3; ++rep) {
        Evaluator full(instance);
        IncrementalEvaluator solution_state(instance, initial), move_state(instance, initial);
        // Warm memory to the maximum interval outside measurement.
        full(initial);
        auto warm = initial; std::reverse(warm.begin(), warm.end());
        solution_state.evaluate(warm); move_state.evaluate(Move::reverse(0, n - 1));
        long long full_checksum = 0, solution_checksum = 0, move_checksum = 0;
        const double full_time = elapsed([&] {
            for (const auto& candidate : candidates) full_checksum += full(candidate);
        });
        const double solution_time = elapsed([&] {
            for (int i = 0; i < count; ++i) {
                solution_checksum += solution_state.evaluate(candidates[static_cast<std::size_t>(i)]);
                if (accepted[static_cast<std::size_t>(i)]) solution_state.apply(candidates[static_cast<std::size_t>(i)]);
            }
        });
        const double move_time = elapsed([&] {
            for (int i = 0; i < count; ++i) {
                move_checksum += move_state.evaluate(moves[static_cast<std::size_t>(i)]);
                if (accepted[static_cast<std::size_t>(i)]) move_state.apply(moves[static_cast<std::size_t>(i)]);
            }
        });
        require(full_checksum == solution_checksum && full_checksum == move_checksum);
        require(solution_state.solution() == current && move_state.solution() == current);
        require(full(current) == solution_state.value() && full.profile() == solution_state.profile());
        require(full(current) == move_state.value() && full.profile() == move_state.profile());
        row(phase, n, density, move_name(kind), distance, fraction, count, rep,
            "full_solution", full_time, full_checksum);
        row(phase, n, density, move_name(kind), distance, fraction, count, rep,
            "incremental_solution", solution_time, solution_checksum);
        row(phase, n, density, move_name(kind), distance, fraction, count, rep,
            "incremental_move", move_time, move_checksum);
    }
}
} // namespace

int main() {
    try {
        std::mt19937 rng(20261029);
        std::cout << std::setprecision(12)
                  << "phase,patterns,items,density,move,distance,accept_fraction,moves,repetition,method,seconds,checksum\n";
        for (int n : {200, 1000}) for (double density : {0.02, 0.2, 0.8}) {
            PatternItems patterns(static_cast<std::size_t>(n));
            for (auto& pattern : patterns)
                for (int x = 0; x < n; ++x)
                    if (static_cast<double>(rng()) / rng.max() < density) pattern.push_back(x);
            const auto instance = Instance::from_patterns(static_cast<std::size_t>(n), std::move(patterns));
            Permutation initial(static_cast<std::size_t>(n)); std::iota(initial.begin(), initial.end(), 0);
            std::shuffle(initial.begin(), initial.end(), rng);
            for (int rep = 1; rep <= 3; ++rep) {
                long long full_sum = 0, incremental_sum = 0;
                const double full_time = elapsed([&] {
                    for (int i = 0; i < 32; ++i) { Evaluator e(instance); full_sum += e(initial); }
                });
                const double incremental_time = elapsed([&] {
                    for (int i = 0; i < 32; ++i) { IncrementalEvaluator e(instance, initial); incremental_sum += e.value(); }
                });
                require(full_sum == incremental_sum);
                row("initialization", n, density, "none", 0, 0.0, 32, rep, "full_solution", full_time, full_sum);
                row("initialization", n, density, "none", 0, 0.0, 32, rep, "incremental_move", incremental_time, incremental_sum);
            }
            for (auto kind : {MoveKind::Insert, MoveKind::Swap, MoveKind::Reverse})
                for (int distance : {1, n / 2})
                    trial(instance, initial, density, kind, distance, 0.0, "evaluate_only", rng, n == 200 ? 1000 : 300);
            // Acceptance is prescribed, independent of temperature/objective.
            // This isolates evaluate+apply overhead without claiming PT speedup.
            if (n == 1000 && density == 0.2)
                for (auto kind : {MoveKind::Insert, MoveKind::Swap, MoveKind::Reverse})
                    for (int distance : {1, n / 2})
                        for (double fraction : {0.5, 1.0})
                            trial(instance, initial, density, kind, distance, fraction, "accepted_chain", rng, 300);
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
