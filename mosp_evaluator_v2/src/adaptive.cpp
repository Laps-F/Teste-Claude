#include "mosp/adaptive.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <random>
#include <stdexcept>

namespace mosp {
namespace {
void check_kind(InputKind kind) {
    if (kind != InputKind::Solution && kind != InputKind::Insert &&
        kind != InputKind::Swap && kind != InputKind::Reverse)
        throw std::invalid_argument("MOSP: unknown input kind");
}
void check_move(Move move, std::size_t n) {
    if (move.kind != MoveKind::Insert && move.kind != MoveKind::Swap && move.kind != MoveKind::Reverse)
        throw std::invalid_argument("MOSP: unknown move kind");
    if (move.from < 0 || move.to < 0 || static_cast<std::size_t>(move.from) >= n ||
        static_cast<std::size_t>(move.to) >= n)
        throw std::out_of_range("MOSP: move position out of range");
}
InputKind input_kind(MoveKind kind) {
    if (kind == MoveKind::Insert) return InputKind::Insert;
    if (kind == MoveKind::Swap) return InputKind::Swap;
    if (kind == MoveKind::Reverse) return InputKind::Reverse;
    throw std::invalid_argument("MOSP: unknown move kind");
}
void change(Permutation& p, Move move) {
    check_move(move, p.size());
    auto a = p.begin() + move.from, b = p.begin() + move.to;
    if (move.kind == MoveKind::Swap) std::iter_swap(a, b);
    else if (move.kind == MoveKind::Reverse) std::reverse(std::min(a, b), std::max(a, b) + 1);
    else if (a < b) std::rotate(a, a + 1, b + 1);
    else if (b < a) std::rotate(b, a, a + 1);
}
double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto mid = values.size() / 2;
    return values.size() % 2 ? values[mid] : (values[mid - 1] + values[mid]) / 2.0;
}
} // namespace

const char* backend_name(Backend backend) {
    if (backend == Backend::Full) return "full";
    if (backend == Backend::Incremental) return "incremental";
    if (backend == Backend::Auto) return "auto";
    throw std::invalid_argument("MOSP: unknown backend");
}
const char* energy_name(Energy mode) {
    if (mode == Energy::MOSP) return "mosp";
    if (mode == Energy::MMOSP) return "mmosp";
    throw std::invalid_argument("MOSP: unknown energy mode");
}
Backend parse_backend(const std::string& s) {
    if (s == "full") return Backend::Full;
    if (s == "incremental") return Backend::Incremental;
    if (s == "auto") return Backend::Auto;
    throw std::invalid_argument("MOSP: backend must be full, incremental or auto");
}
Energy parse_energy(const std::string& s) {
    if (s == "mosp") return Energy::MOSP;
    if (s == "mmosp") return Energy::MMOSP;
    throw std::invalid_argument("MOSP: energy must be mosp or mmosp");
}

SelectionPlan::Ptr SelectionPlan::from_decisions(Instance::Ptr instance, std::vector<Decision> decisions) {
    if (!instance) throw std::invalid_argument("MOSP: null instance in selection plan");
    auto result = std::shared_ptr<SelectionPlan>(new SelectionPlan(std::move(instance)));
    for (const auto& row : decisions) {
        check_kind(row.input);
        if (row.backend != Backend::Full && row.backend != Backend::Incremental)
            throw std::invalid_argument("MOSP: a selection decision must select a concrete backend");
        if (!row.max_interval || !std::isfinite(row.full_ns) || !std::isfinite(row.incremental_ns) ||
            row.full_ns < 0 || row.incremental_ns < 0)
            throw std::invalid_argument("MOSP: invalid selection interval/timing");
    }
    std::sort(decisions.begin(), decisions.end(), [](const Decision& a, const Decision& b) {
        return a.input < b.input || (a.input == b.input && a.max_interval < b.max_interval);
    });
    for (std::size_t i = 1; i < decisions.size(); ++i)
        if (decisions[i].input == decisions[i-1].input && decisions[i].max_interval == decisions[i-1].max_interval)
            throw std::invalid_argument("MOSP: duplicate selection bucket");
    for (auto kind : {InputKind::Solution, InputKind::Insert, InputKind::Swap, InputKind::Reverse}) {
        const auto it = std::find_if(decisions.rbegin(), decisions.rend(), [=](const Decision& d) { return d.input == kind; });
        if (it == decisions.rend() || it->max_interval < result->instance_->pattern_count())
            throw std::invalid_argument("MOSP: incomplete selection table");
    }
    result->decisions_ = std::move(decisions);
    return result;
}

Backend SelectionPlan::choose(InputKind input, std::size_t length) const {
    check_kind(input);
    if (length > instance_->pattern_count()) throw std::out_of_range("MOSP: interval exceeds pattern count");
    const Decision* previous = nullptr;
    for (const auto& row : decisions_) {
        if (row.input != input) continue;
        if (length == row.max_interval) return row.backend;
        if (length < row.max_interval) {
            // Explicit zero-timing tables retain literal bucket choices.
            if (previous && previous->full_ns > 0 && previous->incremental_ns > 0 &&
                row.full_ns > 0 && row.incremental_ns > 0) {
                const double fraction = static_cast<double>(length - previous->max_interval) /
                                        static_cast<double>(row.max_interval - previous->max_interval);
                const double full_ns = previous->full_ns + fraction * (row.full_ns - previous->full_ns);
                const double inc_ns = previous->incremental_ns + fraction * (row.incremental_ns - previous->incremental_ns);
                return inc_ns < full_ns * 0.95 ? Backend::Incremental : Backend::Full;
            }
            return row.backend;
        }
        previous = &row;
    }
    throw std::logic_error("MOSP: incomplete selection plan");
}
bool SelectionPlan::uses_incremental() const noexcept {
    return std::any_of(decisions_.begin(), decisions_.end(), [](const Decision& d) { return d.backend == Backend::Incremental; });
}

AdaptiveEvaluator::AdaptiveEvaluator(Instance::Ptr instance, const Permutation& initial,
                                     Backend backend, SelectionPlan::Ptr plan)
    : instance_(std::move(instance)), full_(instance_), backend_(backend), plan_(std::move(plan)) {
    backend_name(backend_); // Validate enum even when the instance is empty.
    current_ = full_.evaluate_metrics(initial);
    if (backend_ == Backend::Auto) {
        if (!plan_) plan_ = SelectionPlan::calibrate(instance_, initial);
        if (plan_->instance().get() != instance_.get())
            throw std::invalid_argument("MOSP: selection plan belongs to another instance");
    }
    if (backend_ == Backend::Incremental || (backend_ == Backend::Auto && plan_->uses_incremental()))
        incremental_.reset(new IncrementalEvaluator(instance_, initial));
    else accepted_ = initial;
    candidate_buffer_.reserve(initial.size());
}

const Permutation& AdaptiveEvaluator::solution() const noexcept {
    return incremental_ ? incremental_->solution() : accepted_;
}
Backend AdaptiveEvaluator::choose(InputKind input, std::size_t length) const {
    return backend_ == Backend::Auto ? plan_->choose(input, length) : backend_;
}
std::size_t AdaptiveEvaluator::changed_length(const Permutation& candidate) const {
    const auto& current = solution();
    if (candidate.size() != current.size()) throw std::invalid_argument("MOSP: candidate length mismatch");
    std::size_t left = 0, end = current.size();
    while (left < end && current[left] == candidate[left]) ++left;
    while (end > left && current[end - 1] == candidate[end - 1]) --end;
    return end - left;
}
void AdaptiveEvaluator::materialize(Move move) {
    check_move(move, solution().size());
    candidate_buffer_ = solution();
    change(candidate_buffer_, move);
}

Metrics AdaptiveEvaluator::evaluate(const Permutation& candidate) {
    const auto length = changed_length(candidate);
    if (!length) return current_;
    last_backend_ = choose(InputKind::Solution, length);
    if (last_backend_ == Backend::Incremental) {
        ++counts_.incremental;
        return incremental_->evaluate_metrics(candidate);
    }
    ++counts_.full;
    return full_.evaluate_metrics(candidate);
}
Metrics AdaptiveEvaluator::evaluate(Move move) {
    check_move(move, solution().size());
    if (move.from == move.to) return current_;
    const auto length = static_cast<std::size_t>(std::max(move.from, move.to) - std::min(move.from, move.to)) + 1;
    last_backend_ = choose(input_kind(move.kind), length);
    if (last_backend_ == Backend::Incremental) {
        ++counts_.incremental;
        return incremental_->evaluate_metrics(move);
    }
    materialize(move);
    ++counts_.full;
    return full_.evaluate_metrics(candidate_buffer_);
}
Metrics AdaptiveEvaluator::apply(const Permutation& candidate) {
    if (incremental_) current_ = incremental_->apply_metrics(candidate);
    else {
        current_ = full_.evaluate_metrics(candidate);
        accepted_ = candidate;
    }
    ++counts_.applications;
    return current_;
}
Metrics AdaptiveEvaluator::apply(Move move) {
    check_move(move, solution().size());
    if (incremental_) current_ = incremental_->apply_metrics(move);
    else {
        materialize(move);
        current_ = full_.evaluate_metrics(candidate_buffer_);
        accepted_ = candidate_buffer_;
    }
    ++counts_.applications;
    return current_;
}

SelectionPlan::Ptr SelectionPlan::calibrate(Instance::Ptr instance, const Permutation& initial,
                                          CalibrationOptions options) {
    if (!instance) throw std::invalid_argument("MOSP: null calibration instance");
    if (!options.samples_per_bucket || options.samples_per_bucket > 1024 ||
        !options.repetitions || options.repetitions > 31)
        throw std::invalid_argument("MOSP: invalid calibration sample/repetition count");
    Evaluator reference(instance);
    reference.evaluate(initial); // Validate before the trivial-case shortcut.
    const auto n = initial.size();
    const auto begin = std::chrono::steady_clock::now();
    std::vector<Decision> rows;
    if (n < 2 || instance->item_count() == 0 || instance->incidence_count() == 0) {
        for (auto input : {InputKind::Solution, InputKind::Insert, InputKind::Swap, InputKind::Reverse})
            rows.push_back({input, std::max<std::size_t>(1, n), Backend::Full});
        return from_decisions(instance, std::move(rows));
    }
    std::vector<std::size_t> lengths{2, std::min<std::size_t>(16, n), std::max<std::size_t>(2, n / 4), n};
    std::sort(lengths.begin(), lengths.end());
    lengths.erase(std::unique(lengths.begin(), lengths.end()), lengths.end());
    std::mt19937 rng(options.seed); // Never consumes the host search RNG.
    for (auto input : {InputKind::Solution, InputKind::Insert, InputKind::Swap, InputKind::Reverse}) {
        for (auto length : lengths) {
            std::vector<Move> moves;
            std::vector<Permutation> candidates;
            auto trajectory = initial;
            for (unsigned i = 0; i < options.samples_per_bucket; ++i) {
                const int left = static_cast<int>(rng() % (n - length + 1));
                const int right = left + static_cast<int>(length) - 1;
                auto kind = input == InputKind::Swap ? MoveKind::Swap :
                            input == InputKind::Reverse ? MoveKind::Reverse : MoveKind::Insert;
                Move move{kind, left, right};
                if (rng() % 2) std::swap(move.from, move.to);
                auto q = trajectory; change(q, move);
                moves.push_back(move); candidates.push_back(q);
                if (i % 2 == 0) trajectory = q;
            }
            std::vector<double> full_times, incremental_times;
            const double full_work = static_cast<double>(instance->incidence_count()) +
                                     static_cast<double>(n) + instance->item_count();
            const double incremental_work = static_cast<double>(length) *
                                            static_cast<double>(instance->words_per_pattern() + 1) +
                                            std::ceil(std::log2(static_cast<double>(n)));
            const bool full_first = full_work <= incremental_work;
            for (unsigned rep = 0; rep < options.repetitions; ++rep) {
                for (int pass = 0; pass < 2; ++pass) {
                    // Alternate order across repetitions to reduce timing bias.
                    const bool use_full = (pass + rep) % 2 == (full_first ? 0U : 1U);
                    AdaptiveEvaluator state(instance, initial, Backend::Incremental);
                    state.backend_ = use_full ? Backend::Full : Backend::Incremental;
                    // Full path also maintains incremental state on acceptance,
                    // matching a mixed dispatcher. Warm largest buffers first.
                    auto warm = initial; std::reverse(warm.begin(), warm.end());
                    state.incremental_->evaluate_metrics(warm);
                    auto tick = std::chrono::steady_clock::now();
                    std::uint64_t checksum = 0;
                    for (unsigned i = 0; i < options.samples_per_bucket; ++i) {
                        const auto result = input == InputKind::Solution ? state.evaluate(candidates[i]) : state.evaluate(moves[i]);
                        checksum += static_cast<std::uint64_t>(result.max_open) + static_cast<std::uint64_t>(result.total_open);
                        if (i % 2 == 0) {
                            if (input == InputKind::Solution) state.apply(candidates[i]);
                            else state.apply(moves[i]);
                        }
                    }
                    const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - tick).count();
                    volatile std::uint64_t retained = checksum;
                    (void)retained;
                    // Verification stays outside the timed section.
                    if (state.solution() != trajectory || state.metrics() != reference.evaluate_metrics(trajectory))
                        throw std::logic_error("MOSP: calibration state mismatch");
                    (use_full ? full_times : incremental_times).push_back(ns / options.samples_per_bucket);
                }
            }
            const double full_ns = median(full_times), inc_ns = median(incremental_times);
            // A modest margin avoids selecting extra state for marginal gains.
            rows.push_back({input, length, inc_ns < full_ns * 0.95 ? Backend::Incremental : Backend::Full,
                            full_ns, inc_ns});
        }
    }
    auto result = std::const_pointer_cast<SelectionPlan>(from_decisions(instance, std::move(rows)));
    result->seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    return result;
}

ItemOrderDecoder::Data::Data(Instance::Ptr instance) : instance_(std::move(instance)) {
    if (!instance_) throw std::invalid_argument("MOSP: null decoder instance");
    by_item_.resize(instance_->item_count());
    for (std::size_t j = 0; j < instance_->patterns().size(); ++j)
        for (int x : instance_->patterns()[j]) by_item_[static_cast<std::size_t>(x)].push_back(static_cast<int>(j));
}
ItemOrderDecoder::Data::Ptr ItemOrderDecoder::Data::create(Instance::Ptr instance) {
    return Ptr(new Data(std::move(instance)));
}
ItemOrderDecoder::ItemOrderDecoder(Data::Ptr data) : data_(std::move(data)) {
    if (!data_) throw std::invalid_argument("MOSP: null decoder data");
    seen_.resize(data_->instance_->item_count());
    remaining_.resize(data_->instance_->pattern_count());
    order_.reserve(data_->instance_->pattern_count());
}
const Permutation& ItemOrderDecoder::decode(const Permutation& item_order) {
    if (item_order.size() != data_->instance_->item_count())
        throw std::invalid_argument("MOSP: item order size mismatch");
    std::fill(seen_.begin(), seen_.end(), 0);
    for (int x : item_order) {
        if (x < 0 || static_cast<std::size_t>(x) >= seen_.size() || seen_[static_cast<std::size_t>(x)])
            throw std::invalid_argument("MOSP: invalid item permutation");
        seen_[static_cast<std::size_t>(x)] = 1;
    }
    order_.clear();
    const auto& patterns = data_->instance_->patterns();
    for (std::size_t j = 0; j < patterns.size(); ++j) {
        remaining_[j] = static_cast<int>(patterns[j].size());
        if (patterns[j].empty()) order_.push_back(static_cast<int>(j));
    }
    for (int x : item_order)
        for (int j : data_->by_item_[static_cast<std::size_t>(x)])
            if (--remaining_[static_cast<std::size_t>(j)] == 0) order_.push_back(j);
    return order_;
}

} // namespace mosp
