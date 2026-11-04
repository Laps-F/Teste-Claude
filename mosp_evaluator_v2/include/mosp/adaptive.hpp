#ifndef MOSP_ADAPTIVE_HPP
#define MOSP_ADAPTIVE_HPP

#include "evaluator.hpp"
#include <array>
#include <memory>
#include <string>

namespace mosp {

enum class Backend { Full, Incremental, Auto };
enum class InputKind { Solution, Insert, Swap, Reverse };
const char* backend_name(Backend backend);
const char* energy_name(Energy mode);
Backend parse_backend(const std::string& text);
Energy parse_energy(const std::string& text);

struct Decision {
    InputKind input;
    std::size_t max_interval;
    Backend backend;
    double full_ns = 0;
    double incremental_ns = 0;
};
struct CalibrationOptions {
    unsigned samples_per_bucket = 6;
    unsigned repetitions = 3;
    std::uint32_t seed = 20261029;
};

class SelectionPlan {
public:
    using Ptr = std::shared_ptr<const SelectionPlan>;
    static Ptr calibrate(Instance::Ptr instance, const Permutation& initial,
                         CalibrationOptions options = {});
    // Restore recorded choices or provide a reproducible explicit dispatch table.
    // Each input kind must cover interval lengths up to pattern_count().
    static Ptr from_decisions(Instance::Ptr instance, std::vector<Decision> decisions);
    Backend choose(InputKind input, std::size_t interval_length) const;
    const std::vector<Decision>& decisions() const noexcept { return decisions_; }
    double calibration_seconds() const noexcept { return seconds_; }
    double density() const noexcept { return instance_->density(); }
    const Instance::Ptr& instance() const noexcept { return instance_; }
    bool uses_incremental() const noexcept;
private:
    explicit SelectionPlan(Instance::Ptr instance) : instance_(std::move(instance)) {}
    Instance::Ptr instance_;
    std::vector<Decision> decisions_;
    double seconds_ = 0;
};

struct EvaluationCounts {
    std::uint64_t full = 0;
    std::uint64_t incremental = 0;
    std::uint64_t applications = 0;
};

// Host-owned reference solution, suitable for one PT replica. Energy is a
// separate policy: all backends return exactly the same integer metrics.
// evaluate() never accepts; apply() always accepts, regardless of objective.
class AdaptiveEvaluator {
public:
    AdaptiveEvaluator(Instance::Ptr instance, const Permutation& initial,
                      Backend backend = Backend::Auto, SelectionPlan::Ptr plan = nullptr);
    Metrics evaluate(const Permutation& candidate);
    Metrics evaluate(Move move);
    Metrics apply(const Permutation& candidate);
    Metrics apply(Move move);
    Metrics metrics() const noexcept { return current_; }
    const Permutation& solution() const noexcept;
    Backend policy() const noexcept { return backend_; }
    Backend last_backend() const noexcept { return last_backend_; }
    const EvaluationCounts& counts() const noexcept { return counts_; }
    const SelectionPlan::Ptr& plan() const noexcept { return plan_; }
private:
    Instance::Ptr instance_;
    Evaluator full_;
    std::unique_ptr<IncrementalEvaluator> incremental_;
    Permutation accepted_, candidate_buffer_;
    Backend backend_, last_backend_ = Backend::Full;
    SelectionPlan::Ptr plan_;
    Metrics current_;
    EvaluationCounts counts_;
    Backend choose(InputKind input, std::size_t length) const;
    std::size_t changed_length(const Permutation& candidate) const;
    void materialize(Move move);
    friend class SelectionPlan;
};

// Pattern completion decoder for item-order representations. This is optional
// and independent of any PT API. Empty patterns are emitted first, then each
// nonempty pattern when its last item is visited, in ascending pattern ID.
class ItemOrderDecoder {
public:
    class Data {
    public:
        using Ptr = std::shared_ptr<const Data>;
        static Ptr create(Instance::Ptr instance);
        const Instance::Ptr& instance() const noexcept { return instance_; }
    private:
        explicit Data(Instance::Ptr instance);
        Instance::Ptr instance_;
        std::vector<std::vector<int>> by_item_;
        friend class ItemOrderDecoder;
    };
    explicit ItemOrderDecoder(Data::Ptr data);
    const Permutation& decode(const Permutation& item_order);
private:
    Data::Ptr data_;
    std::vector<unsigned char> seen_;
    std::vector<int> remaining_;
    Permutation order_;
};

} // namespace mosp
#endif
