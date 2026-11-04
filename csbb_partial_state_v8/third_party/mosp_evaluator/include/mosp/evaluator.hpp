#ifndef MOSP_EVALUATOR_HPP
#define MOSP_EVALUATOR_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mosp {

using Permutation = std::vector<int>;
using PatternItems = std::vector<std::vector<int>>;
enum class MatrixAxis { PatternsInRows, PatternsInColumns };
enum class Presence { EqualToOne, Positive };
enum class Energy { MOSP, MMOSP };

struct Metrics {
    int max_open = 0;
    std::int64_t total_open = 0;
    std::size_t pattern_count = 0;
    double mmosp() const noexcept;
    double energy(Energy mode) const;
    bool operator==(const Metrics& other) const noexcept {
        return max_open == other.max_open && total_open == other.total_open &&
               pattern_count == other.pattern_count;
    }
    bool operator!=(const Metrics& other) const noexcept { return !(*this == other); }
};

// Immutable incidence data. Patterns are the objects that are sequenced;
// item IDs identify stacks. IDs and positions are always zero based.
class Instance {
public:
    using Ptr = std::shared_ptr<const Instance>;
    static Ptr from_patterns(std::size_t item_count, PatternItems patterns);
    // Row-major matrix; both axis and presence are explicit, never inferred.
    static Ptr from_dense(std::size_t rows, std::size_t columns,
                          const std::vector<int>& entries,
                          MatrixAxis axis, Presence presence);
    std::size_t pattern_count() const noexcept { return patterns_.size(); }
    std::size_t item_count() const noexcept { return item_count_; }
    std::size_t incidence_count() const noexcept { return incidence_count_; }
    std::size_t words_per_pattern() const noexcept { return words_; }
    const PatternItems& patterns() const noexcept { return patterns_; }
    double density() const noexcept;
private:
    Instance(std::size_t item_count, PatternItems patterns);
    std::size_t item_count_, words_, incidence_count_ = 0;
    PatternItems patterns_;
    std::vector<std::uint64_t> bits_;
    std::vector<int> occurrences_;
    friend class Evaluator;
    friend class IncrementalEvaluator;
};

// Standalone evaluator. evaluate() needs only a complete pattern permutation.
// Reuses working memory; has no dependency on an accepted PT configuration.
// Share Instance among threads, but use one evaluator object per thread.
class Evaluator {
public:
    explicit Evaluator(Instance::Ptr instance);
    int evaluate(const Permutation& solution);
    Metrics evaluate_metrics(const Permutation& solution);
    const Metrics& metrics() const noexcept { return metrics_; }
    int operator()(const Permutation& solution) { return evaluate(solution); }
    // Profile of the most recent successful call; empty before the first call.
    const std::vector<int>& profile() const noexcept { return profile_; }
    const Instance::Ptr& instance() const noexcept { return instance_; }
private:
    Instance::Ptr instance_;
    std::vector<int> remaining_, profile_;
    std::vector<unsigned char> started_, seen_;
    Metrics metrics_;
};

enum class MoveKind { Insert, Swap, Reverse };
struct Move {
    MoveKind kind;
    int from;
    int to;
    // Insert: the removed pattern ends at final position 'to'.
    // Reverse: both endpoints included, in either order.
    static Move insert(int from, int to) { return {MoveKind::Insert, from, to}; }
    static Move swap(int first, int second) { return {MoveKind::Swap, first, second}; }
    static Move reverse(int first, int last) { return {MoveKind::Reverse, first, last}; }
    Move inverse() const;
};

// Optional state for a reference (normally accepted) solution.
// evaluate(): exact candidate objective, no change to solution/profile/value.
// apply(): unconditional update, including ties and worsening moves.
// The full-solution overload validates a permutation and locates its changed
// interval; the Move overload constructs a valid permutation locally.
// Calls reuse private scratch memory, hence one object per concurrent caller.
class IncrementalEvaluator {
public:
    IncrementalEvaluator(Instance::Ptr instance, const Permutation& solution);
    int value() const noexcept { return tree_[1]; }
    Metrics metrics() const noexcept { return {value(), total_open_, solution_.size()}; }
    const Permutation& solution() const noexcept { return solution_; }
    const std::vector<int>& profile() const noexcept { return profile_; }
    const Instance::Ptr& instance() const noexcept { return instance_; }
    int evaluate(const Permutation& candidate);
    int evaluate(Move move);
    int apply(const Permutation& candidate);
    int apply(Move move);
    Metrics evaluate_metrics(const Permutation& candidate);
    Metrics evaluate_metrics(Move move);
    Metrics apply_metrics(const Permutation& candidate);
    Metrics apply_metrics(Move move);
    // Updates two profile positions; useful for scanning adjacent insertions.
    int apply_adjacent(int left_position);
private:
    using Word = std::uint64_t;
    Instance::Ptr instance_;
    Permutation solution_;
    std::vector<int> profile_, tree_;
    std::size_t base_ = 1;
    std::vector<Word> prefix_, suffix_;
    std::vector<unsigned char> seen_;
    Permutation candidate_interval_;
    std::vector<Word> temporary_suffix_, running_prefix_;
    std::vector<int> candidate_profile_;
    std::int64_t total_open_ = 0;

    int range_max(std::size_t first, std::size_t end) const noexcept;
    void update_tree(std::size_t first, std::size_t length) noexcept;
    bool prepare_solution(const Permutation& candidate,
                          std::size_t& first, std::size_t& last);
    void prepare_move(Move move, std::size_t& first, std::size_t& last);
    Metrics evaluate_interval(std::size_t first, std::size_t last);
    void commit_interval(std::size_t first, std::size_t last) noexcept;
    Metrics adjacent(std::size_t left, bool commit);
};

} // namespace mosp
#endif
