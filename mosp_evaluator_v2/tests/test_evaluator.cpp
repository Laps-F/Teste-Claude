#include "mosp/evaluator.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <future>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

using namespace mosp;

namespace {
std::uint64_t assertions = 0, exhaustive_cases = 0, random_cases = 0;
void need(bool ok, const std::string& what) {
    ++assertions;
    if (!ok) throw std::runtime_error(what);
}
// Independent oracle: explicitly find first and last occurrence of each item,
// then sum its inclusive interval. No bitsets or remaining-count update.
std::vector<int> oracle(const Instance& instance, const Permutation& solution) {
    const int n = static_cast<int>(solution.size());
    std::vector<int> first(instance.item_count(), n), last(instance.item_count(), -1);
    for (int t = 0; t < n; ++t)
        for (int x : instance.patterns()[static_cast<std::size_t>(solution[static_cast<std::size_t>(t)])]) {
            first[static_cast<std::size_t>(x)] = std::min(first[static_cast<std::size_t>(x)], t);
            last[static_cast<std::size_t>(x)] = t;
        }
    std::vector<int> diff(solution.size() + 1, 0), profile(solution.size(), 0);
    for (std::size_t x = 0; x < instance.item_count(); ++x)
        if (last[x] >= 0) {
            ++diff[static_cast<std::size_t>(first[x])];
            --diff[static_cast<std::size_t>(last[x] + 1)];
        }
    int active = 0;
    for (std::size_t t = 0; t < solution.size(); ++t) profile[t] = (active += diff[t]);
    return profile;
}
int maximum(const std::vector<int>& values) {
    return values.empty() ? 0 : *std::max_element(values.begin(), values.end());
}
Permutation moved(Permutation solution, Move move) {
    if (move.kind == MoveKind::Insert) {
        const int id = solution[static_cast<std::size_t>(move.from)];
        solution.erase(solution.begin() + move.from);
        solution.insert(solution.begin() + move.to, id);
    } else if (move.kind == MoveKind::Swap) {
        std::swap(solution[static_cast<std::size_t>(move.from)], solution[static_cast<std::size_t>(move.to)]);
    } else {
        std::reverse(solution.begin() + std::min(move.from, move.to),
                     solution.begin() + std::max(move.from, move.to) + 1);
    }
    return solution;
}
template<class F> void throws(F f, const char* what) {
    bool caught = false;
    try { f(); } catch (const std::exception&) { caught = true; }
    need(caught, what);
}
void state_ok(const IncrementalEvaluator& state, const Permutation& expected) {
    need(state.solution() == expected, "state permutation");
    const auto profile = oracle(*state.instance(), expected);
    need(state.profile() == profile, "state profile");
    need(state.value() == maximum(profile), "state objective");
    const auto sum = std::accumulate(profile.begin(), profile.end(), std::int64_t(0));
    need(state.metrics() == Metrics{maximum(profile), sum, expected.size()}, "state metrics");
}
void exhaustive(int m, int n) {
    for (int mask = 0; mask < (1 << (m * n)); ++mask) {
        PatternItems patterns(static_cast<std::size_t>(n));
        for (int j = 0; j < n; ++j)
            for (int x = 0; x < m; ++x)
                if (mask & (1 << (j * m + x))) patterns[static_cast<std::size_t>(j)].push_back(x);
        auto instance = Instance::from_patterns(static_cast<std::size_t>(m), patterns);
        Evaluator full(instance);
        Permutation p(static_cast<std::size_t>(n));
        std::iota(p.begin(), p.end(), 0);
        do {
            IncrementalEvaluator state(instance, p);
            state_ok(state, p);
            for (auto kind : {MoveKind::Insert, MoveKind::Swap, MoveKind::Reverse})
                for (int from = 0; from < n; ++from)
                    for (int to = 0; to < n; ++to) {
                        const Move move{kind, from, to};
                        const auto q = moved(p, move);
                        const auto profile = oracle(*instance, q);
                        const int expected = maximum(profile);
                        const Metrics expected_metrics{expected, std::accumulate(profile.begin(), profile.end(), std::int64_t(0)), q.size()};
                        need(full.evaluate_metrics(q) == expected_metrics, "full metrics oracle");
                        need(state.evaluate_metrics(move) == expected_metrics, "move metrics oracle");
                        need(state.evaluate_metrics(q) == expected_metrics, "solution metrics oracle");
                        need(full(q) == expected && full.profile() == profile, "full oracle");
                        need(state.evaluate(move) == expected, "move evaluation");
                        need(state.evaluate(q) == expected, "solution evaluation");
                        state_ok(state, p); // Both evaluations must preserve reference state.
                        need(state.apply(move) == expected, "move application");
                        state_ok(state, q);
                        state.apply(move.inverse());
                        state_ok(state, p);
                        need(state.apply(q) == expected, "solution application");
                        state_ok(state, q);
                        state.apply(p);
                        state_ok(state, p);
                        ++exhaustive_cases;
                    }
        } while (std::next_permutation(p.begin(), p.end()));
    }
}

void edge_cases() {
    for (std::size_t m : {std::size_t(0), std::size_t(65)}) {
        const auto instance = Instance::from_patterns(m, {});
        Evaluator full(instance);
        IncrementalEvaluator state(instance, {});
        need(full({}) == 0 && full.profile().empty(), "empty full");
        need(state.value() == 0 && state.evaluate(Permutation{}) == 0, "empty incremental");
        need(state.apply(Permutation{}) == 0, "empty application");
        throws([&] { state.evaluate(Move::swap(0, 0)); }, "empty move");
    }
    const auto singleton = Instance::from_patterns(1031, {{0, 63, 64, 65, 250, 1030}});
    Evaluator full_single(singleton);
    IncrementalEvaluator single(singleton, {0});
    need(full_single({0}) == 6 && single.value() == 6, "singleton inclusivity");
    need(single.apply(Move::reverse(0, 0)) == 6, "singleton noop");

    // Stale singleton-mask reproducer, with two independent instances alive.
    auto small = Instance::from_patterns(1, {{0}});
    auto instance = Instance::from_patterns(2, {{0}, {0}, {1}});
    Evaluator a(small), b(instance);
    IncrementalEvaluator state(instance, {0, 1, 2});
    need(a({0}) == 1 && b({0, 1, 2}) == 1, "two instances initialization");
    need(state.evaluate(Move::insert(1, 2)) == 2, "worsening evaluation");
    need(state.solution() == Permutation({0, 1, 2}), "rejection leaves reference intact");
    need(state.apply(Move::insert(1, 2)) == 2, "worsening accepted explicitly");
    need(state.profile() == Permutation({1, 2, 1}), "worsening profile");
    need(a({0}) == 1 && b({0, 2, 1}) == 2, "no instance cross contamination");

    const auto duplicate = Instance::from_patterns(2, {{1, 0, 1, 0}, {0}});
    need(duplicate->incidence_count() == 3, "binary incidence normalization");
    Evaluator d(duplicate);
    need(d({0, 1}) == 2, "duplicate incidence evaluation");

    // Non-square orientation, transpose, positive and exact-one semantics.
    const std::vector<int> matrix{1, 2, 0, -1, 0, 0, 1, 3, 0, 1, 1, 0, 1, 2, 0};
    std::vector<int> transpose(15);
    for (std::size_t r = 0; r < 3; ++r)
        for (std::size_t c = 0; c < 5; ++c) transpose[c * 3 + r] = matrix[r * 5 + c];
    for (auto presence : {Presence::EqualToOne, Presence::Positive}) {
        auto rows = Instance::from_dense(3, 5, matrix, MatrixAxis::PatternsInRows, presence);
        auto cols = Instance::from_dense(5, 3, transpose, MatrixAxis::PatternsInColumns, presence);
        need(rows->patterns() == cols->patterns() && rows->item_count() == 5, "transpose contract");
        Evaluator r(rows), c(cols);
        need(r({2, 0, 1}) == c({2, 0, 1}), "orientation energy");
        need(rows->incidence_count() == (presence == Presence::EqualToOne ? 5U : 8U), "presence semantics");
    }
    auto zero_rows = Instance::from_dense(0, 3, {}, MatrixAxis::PatternsInColumns, Presence::Positive);
    Evaluator zr(zero_rows);
    need(zr({2, 1, 0}) == 0, "zero rows nonzero patterns");

    for (int n : {1, 2, 3, 4, 7, 8, 9, 31, 32, 33, 64, 65}) {
        auto z = Instance::from_patterns(0, PatternItems(static_cast<std::size_t>(n)));
        Permutation order(static_cast<std::size_t>(n)); std::iota(order.begin(), order.end(), 0);
        IncrementalEvaluator zs(z, order);
        for (int i = 0; i + 1 < n; ++i) need(zs.apply_adjacent(i) == 0, "zero-item adjacent");
        state_ok(zs, zs.solution());
    }
    auto copy = state;
    copy.apply(Move::insert(2, 1));
    need(copy.value() == 1 && state.value() == 2, "copy state is independent");
    std::swap(copy, state);
    state_ok(state, {0, 1, 2}); state_ok(copy, {0, 2, 1});
    need(state.apply(state.solution()) == state.value(), "solution reference alias");

    throws([] { Evaluator bad(nullptr); }, "null full");
    throws([] { IncrementalEvaluator bad(nullptr, {}); }, "null state");
    throws([] { Instance::from_patterns(1, {{1}}); }, "bad item");
    throws([] { Instance::from_patterns(1, {{-1}}); }, "negative item");
    throws([] { Instance::from_patterns(std::numeric_limits<std::size_t>::max(), {}); }, "dimension overflow");
    throws([] { Instance::from_dense(2, 2, {1}, MatrixAxis::PatternsInRows, Presence::Positive); }, "matrix size");
    throws([] { Instance::from_dense(0, 0, {}, static_cast<MatrixAxis>(9), Presence::Positive); }, "matrix axis");
    throws([] { Instance::from_dense(0, 0, {}, MatrixAxis::PatternsInRows, static_cast<Presence>(9)); }, "presence enum");
    for (const Permutation& invalid : {Permutation{}, Permutation{0, 1}, Permutation{0, 0, 2},
                                      Permutation{-1, 1, 2}, Permutation{0, 1, 3}}) {
        throws([&] { b(invalid); }, "full invalid solution");
        throws([&] { state.evaluate(invalid); }, "incremental invalid solution");
        throws([&] { state.apply(invalid); }, "apply invalid solution");
        state_ok(state, {0, 1, 2});
    }
    for (const Move invalid : {Move::insert(-1, 1), Move::swap(0, 3), Move::reverse(0, INT_MAX),
                               Move{static_cast<MoveKind>(9), 0, 0}}) {
        throws([&] { state.evaluate(invalid); }, "evaluate invalid move");
        throws([&] { state.apply(invalid); }, "apply invalid move");
        state_ok(state, {0, 1, 2});
    }
    throws([&] { state.apply_adjacent(-1); }, "negative adjacent");
    throws([&] { state.apply_adjacent(2); }, "last adjacent");
}

void random_tests() {
    std::mt19937 rng(1701);
    for (int m : {0, 1, 63, 64, 65, 249, 250, 251, 1000, 1031})
        for (int trial = 0; trial < 10; ++trial) {
            const int n = 1 + static_cast<int>(rng() % 65);
            const double density = trial == 0 ? 0.0 : trial == 1 ? 1.0 : (trial - 1) * 0.1;
            PatternItems patterns(static_cast<std::size_t>(n));
            for (auto& pattern : patterns)
                for (int x = 0; x < m; ++x)
                    if (static_cast<double>(rng()) / rng.max() < density) pattern.push_back(x);
            auto instance = Instance::from_patterns(static_cast<std::size_t>(m), patterns);
            Evaluator full(instance);
            Permutation p(static_cast<std::size_t>(n)); std::iota(p.begin(), p.end(), 0);
            std::shuffle(p.begin(), p.end(), rng);
            IncrementalEvaluator state(instance, p);
            for (int iteration = 0; iteration < 400; ++iteration) {
                const auto original = state.solution();
                const Move move{static_cast<MoveKind>(rng() % 3), static_cast<int>(rng() % n),
                                 static_cast<int>(rng() % n)};
                auto q = moved(original, move);
                if (iteration % 11 == 0) std::shuffle(q.begin(), q.end(), rng);
                const auto profile = oracle(*instance, q);
                const int expected = maximum(profile);
                const Metrics expected_metrics{expected, std::accumulate(profile.begin(), profile.end(), std::int64_t(0)), q.size()};
                need(full.evaluate_metrics(q) == expected_metrics, "random full metrics");
                need(state.evaluate_metrics(q) == expected_metrics, "random solution metrics");
                if (iteration % 11 != 0) need(state.evaluate_metrics(move) == expected_metrics, "random move metrics");
                need(full(q) == expected && full.profile() == profile, "random full");
                need(state.evaluate(q) == expected, "random solution proposal");
                if (iteration % 11 != 0) need(state.evaluate(move) == expected, "random move proposal");
                state_ok(state, original);
                if (rng() % 2) {
                    const int applied = (iteration % 11 == 0 || iteration % 2) ? state.apply(q) : state.apply(move);
                    need(applied == expected, "random apply");
                    state_ok(state, q);
                } else {
                    state_ok(state, original);
                }
                if (n > 1 && iteration % 13 == 0) {
                    const int left = static_cast<int>(rng() % (n - 1));
                    const auto next = moved(state.solution(), Move::swap(left, left + 1));
                    state.apply_adjacent(left);
                    state_ok(state, next);
                }
                ++random_cases;
            }
        }
}

std::uint64_t parallel_tests() {
    const int n = 37, m = 1031;
    PatternItems patterns(n);
    for (int j = 0; j < n; ++j)
        for (int x = 0; x < m; ++x) if ((j * 11 + x * 3) % 17 < 4) patterns[j].push_back(x);
    const auto instance = Instance::from_patterns(m, patterns);
    std::vector<std::future<std::uint64_t>> futures;
    for (int worker = 0; worker < 8; ++worker) {
        futures.push_back(std::async(std::launch::async, [instance, worker] {
            std::mt19937 rng(static_cast<std::uint32_t>(2701 + worker));
            Permutation order(instance->pattern_count()); std::iota(order.begin(), order.end(), 0);
            IncrementalEvaluator state(instance, order);
            Evaluator full(instance);
            for (int i = 0; i < 1000; ++i) {
                const Move move{static_cast<MoveKind>(rng() % 3), static_cast<int>(rng() % order.size()),
                                 static_cast<int>(rng() % order.size())};
                const auto q = moved(state.solution(), move);
                const auto profile = oracle(*instance, q);
                const int expected = maximum(profile);
                const Metrics expected_metrics{expected, std::accumulate(profile.begin(), profile.end(), std::int64_t(0)), q.size()};
                if (state.evaluate_metrics(move) != expected_metrics || state.evaluate_metrics(q) != expected_metrics ||
                    full.evaluate_metrics(q) != expected_metrics) throw std::runtime_error("parallel metrics");
                if (state.evaluate(move) != expected || state.evaluate(q) != expected || full(q) != expected)
                    throw std::runtime_error("parallel evaluation");
                if (rng() % 2) {
                    if (state.apply(move) != expected || state.profile() != profile)
                        throw std::runtime_error("parallel application");
                }
            }
            return std::uint64_t(1000);
        }));
    }
    std::uint64_t cases = 0;
    for (auto& future : futures) cases += future.get();
    return cases;
}
} // namespace

int main() {
    try {
        edge_cases();
        exhaustive(2, 4);
        exhaustive(3, 3);
        random_tests();
        const auto parallel_cases = parallel_tests();
        std::cout << "{\"status\":\"PASS\",\"exhaustive_move_scenarios\":" << exhaustive_cases
                  << ",\"random_chained_scenarios\":" << random_cases
                  << ",\"parallel_scenarios\":" << parallel_cases
                  << ",\"parallel_workers\":8,\"serial_assertions\":" << assertions << "}\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
