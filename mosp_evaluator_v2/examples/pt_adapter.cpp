// Executable integration example, not the user's PT implementation.
#include "mosp/evaluator.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>

int main() {
    try {
        const auto instance = mosp::Instance::from_patterns(10,
            {{0, 1}, {1, 2}, {0, 2}, {3, 4}, {4, 5}, {3, 5},
             {6}, {6, 7}, {7, 8}, {8, 9}, {0, 9}, {2, 7}});
        std::mt19937 rng(5901);
        const std::vector<double> temperatures{0.1, 0.5, 2.0, 10.0};
        std::vector<mosp::IncrementalEvaluator> replicas;
        mosp::Evaluator oracle(instance);
        mosp::Permutation initial(instance->pattern_count()); std::iota(initial.begin(), initial.end(), 0);
        for (std::size_t i = 0; i < temperatures.size(); ++i) {
            std::shuffle(initial.begin(), initial.end(), rng);
            replicas.emplace_back(instance, initial);
        }
        int accepted = 0, rejected = 0, worsening_accepted = 0, exchanges = 0;
        auto uniform = [&] { return std::generate_canonical<double, 53>(rng); };
        for (int iteration = 0; iteration < 3000; ++iteration) {
            for (std::size_t r = 0; r < replicas.size(); ++r) {
                auto& state = replicas[r];
                const int n = static_cast<int>(state.solution().size());
                const mosp::Move move{static_cast<mosp::MoveKind>(rng() % 3),
                                     static_cast<int>(rng() % n), static_cast<int>(rng() % n)};
                const int proposed = state.evaluate(move);
                const int delta = proposed - state.value();
                if (delta <= 0 || uniform() < std::exp(-static_cast<double>(delta) / temperatures[r])) {
                    if (state.apply(move) != proposed) throw std::runtime_error("proposal/apply mismatch");
                    ++accepted;
                    if (delta > 0) ++worsening_accepted;
                } else {
                    ++rejected;
                }
                if (oracle(state.solution()) != state.value() || oracle.profile() != state.profile())
                    throw std::runtime_error("accepted configuration mismatch");
            }
            if (iteration % 10 == 0) {
                for (std::size_t r = 0; r + 1 < replicas.size(); ++r) {
                    const double log_ratio = (1.0 / temperatures[r] - 1.0 / temperatures[r + 1]) *
                                             (replicas[r].value() - replicas[r + 1].value());
                    if (log_ratio >= 0 || std::log(uniform()) < log_ratio) {
                        // Temperatures remain attached to slots; swap complete configurations.
                        std::swap(replicas[r], replicas[r + 1]);
                        ++exchanges;
                    }
                }
            }
        }
        for (auto& state : replicas)
            if (oracle(state.solution()) != state.value() || oracle.profile() != state.profile())
                throw std::runtime_error("exchange mismatch");
        if (!accepted || !rejected || !worsening_accepted || !exchanges)
            throw std::runtime_error("incomplete acceptance/exchange coverage");
        std::cout << "{\"status\":\"PASS\",\"replicas\":4,\"proposals\":12000,\"accepted\":"
                  << accepted << ",\"rejected\":" << rejected << ",\"worsening_accepted\":"
                  << worsening_accepted << ",\"exchanges\":" << exchanges << "}\n";
    } catch (const std::exception& e) {
        std::cerr << "PT example failed: " << e.what() << '\n';
        return 1;
    }
}
