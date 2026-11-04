#include "mosp/evaluator.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace mosp {
namespace {
std::size_t product(std::size_t a, std::size_t b) {
    if (b && a > std::numeric_limits<std::size_t>::max() / b)
        throw std::length_error("MOSP: dimensions overflow");
    return a * b;
}
void dimension(std::size_t value) {
    if (value > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("MOSP: item/pattern count exceeds int range");
}
void require_instance(const Instance::Ptr& p) {
    if (!p) throw std::invalid_argument("MOSP: null instance");
}
void validate(const Permutation& solution, std::size_t n,
              std::vector<unsigned char>& seen) {
    if (solution.size() != n)
        throw std::invalid_argument("MOSP: solution length differs from pattern count");
    seen.assign(n, 0);
    for (int id : solution) {
        if (id < 0 || static_cast<std::size_t>(id) >= n)
            throw std::invalid_argument("MOSP: pattern ID out of range");
        if (seen[static_cast<std::size_t>(id)])
            throw std::invalid_argument("MOSP: duplicate pattern ID");
        seen[static_cast<std::size_t>(id)] = 1;
    }
}
void check_move(Move move, std::size_t n) {
    switch (move.kind) {
        case MoveKind::Insert: case MoveKind::Swap: case MoveKind::Reverse: break;
        default: throw std::invalid_argument("MOSP: unknown move kind");
    }
    if (move.from < 0 || move.to < 0 ||
        static_cast<std::size_t>(move.from) >= n ||
        static_cast<std::size_t>(move.to) >= n)
        throw std::out_of_range("MOSP: move position out of range");
}
int popcount(std::uint64_t x) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(static_cast<unsigned long long>(x));
#else
    x -= (x >> 1) & UINT64_C(0x5555555555555555);
    x = (x & UINT64_C(0x3333333333333333)) + ((x >> 2) & UINT64_C(0x3333333333333333));
    x = (x + (x >> 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
    return static_cast<int>((x * UINT64_C(0x0101010101010101)) >> 56);
#endif
}
} // namespace

double Metrics::mmosp() const noexcept {
    if (max_open == 0 || pattern_count == 0) return 0.0;
    return static_cast<double>(max_open) + static_cast<double>(total_open) /
           (static_cast<double>(pattern_count) * static_cast<double>(max_open));
}

double Metrics::energy(Energy mode) const {
    if (mode == Energy::MOSP) return static_cast<double>(max_open);
    if (mode == Energy::MMOSP) return mmosp();
    throw std::invalid_argument("MOSP: unknown energy mode");
}

double Instance::density() const noexcept {
    if (patterns_.empty() || item_count_ == 0) return 0.0;
    return static_cast<double>(incidence_count_) /
           (static_cast<double>(patterns_.size()) * static_cast<double>(item_count_));
}

Instance::Instance(std::size_t item_count, PatternItems patterns)
    : item_count_(item_count), words_(item_count / 64 + (item_count % 64 != 0)),
      patterns_(std::move(patterns)) {
    dimension(item_count_);
    dimension(patterns_.size());
    // Validate dimensions before any dimension-dependent allocation.
    bits_.assign(product(patterns_.size(), words_), 0);
    occurrences_.assign(item_count_, 0);
    for (std::size_t j = 0; j < patterns_.size(); ++j) {
        auto& items = patterns_[j];
        for (int id : items)
            if (id < 0 || static_cast<std::size_t>(id) >= item_count_)
                throw std::invalid_argument("MOSP: item ID out of range");
        std::sort(items.begin(), items.end());
        items.erase(std::unique(items.begin(), items.end()), items.end());
        for (int id : items) {
            const auto x = static_cast<std::size_t>(id);
            bits_[j * words_ + x / 64] |= std::uint64_t(1) << (x % 64);
            ++occurrences_[x];
        }
        if (items.size() > std::numeric_limits<std::size_t>::max() - incidence_count_)
            throw std::length_error("MOSP: incidence count overflow");
        incidence_count_ += items.size();
    }
}

Instance::Ptr Instance::from_patterns(std::size_t item_count, PatternItems patterns) {
    return Ptr(new Instance(item_count, std::move(patterns)));
}

Instance::Ptr Instance::from_dense(std::size_t rows, std::size_t columns,
                                 const std::vector<int>& entries,
                                 MatrixAxis axis, Presence presence) {
    dimension(rows); dimension(columns);
    if (entries.size() != product(rows, columns))
        throw std::invalid_argument("MOSP: dense matrix size mismatch");
    if (axis != MatrixAxis::PatternsInRows && axis != MatrixAxis::PatternsInColumns)
        throw std::invalid_argument("MOSP: unknown matrix axis");
    if (presence != Presence::EqualToOne && presence != Presence::Positive)
        throw std::invalid_argument("MOSP: unknown presence rule");
    const bool in_rows = axis == MatrixAxis::PatternsInRows;
    PatternItems patterns(in_rows ? rows : columns);
    for (std::size_t row = 0; row < rows; ++row)
        for (std::size_t col = 0; col < columns; ++col) {
            const int value = entries[row * columns + col];
            if (presence == Presence::EqualToOne ? value == 1 : value > 0)
                patterns[in_rows ? row : col].push_back(static_cast<int>(in_rows ? col : row));
        }
    return from_patterns(in_rows ? columns : rows, std::move(patterns));
}

Evaluator::Evaluator(Instance::Ptr instance) : instance_(std::move(instance)) {
    require_instance(instance_);
    remaining_.resize(instance_->item_count());
    started_.resize(instance_->item_count());
    seen_.resize(instance_->pattern_count());
    profile_.reserve(instance_->pattern_count());
}

Metrics Evaluator::evaluate_metrics(const Permutation& solution) {
    validate(solution, instance_->pattern_count(), seen_);
    remaining_ = instance_->occurrences_;
    std::fill(started_.begin(), started_.end(), 0);
    profile_.resize(solution.size());
    int active = 0, result = 0;
    std::int64_t sum = 0;
    for (std::size_t position = 0; position < solution.size(); ++position) {
        const auto& items = instance_->patterns_[static_cast<std::size_t>(solution[position])];
        for (int id : items) {
            const auto x = static_cast<std::size_t>(id);
            if (!started_[x]) { started_[x] = 1; ++active; }
        }
        // Inclusive endpoints: a singleton is counted at its only position.
        profile_[position] = active;
        sum += active;
        result = std::max(result, active);
        for (int id : items)
            if (--remaining_[static_cast<std::size_t>(id)] == 0) --active;
    }
    metrics_ = {result, sum, solution.size()};
    return metrics_;
}

int Evaluator::evaluate(const Permutation& solution) {
    return evaluate_metrics(solution).max_open;
}

Move Move::inverse() const {
    if (kind == MoveKind::Insert) return insert(to, from);
    if (kind == MoveKind::Swap || kind == MoveKind::Reverse) return *this;
    throw std::invalid_argument("MOSP: unknown move kind");
}

IncrementalEvaluator::IncrementalEvaluator(Instance::Ptr instance,
                                         const Permutation& solution)
    : instance_(std::move(instance)) {
    require_instance(instance_);
    const auto n = instance_->pattern_count(), k = instance_->words_;
    validate(solution, n, seen_);
    solution_ = solution;
    profile_.resize(n);
    prefix_.assign(product(n + 1, k), 0);
    suffix_.assign(product(n + 1, k), 0);
    while (base_ < n) {
        if (base_ > std::numeric_limits<std::size_t>::max() / 2)
            throw std::length_error("MOSP: segment tree size overflow");
        base_ *= 2;
    }
    tree_.assign(product(base_, 2), 0);
    for (std::size_t i = 0; i < n; ++i) {
        const auto id = static_cast<std::size_t>(solution_[i]);
        for (std::size_t w = 0; w < k; ++w)
            prefix_[(i + 1) * k + w] = prefix_[i * k + w] | instance_->bits_[id * k + w];
    }
    for (std::size_t i = n; i-- > 0;) {
        const auto id = static_cast<std::size_t>(solution_[i]);
        for (std::size_t w = 0; w < k; ++w)
            suffix_[i * k + w] = suffix_[(i + 1) * k + w] | instance_->bits_[id * k + w];
    }
    for (std::size_t i = 0; i < n; ++i) {
        int count = 0;
        for (std::size_t w = 0; w < k; ++w)
            count += popcount(prefix_[(i + 1) * k + w] & suffix_[i * k + w]);
        profile_[i] = count;
        total_open_ += count;
        tree_[base_ + i] = count;
    }
    for (std::size_t i = base_; i-- > 1;)
        tree_[i] = std::max(tree_[2 * i], tree_[2 * i + 1]);
}

int IncrementalEvaluator::range_max(std::size_t first, std::size_t end) const noexcept {
    int result = 0;
    for (first += base_, end += base_; first < end; first /= 2, end /= 2) {
        if (first & 1U) result = std::max(result, tree_[first++]);
        if (end & 1U) result = std::max(result, tree_[--end]);
    }
    return result;
}

void IncrementalEvaluator::update_tree(std::size_t first, std::size_t length) noexcept {
    if (!length) return;
    for (std::size_t i = first; i < first + length; ++i) tree_[base_ + i] = profile_[i];
    auto left = (base_ + first) / 2, right = (base_ + first + length - 1) / 2;
    while (left) {
        for (auto i = left; i <= right; ++i)
            tree_[i] = std::max(tree_[2 * i], tree_[2 * i + 1]);
        left /= 2; right /= 2;
    }
}

bool IncrementalEvaluator::prepare_solution(const Permutation& candidate,
                                          std::size_t& first, std::size_t& last) {
    const auto n = solution_.size();
    validate(candidate, n, seen_);
    first = 0;
    while (first < n && candidate[first] == solution_[first]) ++first;
    if (first == n) return false;
    last = n - 1;
    while (candidate[last] == solution_[last]) --last;
    candidate_interval_.assign(candidate.begin() + static_cast<std::ptrdiff_t>(first),
                               candidate.begin() + static_cast<std::ptrdiff_t>(last + 1));
    return true;
}

void IncrementalEvaluator::prepare_move(Move move, std::size_t& first, std::size_t& last) {
    check_move(move, solution_.size());
    first = static_cast<std::size_t>(std::min(move.from, move.to));
    last = static_cast<std::size_t>(std::max(move.from, move.to));
    candidate_interval_.assign(solution_.begin() + static_cast<std::ptrdiff_t>(first),
                               solution_.begin() + static_cast<std::ptrdiff_t>(last + 1));
    if (move.from == move.to) return;
    if (move.kind == MoveKind::Swap) {
        std::swap(candidate_interval_.front(), candidate_interval_.back());
    } else if (move.kind == MoveKind::Reverse) {
        std::reverse(candidate_interval_.begin(), candidate_interval_.end());
    } else if (move.from < move.to) {
        std::rotate(candidate_interval_.begin(), candidate_interval_.begin() + 1,
                    candidate_interval_.end());
    } else {
        std::rotate(candidate_interval_.begin(), candidate_interval_.end() - 1,
                    candidate_interval_.end());
    }
}

Metrics IncrementalEvaluator::evaluate_interval(std::size_t first, std::size_t last) {
    const auto length = last - first + 1, k = instance_->words_;
    temporary_suffix_.resize(product(length + 1, k));
    running_prefix_.resize(k);
    candidate_profile_.resize(length);
    for (std::size_t w = 0; w < k; ++w) {
        temporary_suffix_[length * k + w] = suffix_[(last + 1) * k + w];
        running_prefix_[w] = prefix_[first * k + w];
    }
    for (std::size_t i = length; i-- > 0;) {
        const auto id = static_cast<std::size_t>(candidate_interval_[i]);
        for (std::size_t w = 0; w < k; ++w)
            temporary_suffix_[i * k + w] = temporary_suffix_[(i + 1) * k + w] |
                                           instance_->bits_[id * k + w];
    }
    int result = std::max(range_max(0, first), range_max(last + 1, solution_.size()));
    std::int64_t sum = total_open_;
    for (std::size_t i = 0; i < length; ++i) {
        const auto id = static_cast<std::size_t>(candidate_interval_[i]);
        int count = 0;
        for (std::size_t w = 0; w < k; ++w) {
            running_prefix_[w] |= instance_->bits_[id * k + w];
            count += popcount(running_prefix_[w] & temporary_suffix_[i * k + w]);
        }
        candidate_profile_[i] = count;
        sum += static_cast<std::int64_t>(count) - profile_[first + i];
        result = std::max(result, count);
    }
    return {result, sum, solution_.size()};
}

void IncrementalEvaluator::commit_interval(std::size_t first, std::size_t last) noexcept {
    const auto length = last - first + 1, k = instance_->words_;
    for (std::size_t w = 0; w < k; ++w) running_prefix_[w] = prefix_[first * k + w];
    for (std::size_t i = 0; i < length; ++i) {
        const auto id = static_cast<std::size_t>(candidate_interval_[i]);
        solution_[first + i] = candidate_interval_[i];
        total_open_ += static_cast<std::int64_t>(candidate_profile_[i]) - profile_[first + i];
        profile_[first + i] = candidate_profile_[i];
        for (std::size_t w = 0; w < k; ++w) {
            running_prefix_[w] |= instance_->bits_[id * k + w];
            prefix_[(first + i + 1) * k + w] = running_prefix_[w];
            suffix_[(first + i) * k + w] = temporary_suffix_[i * k + w];
        }
    }
    update_tree(first, length);
}

Metrics IncrementalEvaluator::adjacent(std::size_t left, bool commit) {
    const auto k = instance_->words_;
    const auto a = static_cast<std::size_t>(solution_[left]);
    const auto b = static_cast<std::size_t>(solution_[left + 1]);
    int height_left = 0, height_right = 0;
    for (std::size_t w = 0; w < k; ++w) {
        const Word p = prefix_[left * k + w] | instance_->bits_[b * k + w];
        const Word s = instance_->bits_[a * k + w] | suffix_[(left + 2) * k + w];
        height_left += popcount(p & suffix_[left * k + w]);
        height_right += popcount(prefix_[(left + 2) * k + w] & s);
        if (commit) {
            prefix_[(left + 1) * k + w] = p;
            suffix_[(left + 1) * k + w] = s;
        }
    }
    if (commit) {
        total_open_ += static_cast<std::int64_t>(height_left) + height_right -
                       profile_[left] - profile_[left + 1];
        std::swap(solution_[left], solution_[left + 1]);
        profile_[left] = height_left; profile_[left + 1] = height_right;
        update_tree(left, 2);
        return metrics();
    }
    const auto sum = total_open_ + static_cast<std::int64_t>(height_left) + height_right -
                     profile_[left] - profile_[left + 1];
    return {std::max({height_left, height_right, range_max(0, left),
                     range_max(left + 2, solution_.size())}), sum, solution_.size()};
}

Metrics IncrementalEvaluator::evaluate_metrics(const Permutation& candidate) {
    std::size_t first = 0, last = 0;
    if (!prepare_solution(candidate, first, last)) return metrics();
    if (last == first + 1) return adjacent(first, false);
    return evaluate_interval(first, last);
}

Metrics IncrementalEvaluator::apply_metrics(const Permutation& candidate) {
    std::size_t first = 0, last = 0;
    if (!prepare_solution(candidate, first, last)) return metrics();
    if (last == first + 1) return adjacent(first, true);
    evaluate_interval(first, last);
    commit_interval(first, last);
    return metrics();
}

Metrics IncrementalEvaluator::evaluate_metrics(Move move) {
    check_move(move, solution_.size());
    if (move.from == move.to) return metrics();
    const auto first = static_cast<std::size_t>(std::min(move.from, move.to));
    const auto last = static_cast<std::size_t>(std::max(move.from, move.to));
    if (last == first + 1) return adjacent(first, false);
    std::size_t l = 0, r = 0;
    prepare_move(move, l, r);
    return evaluate_interval(l, r);
}

Metrics IncrementalEvaluator::apply_metrics(Move move) {
    check_move(move, solution_.size());
    if (move.from == move.to) return metrics();
    const auto first = static_cast<std::size_t>(std::min(move.from, move.to));
    const auto last = static_cast<std::size_t>(std::max(move.from, move.to));
    if (last == first + 1) return adjacent(first, true);
    std::size_t l = 0, r = 0;
    prepare_move(move, l, r);
    evaluate_interval(l, r);
    commit_interval(l, r);
    return metrics();
}

int IncrementalEvaluator::apply_adjacent(int left_position) {
    if (left_position < 0 || static_cast<std::size_t>(left_position) >= solution_.size() ||
        solution_.size() - static_cast<std::size_t>(left_position) < 2)
        throw std::out_of_range("MOSP: adjacent position out of range");
    return adjacent(static_cast<std::size_t>(left_position), true).max_open;
}

int IncrementalEvaluator::evaluate(const Permutation& candidate) {
    return evaluate_metrics(candidate).max_open;
}
int IncrementalEvaluator::evaluate(Move move) { return evaluate_metrics(move).max_open; }
int IncrementalEvaluator::apply(const Permutation& candidate) { return apply_metrics(candidate).max_open; }
int IncrementalEvaluator::apply(Move move) { return apply_metrics(move).max_open; }

} // namespace mosp
