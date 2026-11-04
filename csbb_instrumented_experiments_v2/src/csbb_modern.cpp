/*
 * Modernized Chu--Stuckey-style branch-and-bound for MOSP.
 *
 * Task-6 optimization package:
 * - dynamic bitsets for instances beyond the original 256-customer limit;
 * - customer-search decision structure and dominance memoization;
 * - flat/open-addressing dominance table;
 * - conservative Chu--Stuckey-inspired root merge relaxation probe;
 * - MCNh warm-start support and refined warm-start reporting;
 * - optional time and node stopping criteria;
 * - upper bound, lower bound, active decision target, and optimality gap in all terminal statuses;
 * - controlled parallel frontier search;
 * - lightweight profiling instrumentation for search components;
 * - low-risk model-inspired enhancements: optional graph lower bound,
 *   independent Faggioli--Bentivoglio sequence evaluator, reverse-anchor
 *   warm-start canonicalization, canonical simultaneous closures, and
 *   optional stack-profile emission;
 * - optional exact-safe close-stack branching layer with customer-search fallback.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <condition_variable>
#include <sys/resource.h>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <cctype>
#include <utility>
#include <vector>

namespace csbb {

using Clock = std::chrono::steady_clock;

static inline std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

class Bitset {
public:
    Bitset() = default;
    explicit Bitset(int n_bits) { reset(n_bits); }

    void reset(int n_bits) {
        n_ = n_bits;
        words_.assign(static_cast<size_t>((n_bits + 63) / 64), 0ULL);
        trim();
    }

    int size() const noexcept { return n_; }
    int nwords() const noexcept { return static_cast<int>(words_.size()); }
    const std::vector<std::uint64_t>& words() const noexcept { return words_; }

    bool empty() const noexcept {
        for (auto w : words_) if (w) return false;
        return true;
    }

    void set(int i) noexcept { words_[static_cast<size_t>(i >> 6)] |= (1ULL << (i & 63)); }
    void clear(int i) noexcept { words_[static_cast<size_t>(i >> 6)] &= ~(1ULL << (i & 63)); }
    bool test(int i) const noexcept { return (words_[static_cast<size_t>(i >> 6)] >> (i & 63)) & 1ULL; }

    int count() const noexcept {
        int total = 0;
        for (std::uint64_t w : words_) total += __builtin_popcountll(w);
        return total;
    }

    bool subset_of(const Bitset& other) const noexcept {
        for (size_t i = 0; i < words_.size(); ++i) {
            if ((words_[i] & ~other.words_[i]) != 0ULL) return false;
        }
        return true;
    }

    void inplace_or(const Bitset& other) noexcept {
        for (size_t i = 0; i < words_.size(); ++i) words_[i] |= other.words_[i];
    }

    void inplace_and(const Bitset& other) noexcept {
        for (size_t i = 0; i < words_.size(); ++i) words_[i] &= other.words_[i];
    }

    void inplace_minus(const Bitset& other) noexcept {
        for (size_t i = 0; i < words_.size(); ++i) words_[i] &= ~other.words_[i];
        trim();
    }

    Bitset bit_or(const Bitset& other) const {
        Bitset r = *this;
        r.inplace_or(other);
        return r;
    }

    Bitset bit_and(const Bitset& other) const {
        Bitset r = *this;
        r.inplace_and(other);
        return r;
    }

    Bitset minus(const Bitset& other) const {
        Bitset r = *this;
        r.inplace_minus(other);
        return r;
    }

    bool operator==(const Bitset& other) const noexcept { return words_ == other.words_; }
    bool operator!=(const Bitset& other) const noexcept { return !(*this == other); }

    bool lex_less(const Bitset& other) const noexcept {
        for (int i = static_cast<int>(words_.size()) - 1; i >= 0; --i) {
            if (words_[static_cast<size_t>(i)] < other.words_[static_cast<size_t>(i)]) return true;
            if (words_[static_cast<size_t>(i)] > other.words_[static_cast<size_t>(i)]) return false;
        }
        return false;
    }

    template <class Fn>
    void for_each_set_bit(Fn&& fn) const {
        for (size_t widx = 0; widx < words_.size(); ++widx) {
            std::uint64_t w = words_[widx];
            while (w) {
                const int b = __builtin_ctzll(w);
                const int idx = static_cast<int>(widx * 64 + static_cast<size_t>(b));
                if (idx < n_) fn(idx);
                w &= (w - 1ULL);
            }
        }
    }

    std::uint64_t hash() const noexcept {
        std::uint64_t h = splitmix64(static_cast<std::uint64_t>(n_));
        for (size_t i = 0; i < words_.size(); ++i) {
            h ^= splitmix64(words_[i] + 0x9e3779b97f4a7c15ULL * (i + 1));
            h = (h << 7) | (h >> 57);
        }
        return h;
    }

private:
    int n_ = 0;
    std::vector<std::uint64_t> words_;

    void trim() noexcept {
        if (n_ <= 0 || words_.empty()) return;
        const int used = n_ & 63;
        if (used != 0) {
            const std::uint64_t mask = (1ULL << used) - 1ULL;
            words_.back() &= mask;
        }
    }
};

// STEP 3: admissible bounds for the residual CUSTOMER-CLOSURE problem.
// 'cross' is the undirected customer co-occurrence graph, with self loops.
// Lower bounds are on the maximum number of simultaneously open CUSTOMERS.
struct ResidualBounds {
    // With no freely closable customer left, every continuation must choose c in R.
    // The next peak is |O intersect R| + |N(c) minus O|. Its minimum is admissible.
    static int first_closure(const std::vector<Bitset>& cross,
                             const Bitset& opened, const Bitset& remaining) {
        if (remaining.empty()) return 0;
        const int active_open = opened.bit_and(remaining).count();
        int best_add = std::numeric_limits<int>::max();
        remaining.for_each_set_bit([&](int c) {
            const int add = cross[static_cast<size_t>(c)].minus(opened).count();
            best_add = std::min(best_add, add);
        });
        return active_open + best_add;
    }

    // Degeneracy+1 is a lower bound on customer-search MOSP.
    // Rather than compute the full degeneracy in every state, ask only whether
    // the residual induced graph has a non-empty k-core, which certifies LB>k.
    // Self loops do NOT count as neighbor degree.
    static bool core_exceeds_target(const std::vector<Bitset>& cross,
                                     const Bitset& remaining, int k) {
        const int n = remaining.size();
        const int active = remaining.count();
        if (active == 0 || active <= k || k < 1) return false;
        std::vector<int> degree(static_cast<size_t>(n), -1);
        std::vector<int> queue;
        queue.reserve(static_cast<size_t>(active));
        remaining.for_each_set_bit([&](int c) {
            degree[static_cast<size_t>(c)] = cross[static_cast<size_t>(c)].bit_and(remaining).count() - 1;
            if (degree[static_cast<size_t>(c)] < k) queue.push_back(c);
        });
        int surviving = active;
        for (size_t h = 0; h < queue.size(); ++h) {
            const int c = queue[h];
            if (degree[static_cast<size_t>(c)] == -1) continue;
            degree[static_cast<size_t>(c)] = -1;
            --surviving;
            cross[static_cast<size_t>(c)].for_each_set_bit([&](int v) {
                if (v != c && remaining.test(v) && degree[static_cast<size_t>(v)] >= 0) {
                    if (--degree[static_cast<size_t>(v)] == k - 1) queue.push_back(v);
                }
            });
        }
        return surviving != 0;
    }

    // Root diagnostic, or exact root LB when explicitly requested.
    static int degeneracy_plus_one(const std::vector<Bitset>& cross, const Bitset& active) {
        int lo = 1, hi = active.count();
        while (lo < hi) {
            const int mid = lo + (hi - lo + 1) / 2;
            if (core_exceeds_target(cross, active, mid-1)) lo = mid;
            else hi = mid - 1;
        }
        return lo;
    }
};

// Reversible graph contraction, following the adjacency update and merge
// selection policy of Chu & Stuckey (2009). Every row change is undo-logged.
// A contracted graph is a minor of the original customer graph; therefore
// its vertex-separation optimum is a valid lower bound for the original.
class RelaxationGraph {
public:
    struct MergeRecord {
        int removed = -1;
        int representative = -1;
        std::vector<std::pair<int, Bitset>> previous_rows;
    };
    explicit RelaxationGraph(const std::vector<Bitset>& original)
        : rows(original), active(static_cast<int>(original.size())) {
        for (int v = 0; v < active.size(); ++v) active.set(v);
    }
    std::vector<Bitset> rows;
    Bitset active;
    std::vector<MergeRecord> log;

    // Original Chu-Stuckey safety heuristic: choose a non-open vertex and
    // adjacent representative that minimizes danger after removing its worst
    // neighbor contribution. Root-level relaxation has no already-open nodes.
    std::pair<int,int> choose_original_pair(const std::function<bool()>& interrupted) const {
        const int n = active.size();
        std::vector<double> weight(static_cast<size_t>(n), 0.0);
        active.for_each_set_bit([&](int v) {
            weight[static_cast<size_t>(v)] = 1.0 / std::max(1, rows[v].count());
        });
        double safest = std::numeric_limits<double>::infinity();
        int best_a = -1, best_b = -1;
        bool abort = false;
        active.for_each_set_bit([&](int a) {
            if (abort) return;
            if (interrupted()) { abort = true; return; }
            double danger_sum = 0.0, weight_sum = 0.0, worst = -1.0;
            int b = -1;
            rows[a].for_each_set_bit([&](int j) {
                if (j == a || !active.test(j)) return;
                const double danger = weight[static_cast<size_t>(j)] * rows[a].minus(rows[j]).count();
                danger_sum += danger;
                weight_sum += weight[static_cast<size_t>(j)];
                if (danger > worst) { worst = danger; b = j; }
            });
            if (b < 0 || weight_sum <= 0) return;
            const double score = (danger_sum - worst) / weight_sum;
            if (score < safest) { safest = score; best_a = a; best_b = b; }
        });
        if (abort) return {-1,-1};
        return {best_a,best_b};
    }

    bool merge(int a, int b) {
        if (a < 0 || b < 0 || a == b || !active.test(a) || !active.test(b) || !rows[a].test(b)) return false;
        MergeRecord rec; rec.removed = a; rec.representative = b;
        const Bitset neighbors = rows[a];
        active.for_each_set_bit([&](int v) {
            if (v == a) return;
            if (v == b || rows[v].test(a) || neighbors.test(v)) {
                rec.previous_rows.emplace_back(v, rows[v]);
                rows[v].clear(a);
                if (neighbors.test(v)) rows[v].set(b);
                if (v == b) {
                    rows[b].inplace_or(neighbors);
                    rows[b].clear(a);
                    rows[b].set(b);
                }
            }
        });
        active.clear(a);
        log.push_back(std::move(rec));
        return true;
    }
    bool unmerge() {
        if (log.empty()) return false;
        MergeRecord rec = std::move(log.back()); log.pop_back();
        for (auto& row : rec.previous_rows) rows[row.first] = std::move(row.second);
        active.set(rec.removed);
        return true;
    }
    void rollback_to(size_t depth) {
        while (log.size() > depth) unmerge();
    }
    bool valid() const {
        bool ok = true;
        active.for_each_set_bit([&](int v) {
            if (!rows[v].test(v)) ok = false;
            active.for_each_set_bit([&](int u) {
                if (rows[v].test(u) != rows[u].test(v)) ok = false;
            });
        });
        return ok;
    }
};

struct BitsetHash {
    size_t operator()(const Bitset& b) const noexcept { return static_cast<size_t>(b.hash()); }
};

struct Options {
    std::string input_path;
    std::string input_format = "cs";          // cs | scoop
    std::string presence_mode = "positive";   // positive | legacy-one
    std::string warm_start_file;
    int explicit_upper = -1;
    int explicit_lower = -1;
    bool auto_lower_bound = true;
    std::string auto_lower_mode = "legacy";   // off | legacy | model | graph
    std::string residual_lb_mode = "off"; // off | next | core; off keeps legacy search
    int residual_core_max_vertices = 192; // skip expensive core test above this size (0 = no limit)
    bool use_hash = true;
    bool sort_moves = true;
    bool group_moves = true;
    bool quiet = false;
    bool output_json = false;
    std::string warm_start_policy = "value"; // verified value | verified sequence | strict
    std::string relaxation_mode = "off";      // off | root-merge-probe | full
    std::string relax_merge_policy = "original";
    std::string relax_unmerge_policy = "original";  // original | lifo
    int relax_max_merges = 0;                  // 0 => until active <= target
    double relax_max_time_share = 0.25;       // fraction of finite global time budget
    std::string branching_mode = "customer";   // customer | close-stack | auto
    bool canonical_stack_closure = true;       // canonicalize simultaneous closures
    std::string sequence_evaluator = "native"; // native | faggioli | both
    std::string symmetry_break = "off";        // off | reverse-anchor
    int anchor_pattern = -1;                   // 0-based; -1 means automatic
    std::string stack_profile_file;
    bool profile = false;
    // Progress snapshots are opt-in. No background monitoring when interval <= 0.
    double progress_interval_seconds = 0.0;
    std::string progress_file;

    double time_limit_seconds = 0.0;           // <=0 means unlimited
    std::uint64_t node_limit = 0;              // 0 means unlimited
    int threads = 1;
    int parallel_depth = 1;
    // Closure search uses the same frontier worker dispatcher as customer search.
    // 0 = inherit --threads, -1 = inherit --parallel-depth.
    int closure_threads = 0;
    int closure_parallel_depth = -1;
    int closure_frontier_cap = 8192;
    // 0 = inherit --threads; -1 = inherit --parallel-depth.
    int relax_proof_threads = 0;
    int relax_proof_depth = -1;
    int relax_proof_frontier_cap = 8192; // complete bounded frontier, not truncated
};

struct Instance {
    int n_patterns = 0; // customers in Chu--Stuckey terminology
    int n_pieces = 0;   // products in Chu--Stuckey terminology
    std::vector<std::vector<unsigned char>> pattern_piece; // pattern x piece, boolean
};

struct DecisionResult {
    enum class Status { Feasible, Infeasible, Timeout, NodeLimit, RelaxBudget } status = Status::Infeasible;
};

struct ProfileStats {
    double force_close_seconds = 0.0;
    double move_generation_seconds = 0.0;
    double hash_lookup_seconds = 0.0;
    double hash_insert_seconds = 0.0;
    double relaxation_probe_seconds = 0.0;
    double frontier_seconds = 0.0;
    double warm_start_seconds = 0.0;
    double residual_lb_seconds = 0.0;

    void add(const ProfileStats& other) noexcept {
        force_close_seconds += other.force_close_seconds;
        move_generation_seconds += other.move_generation_seconds;
        hash_lookup_seconds += other.hash_lookup_seconds;
        hash_insert_seconds += other.hash_insert_seconds;
        relaxation_probe_seconds += other.relaxation_probe_seconds;
        frontier_seconds += other.frontier_seconds;
        warm_start_seconds += other.warm_start_seconds;
        residual_lb_seconds += other.residual_lb_seconds;
    }
};

struct WorkerMetric {
    std::uint64_t nodes = 0;
    std::uint64_t tasks = 0;
    double active_seconds = 0.0;
    double available_seconds = 0.0; // time of dispatch window, including idle/termination
    void add(const WorkerMetric& x) {
        nodes += x.nodes; tasks += x.tasks;
        active_seconds += x.active_seconds;
        available_seconds += x.available_seconds;
    }
};

struct SolveResult {
    std::string status;
    int upper_bound = -1;
    int lower_bound = 1;
    int applied_initial_lower_bound = 1;
    int legacy_lower_bound = 1;
    int model_cardinality_lower_bound = 1;
    int graph_min_degree_lower_bound = 1;
    int graph_degeneracy_lower_bound = 1;
    int customer_graph_core_lower_bound = 1;
    std::string residual_lb_mode = "off";
    std::uint64_t residual_lb_calls = 0;
    std::uint64_t residual_lb_core_tests = 0;
    std::uint64_t residual_lb_prunes = 0;
    int active_target = -1;
    int last_completed_target = -1;
    double gap_percent = std::numeric_limits<double>::infinity();
    std::uint64_t nodes = 0;
    std::uint64_t hash_hits = 0;
    std::uint64_t hash_entries = 0;
    std::uint64_t dominance_keys = 0;
    double elapsed_seconds = 0.0;
    double cpu_seconds = 0.0;
    double peak_rss_mb = 0.0;
    std::uint64_t nodes_customer = 0, nodes_relaxed_proofs = 0;
    std::uint64_t nodes_close_stack = 0, nodes_frontier = 0;
    double nodes_per_second = 0.0;
    std::vector<WorkerMetric> workers_customer, workers_relaxed, workers_closure;
    double progress_interval_seconds = 0.0;
    std::string progress_file;

    int threads = 1;
    int parallel_depth = 0;
    std::uint64_t frontier_states = 0;
    std::uint64_t relaxation_probes = 0;
    std::uint64_t relaxation_successes = 0;
    std::uint64_t relaxation_merges = 0;
    std::string relaxed_proof_status = "NOT_RUN";
    std::string proof_source = "NONE";
    std::uint64_t relaxed_proofs_attempted = 0;
    std::uint64_t relaxed_proofs_completed = 0;
    std::uint64_t relaxed_proof_infeasibilities = 0;
    std::uint64_t relaxation_full_merges = 0;
    std::uint64_t relaxation_full_unmerges = 0;
    std::uint64_t relaxation_customers_at_proof = 0;
    double relaxation_full_seconds = 0.0;
    double relaxed_proof_seconds = 0.0;
    std::uint64_t relaxed_proof_parallel_calls = 0;
    std::uint64_t relaxed_proof_frontier_states = 0;
    int relaxed_proof_max_workers = 0;
    int relax_proof_threads = 1;
    int relax_proof_depth = 0;
    int relax_proof_frontier_cap = 8192;
    bool original_fallback_used = false;
    int full_verified_initial_upper_bound = -1;
    bool full_unverified_warm_ignored = false;
    bool full_explicit_upper_ignored = false;
    int warm_start_value = -1;
    int warm_start_sequence_value = -1;
    int warm_start_faggioli_value = -1;
    bool warm_start_evaluators_agree = true;
    int anchor_pattern = -1;
    bool reverse_anchor_applied = false;
    int best_sequence_faggioli_value = -1;
    std::string branching_mode = "customer";
    std::uint64_t closure_branch_calls = 0;
    std::uint64_t closure_branch_nodes = 0;
    std::uint64_t closure_branch_dead_states = 0;
    std::uint64_t closure_branch_successes = 0;
    std::uint64_t closure_branch_fallbacks = 0;
    std::uint64_t closure_parallel_calls = 0;
    std::uint64_t closure_frontier_states = 0;
    int closure_max_workers = 0;
    int closure_threads = 1;
    int closure_parallel_depth = 0;
    int closure_frontier_cap = 8192;
    bool warm_start_accepted = false;
    bool warm_start_sequence_accepted = false;
    bool warm_start_verified = false;
    std::string warm_start_validation_status = "NOT_PROVIDED";
    int warm_start_declared_value = -1;
    int warm_start_verified_value = -1;
    std::string warm_start_sequence_domain = "NONE";

    ProfileStats profile;
    std::vector<int> best_pattern_order;
};

class DominanceTable {
public:
    explicit DominanceTable(size_t initial_capacity = 1024) { reset_capacity(initial_capacity); }

    void clear() {
        reset_capacity(1024);
        entries_ = 0;
        occupied_ = 0;
    }

    bool dominated(const Bitset& p, const Bitset& q) {
        const Bucket* bucket = find_bucket(p);
        if (!bucket) return false;
        for (const Bitset& stored_q : bucket->qs) {
            if (stored_q.subset_of(q)) return true;
        }
        return false;
    }

    void insert(const Bitset& p, const Bitset& q) {
        ensure_capacity();
        Bucket& bucket = find_or_create_bucket(p);
        for (const Bitset& stored_q : bucket.qs) {
            if (stored_q.subset_of(q)) return;
        }
        size_t out = 0;
        for (size_t i = 0; i < bucket.qs.size(); ++i) {
            if (!q.subset_of(bucket.qs[i])) bucket.qs[out++] = std::move(bucket.qs[i]);
            else --entries_;
        }
        bucket.qs.resize(out);
        bucket.qs.push_back(q);
        ++entries_;
    }

    std::uint64_t entries() const noexcept { return entries_; }
    size_t keys() const noexcept { return occupied_; }
    size_t capacity() const noexcept { return buckets_.size(); }

private:
    struct Bucket {
        bool used = false;
        std::uint64_t hash = 0;
        Bitset key;
        std::vector<Bitset> qs;
    };

    std::vector<Bucket> buckets_;
    size_t occupied_ = 0;
    std::uint64_t entries_ = 0;

    static size_t next_power_of_two(size_t x) {
        size_t p = 1;
        while (p < x) p <<= 1;
        return p;
    }

    void reset_capacity(size_t cap) {
        buckets_.clear();
        buckets_.resize(next_power_of_two(std::max<size_t>(cap, 16)));
        occupied_ = 0;
    }

    size_t slot_index(std::uint64_t h) const noexcept {
        return static_cast<size_t>(h) & (buckets_.size() - 1);
    }

    const Bucket* find_bucket(const Bitset& p) const {
        if (buckets_.empty()) return nullptr;
        const std::uint64_t h = p.hash();
        size_t idx = slot_index(h);
        for (;;) {
            const Bucket& b = buckets_[idx];
            if (!b.used) return nullptr;
            if (b.hash == h && b.key == p) return &b;
            idx = (idx + 1) & (buckets_.size() - 1);
        }
    }

    Bucket& find_or_create_bucket(const Bitset& p) {
        const std::uint64_t h = p.hash();
        size_t idx = slot_index(h);
        for (;;) {
            Bucket& b = buckets_[idx];
            if (!b.used) {
                b.used = true;
                b.hash = h;
                b.key = p;
                b.qs.clear();
                ++occupied_;
                return b;
            }
            if (b.hash == h && b.key == p) return b;
            idx = (idx + 1) & (buckets_.size() - 1);
        }
    }

    void ensure_capacity() {
        if ((occupied_ + 1) * 10 < buckets_.size() * 7) return;
        rehash(buckets_.empty() ? 1024 : buckets_.size() * 2);
    }

    void rehash(size_t new_cap) {
        std::vector<Bucket> old = std::move(buckets_);
        reset_capacity(new_cap);
        for (Bucket& b : old) {
            if (!b.used) continue;
            Bucket& nb = find_or_create_bucket(b.key);
            nb.qs = std::move(b.qs);
        }
    }
};

class Solver {
public:
    explicit Solver(Options options) : opt_(std::move(options)) {
        if (opt_.threads < 1) opt_.threads = 1;
        if (opt_.parallel_depth < 0) opt_.parallel_depth = 0;
    }

    int effective_closure_threads() const {
        return opt_.closure_threads > 0 ? opt_.closure_threads : opt_.threads;
    }

    int effective_closure_parallel_depth() const {
        return opt_.closure_parallel_depth >= 0 ? opt_.closure_parallel_depth : opt_.parallel_depth;
    }

    int effective_relax_proof_threads() const {
        return opt_.relax_proof_threads > 0 ? opt_.relax_proof_threads : opt_.threads;
    }

    int effective_relax_proof_depth() const {
        return opt_.relax_proof_depth >= 0 ? opt_.relax_proof_depth : opt_.parallel_depth;
    }

    // Separate inexpensive atomic counters from proof-critical state. The monitor
    // NEVER reads mutable decision structures or worker hash tables.
    void publish_bounds() {
        published_ub_.store(incumbent_, std::memory_order_relaxed);
        published_lb_.store(certified_lb_, std::memory_order_relaxed);
    }

    class ProgressMonitor {
    public:
        explicit ProgressMonitor(Solver& solver) : solver_(solver) {
            if (solver_.opt_.progress_interval_seconds <= 0.0) return;
            stream_.open(solver_.opt_.progress_file);
            if (!stream_) throw std::runtime_error("Unable to create progress CSV: " + solver_.opt_.progress_file);
            stream_ << "time_seconds,nodes_total,nodes_customer,nodes_relaxed_proofs,nodes_close_stack,nodes_frontier,upper_bound,lower_bound,gap_percent,relaxed_proofs_completed\n";
            sample();
            thread_ = std::thread([this] {
                std::unique_lock<std::mutex> lock(mutex_);
                while (!stopped_) {
                    if (cv_.wait_for(lock, std::chrono::duration<double>(solver_.opt_.progress_interval_seconds), [this]{return stopped_;})) break;
                    sample();
                }
            });
        }
        ProgressMonitor(const ProgressMonitor&) = delete;
        ProgressMonitor& operator=(const ProgressMonitor&) = delete;
        ~ProgressMonitor() {
            if (!stream_.is_open()) return;
            { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
            cv_.notify_one();
            if (thread_.joinable()) thread_.join();
            sample();
            stream_.flush();
        }
    private:
        Solver& solver_;
        std::ofstream stream_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::thread thread_;
        bool stopped_ = false;
        void sample() {
            auto c = solver_.nodes_customer_.load(std::memory_order_relaxed);
            auto r = solver_.nodes_relaxed_.load(std::memory_order_relaxed);
            auto f = solver_.nodes_frontier_.load(std::memory_order_relaxed);
            auto cl = solver_.nodes_closure_.load(std::memory_order_relaxed);
            int ub = solver_.published_ub_.load(std::memory_order_relaxed);
            int lb = solver_.published_lb_.load(std::memory_order_relaxed);
            double gap = (ub > 0 && lb > 0 && ub >= lb) ? 100.0 * (ub-lb) / lb : 0.0;
            stream_ << std::fixed << std::setprecision(6)
                    << solver_.elapsed_since(solver_.start_) << ',' << (c+r+f+cl) << ','
                    << c << ',' << r << ',' << cl << ',' << f << ',' << ub << ',' << lb << ','
                    << gap << ',' << solver_.published_completed_proofs_.load(std::memory_order_relaxed) << '\n';
            stream_.flush();
        }
    };

    SolveResult run() {
        start_ = Clock::now();
        cpu_start_ = std::clock();
        read_instance();
        build_sets();
        initialize_bounds();
        load_warm_start_if_any();
        if (opt_.relaxation_mode == "full") secure_full_incumbent();
        publish_bounds();
        ProgressMonitor progress(*this);

        if (opt_.explicit_upper > 0 && !opt_.quiet) {
            std::cerr << "warning: --upper has no original-instance feasibility certificate; numeric hint ignored. "
                         "Provide a verified MCNh PRODUCT sequence instead.\n";
        }
        if (!opt_.quiet) {
            std::cerr << "instance patterns=" << inst_.n_patterns << " pieces=" << inst_.n_pieces
                      << " initial_ub=" << incumbent_ << " initial_lb=" << certified_lb_
                      << " threads=" << opt_.threads << " parallel_depth=" << opt_.parallel_depth << "\n";
        }

        if (incumbent_ <= certified_lb_) return finalize("OPTIMAL");

        int k = incumbent_ - 1;
        while (k >= certified_lb_) {
            decision_found_order_.clear();
            current_decision_limit_ = k;
            DecisionResult dr = decision_search(k);
            if (dr.status == DecisionResult::Status::Feasible) {
                if (opt_.relaxation_mode == "full") {
                    const int v = evaluate_pattern_order_as_customer_order(decision_found_order_);
                    if (v > k || v >= std::numeric_limits<int>::max() / 8)
                        throw std::runtime_error("Full relaxation feasibility safeguard: invalid or absent original-graph witness");
                }
                last_completed_target_ = k;
                incumbent_ = k;
                publish_bounds();
                if (!decision_found_order_.empty()) best_pattern_order_ = decision_found_order_;
                --k;
                continue;
            }
            if (dr.status == DecisionResult::Status::Infeasible) {
                last_completed_target_ = k;
                certified_lb_ = std::max(certified_lb_, k + 1);
                publish_bounds();
                break;
            }
            if (dr.status == DecisionResult::Status::Timeout) return finalize("TIME_LIMIT");
            if (dr.status == DecisionResult::Status::NodeLimit) return finalize("NODE_LIMIT");
        }

        if (incumbent_ == certified_lb_) return finalize("OPTIMAL");
        return finalize("COMPLETE_BUT_GAP");
    }

private:
    struct Move {
        int representative = -1;
        Bitset open_set;
        Bitset close_set;
        int open_count = 0;
        int close_count = 0;
        double score = 0.0;
    };

    struct FrontierState {
        Bitset opened;
        Bitset remaining;
        std::vector<int> closed_order;
    };

    // A closure frontier state contains every variable needed to resume its DFS.
    // Frontier workers never mutate the parent state or the Solver's shared counters.
    struct ClosureFrontierState {
        Bitset processed;
        Bitset closed_items;
        Bitset opened_active;
        std::vector<int> pattern_order;
    };

    struct ClosureSearchContext {
        std::unordered_set<Bitset, BitsetHash> dead;
        std::atomic<bool>* stop = nullptr;
        std::uint64_t local_nodes = 0;
        std::uint64_t local_successes = 0;
        std::uint64_t local_dead_peak = 0;
    };

    // Common, bounded, exception-safe frontier dispatcher used for both the
    // original customer search and the close-stack search.
    template<class State, class Function>
    static void dispatch_frontier(const std::vector<State>& frontier,
                                  int threads, std::atomic<bool>& stop, Function&& work) {
        const int nworkers = std::min<int>(threads, static_cast<int>(frontier.size()));
        if (nworkers == 0) return;
        std::atomic<size_t> next_idx{0};
        std::mutex error_mutex;
        std::exception_ptr worker_error;
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(nworkers));
        auto worker = [&](int worker_id) {
            try {
                while (!stop.load(std::memory_order_acquire)) {
                    size_t idx = next_idx.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= frontier.size()) return;
                    work(worker_id, frontier[idx]);
                }
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(error_mutex);
                    if (!worker_error) worker_error = std::current_exception();
                }
                stop.store(true, std::memory_order_release);
            }
        };
        try {
            for (int id = 0; id < nworkers; ++id) workers.emplace_back(worker, id);
        } catch (...) {
            stop.store(true, std::memory_order_release);
            for (auto& th : workers) th.join();
            throw;
        }
        for (auto& th : workers) th.join();
        if (worker_error) std::rethrow_exception(worker_error);
    }

    struct SearchContext {
        DominanceTable table;
        std::uint64_t local_nodes = 0;
        std::uint64_t local_hash_hits = 0;
        std::uint64_t local_residual_calls = 0;
        std::uint64_t local_residual_core_tests = 0;
        std::uint64_t local_residual_prunes = 0;
        std::uint64_t local_hash_entries = 0;
        std::uint64_t local_dominance_keys = 0;
        std::atomic<bool>* stop = nullptr;
        std::atomic<std::uint64_t>* global_nodes = nullptr;
        std::mutex* result_mutex = nullptr;
        std::vector<int>* shared_found_order = nullptr;
        ProfileStats profile;
        std::optional<Clock::time_point> relax_deadline;
        bool relaxed_phase = false;
    };

    Options opt_;
    Instance inst_;
    Clock::time_point start_;

    std::vector<Bitset> plays_; // piece -> patterns
    std::vector<Bitset> cross_; // pattern -> patterns sharing at least one piece
    std::vector<Bitset> pattern_pieces_; // pattern -> pieces/items
    Bitset all_patterns_;
    Bitset all_pieces_;

    int incumbent_ = -1;
    int certified_lb_ = 1;
    int current_decision_limit_ = -1;
    std::vector<int> best_pattern_order_;
    std::vector<int> decision_found_order_;
    std::uint64_t nodes_ = 0;
    std::atomic<std::uint64_t> nodes_customer_{0}, nodes_relaxed_{0}, nodes_closure_{0}, nodes_frontier_{0};
    std::atomic<int> published_ub_{-1}, published_lb_{1};
    std::atomic<std::uint64_t> published_completed_proofs_{0};
    std::vector<WorkerMetric> workers_customer_, workers_relaxed_, workers_closure_;
    std::clock_t cpu_start_ = std::clock();

    void add_workers(std::vector<WorkerMetric>& destination, const std::vector<WorkerMetric>& source,
                     double dispatch_seconds) {
        if (destination.size() < source.size()) destination.resize(source.size());
        for (size_t i=0; i<source.size(); ++i) {
            WorkerMetric m = source[i];
            // A worker slot is available during the entire dispatch window.
            m.available_seconds = dispatch_seconds;
            destination[i].add(m);
        }
    }

    std::uint64_t hash_hits_ = 0;
    std::uint64_t hash_entries_ = 0;
    std::uint64_t dominance_keys_ = 0;
    std::uint64_t last_frontier_states_ = 0;
    std::uint64_t relaxation_probes_ = 0;
    std::uint64_t relaxation_successes_ = 0;
    std::uint64_t relaxation_merges_ = 0;
    std::string relaxed_proof_status_ = "NOT_RUN";
    std::string proof_source_ = "NONE";
    std::uint64_t relaxed_proofs_attempted_ = 0;
    std::uint64_t relaxed_proofs_completed_ = 0;
    std::uint64_t relaxed_proof_infeasibilities_ = 0;
    std::uint64_t relaxation_full_merges_ = 0;
    std::uint64_t relaxation_full_unmerges_ = 0;
    std::uint64_t relaxation_customers_at_proof_ = 0;
    double relaxation_full_seconds_ = 0.0;
    double relaxed_proof_seconds_ = 0.0;
    std::uint64_t relaxed_proof_parallel_calls_ = 0;
    std::uint64_t relaxed_proof_frontier_states_ = 0;
    int relaxed_proof_max_workers_ = 0;
    bool original_fallback_used_ = false;
    int full_verified_initial_upper_bound_ = -1;
    bool full_unverified_warm_ignored_ = false;
    bool full_explicit_upper_ignored_ = false;
    int active_target_ = -1;
    int last_completed_target_ = -1;
    ProfileStats profile_;
    int warm_start_value_ = -1;
    int warm_start_sequence_value_ = -1;
    int warm_start_faggioli_value_ = -1;
    bool warm_start_evaluators_agree_ = true;
    int applied_initial_lower_bound_ = 1;
    int legacy_lower_bound_ = 1;
    int model_cardinality_lower_bound_ = 1;
    int graph_min_degree_lower_bound_ = 1;
    int graph_degeneracy_lower_bound_ = 1;
    int customer_graph_core_lower_bound_ = 1;
    std::uint64_t residual_lb_calls_ = 0;
    std::uint64_t residual_lb_core_tests_ = 0;
    std::uint64_t residual_lb_prunes_ = 0;
    int selected_anchor_pattern_ = -1;
    bool reverse_anchor_applied_ = false;
    bool warm_start_accepted_ = false;
    bool warm_start_sequence_accepted_ = false;
    bool warm_start_verified_ = false;
    std::string warm_start_validation_status_ = "NOT_PROVIDED";
    int warm_start_declared_value_ = -1;
    int warm_start_verified_value_ = -1;
    std::string warm_start_sequence_domain_ = "NONE";

    std::uint64_t closure_branch_calls_ = 0;
    std::uint64_t closure_branch_nodes_ = 0;
    std::uint64_t closure_branch_dead_states_ = 0;
    std::uint64_t closure_branch_successes_ = 0;
    std::uint64_t closure_branch_fallbacks_ = 0;
    std::uint64_t closure_parallel_calls_ = 0;
    std::uint64_t closure_frontier_states_ = 0;
    int closure_max_workers_ = 0;

    static double elapsed_since(Clock::time_point t0) {
        return std::chrono::duration<double>(Clock::now() - t0).count();
    }

    bool present_value(int v) const {
        if (opt_.presence_mode == "legacy-one") return v == 1;
        return v > 0;
    }

    void read_instance() {
        std::ifstream in(opt_.input_path);
        if (!in) throw std::runtime_error("Could not open input file: " + opt_.input_path);

        if (opt_.input_format == "cs") {
            in >> inst_.n_patterns >> inst_.n_pieces;
            if (!in || inst_.n_patterns <= 0 || inst_.n_pieces <= 0) throw std::runtime_error("Invalid CS input header.");
            inst_.pattern_piece.assign(inst_.n_patterns, std::vector<unsigned char>(inst_.n_pieces, 0));
            for (int p = 0; p < inst_.n_patterns; ++p) {
                for (int j = 0; j < inst_.n_pieces; ++j) {
                    int v = 0;
                    if (!(in >> v)) throw std::runtime_error("Unexpected end of CS input matrix.");
                    inst_.pattern_piece[p][j] = present_value(v) ? 1 : 0;
                }
            }
        } else if (opt_.input_format == "scoop") {
            int n_pieces = 0, n_patterns = 0;
            in >> n_pieces >> n_patterns;
            if (!in || n_patterns <= 0 || n_pieces <= 0) throw std::runtime_error("Invalid SCOOP input header.");
            inst_.n_patterns = n_patterns;
            inst_.n_pieces = n_pieces;
            inst_.pattern_piece.assign(n_patterns, std::vector<unsigned char>(n_pieces, 0));
            for (int piece = n_pieces - 1; piece >= 0; --piece) {
                for (int pat = 0; pat < n_patterns; ++pat) {
                    int v = 0;
                    if (!(in >> v)) throw std::runtime_error("Unexpected end of SCOOP input matrix.");
                    inst_.pattern_piece[pat][piece] = present_value(v) ? 1 : 0;
                }
            }
        } else {
            throw std::runtime_error("Invalid --input-format: " + opt_.input_format);
        }
    }

    void build_sets() {
        all_patterns_.reset(inst_.n_patterns);
        for (int p = 0; p < inst_.n_patterns; ++p) all_patterns_.set(p);
        all_pieces_.reset(inst_.n_pieces);
        for (int j = 0; j < inst_.n_pieces; ++j) all_pieces_.set(j);

        plays_.assign(inst_.n_pieces, Bitset(inst_.n_patterns));
        cross_.assign(inst_.n_patterns, Bitset(inst_.n_patterns));
        pattern_pieces_.assign(inst_.n_patterns, Bitset(inst_.n_pieces));

        for (int p = 0; p < inst_.n_patterns; ++p) {
            bool has_piece = false;
            for (int j = 0; j < inst_.n_pieces; ++j) {
                if (inst_.pattern_piece[p][j]) {
                    has_piece = true;
                    plays_[j].set(p);
                    pattern_pieces_[p].set(j);
                }
            }
            if (!has_piece) throw std::runtime_error("Pattern/customer with no pieces/products encountered.");
        }

        for (int p = 0; p < inst_.n_patterns; ++p) {
            for (int j = 0; j < inst_.n_pieces; ++j) {
                if (inst_.pattern_piece[p][j]) cross_[p].inplace_or(plays_[j]);
            }
            if (cross_[p].empty()) throw std::runtime_error("Pattern/customer with empty cross set encountered.");
        }
    }

    int legacy_customer_lower_bound() const {
        int lb = 1;
        for (const Bitset& piece_set : plays_) lb = std::max(lb, piece_set.count());
        return lb;
    }

    int model_cardinality_lower_bound() const {
        int lb = 1;
        for (int p = 0; p < inst_.n_patterns; ++p) {
            int row = 0;
            for (int j = 0; j < inst_.n_pieces; ++j) row += inst_.pattern_piece[p][j] ? 1 : 0;
            lb = std::max(lb, row);
        }
        return lb;
    }

    std::vector<Bitset> build_piece_graph() const {
        std::vector<Bitset> adj;
        adj.assign(inst_.n_pieces, Bitset(inst_.n_pieces));
        std::vector<int> items;
        items.reserve(static_cast<size_t>(std::min(inst_.n_pieces, 128)));
        for (int p = 0; p < inst_.n_patterns; ++p) {
            items.clear();
            for (int j = 0; j < inst_.n_pieces; ++j) if (inst_.pattern_piece[p][j]) items.push_back(j);
            for (size_t a = 0; a < items.size(); ++a) {
                for (size_t b = a + 1; b < items.size(); ++b) {
                    adj[items[a]].set(items[b]);
                    adj[items[b]].set(items[a]);
                }
            }
        }
        return adj;
    }

    int graph_min_degree_plus_one(const std::vector<Bitset>& adj) const {
        if (adj.empty()) return 1;
        int md = std::numeric_limits<int>::max();
        for (const Bitset& a : adj) md = std::min(md, a.count());
        if (md == std::numeric_limits<int>::max()) md = 0;
        return std::max(1, md + 1);
    }

    int graph_degeneracy_plus_one(const std::vector<Bitset>& adj) const {
        const int n = static_cast<int>(adj.size());
        if (n <= 0) return 1;
        std::vector<int> deg(n, 0);
        std::vector<unsigned char> removed(n, 0);
        for (int i = 0; i < n; ++i) deg[i] = adj[i].count();
        int max_min_degree = 0;
        for (int step = 0; step < n; ++step) {
            int best = -1;
            int best_deg = std::numeric_limits<int>::max();
            for (int i = 0; i < n; ++i) {
                if (!removed[i] && deg[i] < best_deg) {
                    best = i;
                    best_deg = deg[i];
                }
            }
            if (best < 0) break;
            max_min_degree = std::max(max_min_degree, best_deg);
            removed[best] = 1;
            adj[best].for_each_set_bit([&](int nb) {
                if (!removed[nb]) --deg[nb];
            });
        }
        return std::max(1, max_min_degree + 1);
    }

    int compute_auto_lower_bound() {
        legacy_lower_bound_ = legacy_customer_lower_bound();
        model_cardinality_lower_bound_ = model_cardinality_lower_bound();
        std::vector<Bitset> graph = build_piece_graph();
        graph_min_degree_lower_bound_ = graph_min_degree_plus_one(graph);
        graph_degeneracy_lower_bound_ = graph_degeneracy_plus_one(graph);
        customer_graph_core_lower_bound_ = ResidualBounds::degeneracy_plus_one(cross_, all_patterns_);

        if (!opt_.auto_lower_bound || opt_.auto_lower_mode == "off") return 1;
        if (opt_.auto_lower_mode == "legacy") return legacy_lower_bound_;
        if (opt_.auto_lower_mode == "graph") {
            // The co-occurrence graph is on CUSTOMERS (not products): safe.
            return std::max(legacy_lower_bound_, customer_graph_core_lower_bound_);
        }
        if (opt_.auto_lower_mode == "model") {
            // PRODUCT cardinality/degeneracy are unsafe for CUSTOMER stacks.
            return legacy_lower_bound_;
        }
        throw std::runtime_error("Invalid --auto-lower-mode: " + opt_.auto_lower_mode);
    }

    void initialize_bounds() {
        selected_anchor_pattern_ = choose_anchor_pattern();
        const int automatic_lb = compute_auto_lower_bound();
        // Numeric-only external lower assertions are not certificates.
        // Accept --lower only up to a lower bound already established by the
        // independently provable CUSTOMER-graph and legacy counting arguments.
        const int proven_root = std::max(legacy_lower_bound_, customer_graph_core_lower_bound_);
        if (opt_.explicit_lower > proven_root)
            throw std::runtime_error("Uncertified --lower exceeds the internally proven customer-graph bound. "
                                     "An external number alone cannot certify optimality.");
        certified_lb_ = opt_.explicit_lower > 0 ? opt_.explicit_lower : automatic_lb;
        applied_initial_lower_bound_ = certified_lb_;
        // The number of original customer stacks is always a feasible UB.
        // --upper alone is an unverified assertion, never a pruning bound.
        incumbent_ = inst_.n_patterns;
        if (opt_.explicit_upper > 0) full_explicit_upper_ignored_ = true;
        if (certified_lb_ > incumbent_) throw std::runtime_error("Invalid external lower bound exceeds trivial feasible upper bound");
    }

    int choose_anchor_pattern() const {
        if (opt_.anchor_pattern >= 0) {
            if (opt_.anchor_pattern >= inst_.n_patterns) throw std::runtime_error("--anchor-pattern is outside the pattern range.");
            return opt_.anchor_pattern;
        }
        int best = 0;
        int best_card = -1;
        for (int p = 0; p < inst_.n_patterns; ++p) {
            int card = 0;
            for (int j = 0; j < inst_.n_pieces; ++j) card += inst_.pattern_piece[p][j] ? 1 : 0;
            if (card > best_card || (card == best_card && p < best)) {
                best = p;
                best_card = card;
            }
        }
        return best;
    }

    bool validate_permutation(const std::vector<int>& order) const {
        if (static_cast<int>(order.size()) != inst_.n_patterns) return false;
        std::vector<char> seen(inst_.n_patterns, 0);
        for (int p : order) {
            if (p < 0 || p >= inst_.n_patterns || seen[p]) return false;
            seen[p] = 1;
        }
        return true;
    }

    void apply_reverse_anchor_canonicalization(std::vector<int>& order) {
        if (opt_.symmetry_break != "reverse-anchor") return;
        if (!validate_permutation(order)) return;
        if (selected_anchor_pattern_ < 0) selected_anchor_pattern_ = choose_anchor_pattern();
        auto it = std::find(order.begin(), order.end(), selected_anchor_pattern_);
        if (it == order.end()) return;
        const int pos = static_cast<int>(std::distance(order.begin(), it));
        if (pos > (inst_.n_patterns - 1) / 2) {
            std::reverse(order.begin(), order.end());
            reverse_anchor_applied_ = true;
        }
    }

    int evaluate_pattern_order_faggioli(const std::vector<int>& order, const std::string& profile_path = "") const {
        if (!validate_permutation(order)) return std::numeric_limits<int>::max() / 4;
        std::vector<int> first(inst_.n_pieces, -1), last(inst_.n_pieces, -1);
        for (int pos = 0; pos < inst_.n_patterns; ++pos) {
            const int p = order[static_cast<size_t>(pos)];
            for (int j = 0; j < inst_.n_pieces; ++j) {
                if (!inst_.pattern_piece[p][j]) continue;
                if (first[j] < 0) first[j] = pos;
                last[j] = pos;
            }
        }
        int best = 0;
        std::ofstream profile;
        if (!profile_path.empty()) {
            profile.open(profile_path);
            if (profile) profile << "position,pattern_0_based,pattern_1_based,open_stacks\n";
        }
        for (int pos = 0; pos < inst_.n_patterns; ++pos) {
            int open = 0;
            for (int j = 0; j < inst_.n_pieces; ++j) {
                if (first[j] >= 0 && first[j] <= pos && pos <= last[j]) ++open;
            }
            best = std::max(best, open);
            if (profile) profile << pos << ',' << order[static_cast<size_t>(pos)] << ',' << (order[static_cast<size_t>(pos)] + 1) << ',' << open << "\n";
        }
        return best;
    }

    int evaluate_pattern_order_as_customer_order(const std::vector<int>& order, std::vector<int>* product_order_out = nullptr) const {
        if (static_cast<int>(order.size()) != inst_.n_patterns) return std::numeric_limits<int>::max() / 4;
        std::vector<char> seen_pattern(inst_.n_patterns, 0);
        for (int p : order) {
            if (p < 0 || p >= inst_.n_patterns || seen_pattern[p]) return std::numeric_limits<int>::max() / 4;
            seen_pattern[p] = 1;
        }

        Bitset custs_open(inst_.n_patterns);
        Bitset custs = all_patterns_;
        std::vector<char> played_piece(inst_.n_pieces, 0);
        std::vector<int> product_order;
        int best = 0;

        for (int p : order) {
            custs_open.inplace_or(cross_[p]);
            for (int j = 0; j < inst_.n_pieces; ++j) {
                if (!played_piece[j] && plays_[j].subset_of(custs_open)) {
                    played_piece[j] = 1;
                    product_order.push_back(j);
                }
            }
            best = std::max(best, custs_open.bit_and(custs).count());
            custs.clear(p);
        }
        if (static_cast<int>(product_order.size()) != inst_.n_pieces) return std::numeric_limits<int>::max() / 4;
        if (product_order_out) *product_order_out = std::move(product_order);
        return best;
    }

    // An imported MCNh certificate must encode a PRODUCT sequence. The MOSP
    // stacks are the CS customer rows, and each column is a product. This is
    // different from the internal Chu--Stuckey CUSTOMER CLOSURE order. Never
    // pass an imported product permutation to the customer-order evaluator.
    bool valid_product_permutation(const std::vector<int>& order) const {
        if (static_cast<int>(order.size()) != inst_.n_pieces) return false;
        std::vector<unsigned char> seen(static_cast<size_t>(inst_.n_pieces), 0);
        for (int j : order) {
            if (j < 0 || j >= inst_.n_pieces || seen[static_cast<size_t>(j)]) return false;
            seen[static_cast<size_t>(j)] = 1;
        }
        return true;
    }

    // Independent evaluator A: a customer stack is active at every position
    // between its first and last produced product (inclusive).
    int evaluate_product_order_intervals(const std::vector<int>& order) const {
        if (!valid_product_permutation(order)) throw std::runtime_error("Invalid product permutation in warm start");
        std::vector<int> position(static_cast<size_t>(inst_.n_pieces));
        for (int t = 0; t < inst_.n_pieces; ++t) position[static_cast<size_t>(order[t])] = t;
        std::vector<int> delta(static_cast<size_t>(inst_.n_pieces + 1), 0);
        for (int c = 0; c < inst_.n_patterns; ++c) {
            int first = inst_.n_pieces, last = -1;
            for (int j = 0; j < inst_.n_pieces; ++j) if (inst_.pattern_piece[c][j]) {
                first = std::min(first, position[static_cast<size_t>(j)]);
                last = std::max(last, position[static_cast<size_t>(j)]);
            }
            if (last < 0) throw std::runtime_error("Empty customer in warm-start evaluation");
            ++delta[static_cast<size_t>(first)];
            --delta[static_cast<size_t>(last+1)];
        }
        int open = 0, best = 0;
        for (int t = 0; t < inst_.n_pieces; ++t) {
            open += delta[static_cast<size_t>(t)];
            best = std::max(best, open);
        }
        return best;
    }

    // Independent evaluator B: produce each product and count customer stacks
    // before closing those completed by this product. Matches the MCNh
    // counting convention, but is implemented on the original CS matrix.
    int evaluate_product_order_streaming(const std::vector<int>& order) const {
        if (!valid_product_permutation(order)) throw std::runtime_error("Invalid product permutation in warm start");
        std::vector<int> remaining(static_cast<size_t>(inst_.n_patterns), 0);
        std::vector<unsigned char> opened(static_cast<size_t>(inst_.n_patterns), 0);
        for (int c = 0; c < inst_.n_patterns; ++c)
            for (int j = 0; j < inst_.n_pieces; ++j)
                if (inst_.pattern_piece[c][j]) ++remaining[static_cast<size_t>(c)];
        int active = 0, best = 0;
        for (int product : order) {
            int closes = 0;
            for (int c = 0; c < inst_.n_patterns; ++c) if (inst_.pattern_piece[c][product]) {
                if (!opened[static_cast<size_t>(c)]) { opened[static_cast<size_t>(c)] = 1; ++active; }
                if (--remaining[static_cast<size_t>(c)] == 0) ++closes;
            }
            best = std::max(best, active);
            active -= closes;
        }
        if (active != 0) throw std::runtime_error("Unclosed stacks after warm-start evaluation");
        return best;
    }

    struct ImportedWarm {
        int declared = -1;
        int declared_products = -1;
        int declared_customers = -1;
        std::string domain;
        std::string objective;
        std::vector<int> product_order;
        bool has_0 = false, has_1 = false;
    };

    ImportedWarm parse_warm_file(const std::string& filepath) const {
        std::ifstream in(filepath);
        if (!in) throw std::runtime_error("Could not open warm-start file: " + filepath);
        ImportedWarm warm;
        std::vector<int> seq0, seq1;
        std::string line;
        bool found_value = false;
        int seen_domain = 0, seen_objective = 0, seen_customer = 0, seen_product = 0;
        while (std::getline(in, line)) {
            auto hash = line.find('#');
            if (hash != std::string::npos) line.erase(hash);
            std::istringstream iss(line);
            std::string key;
            if (!(iss >> key)) continue;
            auto read_single_int = [&](int& target, const char* name) {
                std::string extra;
                if (!(iss >> target) || (iss >> extra)) throw std::runtime_error(std::string("Invalid warm-start field: ") + name);
            };
            if (key == "value") {
                if (found_value) throw std::runtime_error("Duplicate warm-start value");
                read_single_int(warm.declared, "value"); found_value = true;
            } else if (key == "n_customers") {
                if (++seen_customer > 1) throw std::runtime_error("Duplicate n_customers");
                read_single_int(warm.declared_customers, "n_customers");
            } else if (key == "n_products") {
                if (++seen_product > 1) throw std::runtime_error("Duplicate n_products");
                read_single_int(warm.declared_products, "n_products");
            } else if (key == "sequence_domain") {
                if (++seen_domain > 1 || !(iss >> warm.domain)) throw std::runtime_error("Invalid sequence_domain");
                std::string extra; if (iss >> extra) throw std::runtime_error("Invalid sequence_domain");
            } else if (key == "objective_domain") {
                if (++seen_objective > 1 || !(iss >> warm.objective)) throw std::runtime_error("Invalid objective_domain");
                std::string extra; if (iss >> extra) throw std::runtime_error("Invalid objective_domain");
            } else if (key == "sequence_0_based") {
                if (warm.has_0) throw std::runtime_error("Duplicate sequence_0_based");
                warm.has_0 = true;
                std::string word;
                while (iss >> word) {
                    size_t used = 0;
                    int v;
                    try { v = std::stoi(word, &used); }
                    catch (...) { throw std::runtime_error("Invalid 0-based sequence element"); }
                    if (used != word.size()) throw std::runtime_error("Invalid 0-based sequence element");
                    seq0.push_back(v);
                }
            } else if (key == "sequence_1_based") {
                if (warm.has_1) throw std::runtime_error("Duplicate sequence_1_based");
                warm.has_1 = true;
                std::string word;
                while (iss >> word) {
                    size_t used = 0;
                    int v;
                    try { v = std::stoi(word, &used); }
                    catch (...) { throw std::runtime_error("Invalid 1-based sequence element"); }
                    if (used != word.size() || v < 1) throw std::runtime_error("Invalid 1-based sequence element");
                    seq1.push_back(v-1);
                }
            } else if (key == "n_patterns") {
                // Legacy MCNh metadata; no certificate can be inferred from it.
                int n; read_single_int(n, "n_patterns");
            } else if (key[0] != '#') {
                throw std::runtime_error("Unrecognized warm-start field: " + key);
            }
        }
        if (warm.domain != "products" || warm.objective != "customers")
            throw std::runtime_error("Warm start needs sequence_domain products and objective_domain customers; regenerate with MCNh --objective-orientation customer");
        if (warm.declared_products != inst_.n_pieces || warm.declared_customers != inst_.n_patterns)
            throw std::runtime_error("Warm-start customer/product dimensions do not match input instance");
        if (!warm.has_0 && !warm.has_1) throw std::runtime_error("Warm start has no product sequence");
        if (warm.has_0 && warm.has_1 && seq0 != seq1)
            throw std::runtime_error("Conflicting 0-based and 1-based warm-start sequences");
        warm.product_order = warm.has_0 ? std::move(seq0) : std::move(seq1);
        if (!valid_product_permutation(warm.product_order))
            throw std::runtime_error("Warm-start product sequence must be an exact permutation of all original products");
        return warm;
    }

    void load_warm_start_if_any() {
        const auto warm_t0 = Clock::now();
        if (opt_.warm_start_file.empty()) return;
        ImportedWarm w = parse_warm_file(opt_.warm_start_file);
        warm_start_sequence_domain_ = w.domain;
        warm_start_declared_value_ = w.declared;
        const int interval_score = evaluate_product_order_intervals(w.product_order);
        const int streamed_score = evaluate_product_order_streaming(w.product_order);
        if (interval_score != streamed_score) throw std::runtime_error("Warm-start verification failure: independent original-instance evaluators disagree");
        warm_start_verified_ = true;
        warm_start_verified_value_ = interval_score;
        warm_start_faggioli_value_ = interval_score;
        warm_start_sequence_value_ = streamed_score;
        warm_start_evaluators_agree_ = true;
        if (opt_.warm_start_policy == "strict" || opt_.warm_start_policy == "value") {
            if (w.declared < 1 || w.declared != interval_score) {
                throw std::runtime_error("Warm-start declared UB mismatches independently verified MOSP cost (declared=" +
                                         std::to_string(w.declared) + ", computed=" + std::to_string(interval_score) + ")");
            }
        }
        // 'sequence' ignores the declared value, but still validates the
        // original-instance product sequence and both independent evaluators.
        warm_start_validation_status_ = (w.declared > 0 && w.declared != interval_score)
            ? "VERIFIED_SEQUENCE_DECLARATION_IGNORED" : "VERIFIED";
        warm_start_value_ = interval_score;
        if (interval_score < incumbent_) {
            incumbent_ = interval_score;
            warm_start_accepted_ = true;
        }
        // Do NOT attach product order as customer-close order. The internal
        // Chu--Stuckey state has a different decision representation.
        warm_start_sequence_accepted_ = false;
        if (opt_.profile) profile_.warm_start_seconds += elapsed_since(warm_t0);
    }

    std::vector<int> greedy_order_from_cross(const std::vector<Bitset>& local_cross) const {
        Bitset opened(inst_.n_patterns);
        Bitset remaining = all_patterns_;
        std::vector<int> order;
        order.reserve(inst_.n_patterns);
        while (!remaining.empty()) {
            std::vector<int> forced;
            remaining.for_each_set_bit([&](int p) {
                if (local_cross[p].subset_of(opened)) forced.push_back(p);
            });
            if (!forced.empty()) {
                std::sort(forced.begin(), forced.end());
                for (int p : forced) {
                    if (remaining.test(p)) {
                        remaining.clear(p);
                        order.push_back(p);
                    }
                }
                continue;
            }
            int best = -1;
            int best_open = std::numeric_limits<int>::max();
            int best_cross = std::numeric_limits<int>::max();
            remaining.for_each_set_bit([&](int p) {
                const int oc = local_cross[p].minus(opened).count();
                const int cc = local_cross[p].count();
                if (oc < best_open || (oc == best_open && cc < best_cross) || (oc == best_open && cc == best_cross && p < best)) {
                    best = p;
                    best_open = oc;
                    best_cross = cc;
                }
            });
            if (best < 0) break;
            opened.inplace_or(local_cross[best]);
            remaining.clear(best);
            order.push_back(best);
        }
        return order;
    }

    // The full-relaxation engine must use an incumbent whose feasibility on
    // the ORIGINAL instance was independently checked. MCNh product sequences
    // are verified in load_warm_start_if_any(), and are safe as scalar UBs.
    void secure_full_incumbent() {
        if (opt_.explicit_upper > 0) full_explicit_upper_ignored_ = true;
        if (!opt_.warm_start_file.empty() && !warm_start_verified_)
            throw std::runtime_error("Full relaxation requires a verified warm-start certificate");
        // Retain the verified greedy customer-close-order baseline from the
        // previous full-relaxation release. Importing a product-order MCNh
        // certificate does not remove this independently feasible candidate.
        const std::vector<int> greedy = greedy_order_from_cross(cross_);
        const int greedy_cost = evaluate_pattern_order_as_customer_order(greedy);
        if (greedy_cost < 1 || greedy_cost > inst_.n_patterns)
            throw std::runtime_error("Cannot verify baseline customer-close order");
        if (greedy_cost < incumbent_) {
            incumbent_ = greedy_cost;
            best_pattern_order_ = greedy;
        }
        if (incumbent_ > inst_.n_patterns || incumbent_ < 1)
            throw std::runtime_error("Invalid incumbent for full relaxation");
        full_verified_initial_upper_bound_ = incumbent_;
        certified_lb_ = (opt_.auto_lower_bound && opt_.auto_lower_mode == "graph")
            ? std::max(legacy_customer_lower_bound(), customer_graph_core_lower_bound_)
            : legacy_customer_lower_bound();
        applied_initial_lower_bound_ = certified_lb_;
        if (certified_lb_ > incumbent_) throw std::runtime_error("Invalid incumbent/LB invariant");
    }

    // Extend a solution of a contracted customer graph by reinserting the
    // removed vertices directly before their representatives (reverse merges).
    // This is only a CANDIDATE until evaluated against the ORIGINAL graph.
    std::vector<int> extend_relaxed_order(const std::vector<int>& contracted,
                                          const std::vector<RelaxationGraph::MergeRecord>& log) const {
        std::vector<int> result = contracted;
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            auto pos = std::find(result.begin(), result.end(), it->representative);
            if (pos == result.end()) return {};
            result.insert(pos, it->removed);
        }
        return result;
    }

    // Parallel proof of a SINGLE contracted graph. The caller temporarily
    // installs that immutable adjacency matrix in cross_. Each worker gets a
    // separate dominance table, statistics and DFS recursion stack. No two
    // graph versions are ever searched concurrently.
    DecisionResult solve_contracted_parallel(int k,
            const Bitset& contracted_active,
            std::atomic<std::uint64_t>& global_nodes,
            std::vector<int>& contracted_solution,
            const std::optional<Clock::time_point>& relax_deadline,
            int requested_threads, int depth) {
        std::vector<FrontierState> frontier;
        frontier.reserve(4096);
        Bitset opened(inst_.n_patterns);
        Bitset remaining=contracted_active;
        std::vector<int> order;
        std::vector<int> immediate;
        const auto t_frontier=Clock::now();
        DecisionResult front=expand_frontier(opened,remaining,order,k,depth,frontier,
                                             global_nodes,immediate,relax_deadline,
                                             static_cast<size_t>(opt_.relax_proof_frontier_cap));
        if (opt_.profile) profile_.frontier_seconds += elapsed_since(t_frontier);
        relaxed_proof_frontier_states_ += frontier.size();
        if (front.status == DecisionResult::Status::Feasible) {
            contracted_solution=std::move(immediate);
            return front;
        }
        if (front.status != DecisionResult::Status::Infeasible) return front;
        if (frontier.empty()) return {DecisionResult::Status::Infeasible};

        const int nworkers=std::min<int>(requested_threads,static_cast<int>(frontier.size()));
        relaxed_proof_max_workers_=std::max(relaxed_proof_max_workers_,nworkers);
        if (nworkers>1) ++relaxed_proof_parallel_calls_;
        std::atomic<size_t> next_idx(0);
        std::atomic<bool> stop(false);
        std::atomic<int> terminal(0); // 1=timeout, 2=node limit, 3=relax budget
        std::mutex result_mutex, profile_mutex, error_mutex;
        std::vector<int> found;
        std::exception_ptr worker_exception;
        std::atomic<std::uint64_t> ag_hits(0),ag_entries(0),ag_keys(0);
        std::atomic<std::uint64_t> ag_residual(0),ag_core(0),ag_prunes(0);
        ProfileStats ag_profile;
        std::vector<WorkerMetric> worker_metrics(static_cast<size_t>(nworkers));
        const auto dispatch_start = Clock::now();
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(nworkers));
        auto work = [&](int worker_id) {
            WorkerMetric &wm = worker_metrics[static_cast<size_t>(worker_id)];
            SearchContext ctx;
            ctx.stop=&stop;
            ctx.global_nodes=&global_nodes;
            ctx.result_mutex=&result_mutex;
            ctx.shared_found_order=&found;
            ctx.relax_deadline=relax_deadline;
            ctx.relaxed_phase=true;
            try {
                while (!stop.load(std::memory_order_acquire)) {
                    const size_t idx=next_idx.fetch_add(1,std::memory_order_relaxed);
                    if (idx>=frontier.size()) break;
                    FrontierState st=frontier[idx];
                    auto t_task=Clock::now();
                    auto before_nodes=ctx.local_nodes;
                    DecisionResult dr=dfs(st.opened,st.remaining,st.closed_order,k,ctx);
                    wm.tasks++; wm.nodes += ctx.local_nodes - before_nodes;
                    wm.active_seconds += elapsed_since(t_task);
                    if (dr.status==DecisionResult::Status::Feasible) {
                        stop.store(true,std::memory_order_release);
                        break;
                    }
                    int code=0;
                    if (dr.status==DecisionResult::Status::Timeout) code=1;
                    if (dr.status==DecisionResult::Status::NodeLimit) code=2;
                    if (dr.status==DecisionResult::Status::RelaxBudget) code=3;
                    if (code) {
                        int expected=0;
                        terminal.compare_exchange_strong(expected,code);
                        stop.store(true,std::memory_order_release);
                        break;
                    }
                }
            } catch (...) {
                { std::lock_guard<std::mutex> lock(error_mutex);
                  if (!worker_exception) worker_exception=std::current_exception(); }
                stop.store(true,std::memory_order_release);
            }
            ag_hits.fetch_add(ctx.local_hash_hits,std::memory_order_relaxed);
            ag_entries.fetch_add(ctx.table.entries(),std::memory_order_relaxed);
            ag_keys.fetch_add(ctx.table.keys(),std::memory_order_relaxed);
            ag_residual.fetch_add(ctx.local_residual_calls,std::memory_order_relaxed);
            ag_core.fetch_add(ctx.local_residual_core_tests,std::memory_order_relaxed);
            ag_prunes.fetch_add(ctx.local_residual_prunes,std::memory_order_relaxed);
            if (opt_.profile) {
                std::lock_guard<std::mutex> lock(profile_mutex);
                ag_profile.add(ctx.profile);
            }
        };
        // A failing thread constructor must not leave joinable threads behind.
        try {
            for (int t=0;t<nworkers;++t) workers.emplace_back(work, t);
        } catch (...) {
            stop.store(true,std::memory_order_release);
            for (auto& t:workers) t.join();
            throw;
        }
        for (auto& t:workers) t.join();
        if (worker_exception) std::rethrow_exception(worker_exception);
        add_workers(workers_relaxed_, worker_metrics, elapsed_since(dispatch_start));
        hash_hits_ += ag_hits.load(std::memory_order_relaxed);
        hash_entries_ += ag_entries.load(std::memory_order_relaxed);
        dominance_keys_ += ag_keys.load(std::memory_order_relaxed);
        residual_lb_calls_ += ag_residual.load(std::memory_order_relaxed);
        residual_lb_core_tests_ += ag_core.load(std::memory_order_relaxed);
        residual_lb_prunes_ += ag_prunes.load(std::memory_order_relaxed);
        if (opt_.profile) profile_.add(ag_profile);
        if (!found.empty()) {
            contracted_solution=std::move(found);
            return {DecisionResult::Status::Feasible};
        }
        const int code=terminal.load(std::memory_order_acquire);
        if (code==1) return {DecisionResult::Status::Timeout};
        if (code==2) return {DecisionResult::Status::NodeLimit};
        if (code==3) return {DecisionResult::Status::RelaxBudget};
        // An interrupted or incomplete frontier is NEVER an infeasibility proof.
        if (stop.load(std::memory_order_acquire))
            throw std::runtime_error("Contracted proof stopped without witness or resource status");
        return {DecisionResult::Status::Infeasible};
    }

    DecisionResult solve_contracted_exact(RelaxationGraph& graph, int k,
                                          std::atomic<std::uint64_t>& global_nodes,
                                          std::vector<int>& contracted_solution,
                                          const std::optional<Clock::time_point>& relax_deadline) {
        const auto t0 = Clock::now();
        ++relaxed_proofs_attempted_;
        relaxed_proof_max_workers_ = std::max(relaxed_proof_max_workers_, 1);
        relaxation_customers_at_proof_ = static_cast<std::uint64_t>(graph.active.count());
        // The contracted graph remains immutable while worker threads run.
        // No original-graph customer search or unmerge runs until all join.
        cross_.swap(graph.rows);
        DecisionResult dr;
        try {
            if (effective_relax_proof_threads()>1 && effective_relax_proof_depth()>0) {
                dr=solve_contracted_parallel(k,graph.active,global_nodes,contracted_solution,
                    relax_deadline,effective_relax_proof_threads(),effective_relax_proof_depth());
            } else {
                SearchContext ctx;
                std::atomic<bool> stop(false);
                std::mutex result_mutex;
                ctx.stop=&stop;
                ctx.global_nodes=&global_nodes;
                ctx.result_mutex=&result_mutex;
                ctx.shared_found_order=&contracted_solution;
                ctx.relax_deadline=relax_deadline;
                ctx.relaxed_phase=true;
                Bitset opened(inst_.n_patterns);
                Bitset remaining=graph.active;
                std::vector<int> order;
                const auto serial_start=Clock::now();
                dr=dfs(opened,remaining,order,k,ctx);
                add_workers(workers_relaxed_, {WorkerMetric{ctx.local_nodes, 1, elapsed_since(serial_start), 0.0}}, elapsed_since(serial_start));
                residual_lb_calls_ += ctx.local_residual_calls;
                residual_lb_core_tests_ += ctx.local_residual_core_tests;
                residual_lb_prunes_ += ctx.local_residual_prunes;
                hash_hits_ += ctx.local_hash_hits;
                hash_entries_ += ctx.table.entries();
                dominance_keys_ += ctx.table.keys();
                if (opt_.profile) profile_.add(ctx.profile);
            }
        } catch (...) { cross_.swap(graph.rows); throw; }
        cross_.swap(graph.rows);
        relaxed_proof_seconds_ += elapsed_since(t0);
        if (dr.status == DecisionResult::Status::Feasible || dr.status == DecisionResult::Status::Infeasible) {
            ++relaxed_proofs_completed_;
            published_completed_proofs_.fetch_add(1, std::memory_order_relaxed);
        }
        if (dr.status == DecisionResult::Status::Infeasible) ++relaxed_proof_infeasibilities_;
        return dr;
    }

    // Reproduction of the original findunmerge principle: among the removable
    // merges, prefer the first (searched backward) whose removal prevents the
    // previously found relaxed witness from being extended by the original
    // cheapest-opening order adjustment. This chooses the NEXT relaxation;
    // it never establishes infeasibility by itself.
    bool selective_unmerge(RelaxationGraph& graph, const std::vector<int>& relaxed_order,
                           int k, const std::function<bool()>& interrupted) {
        if (graph.log.empty()) return false;
        if (opt_.relax_unmerge_policy == "lifo" || graph.log.size() == 1) {
            graph.unmerge();
            ++relaxation_full_unmerges_;
            return true;
        }
        std::vector<std::pair<int,int>> merges;
        merges.reserve(graph.log.size());
        for (const auto& item : graph.log) merges.emplace_back(item.removed,item.representative);
        int selected = static_cast<int>(merges.size()) - 1;
        auto evaluate_relaxed_order = [&](const RelaxationGraph& trial, const std::vector<int>& order) {
            if (static_cast<int>(order.size()) != trial.active.count()) return std::numeric_limits<int>::max()/4;
            Bitset opened(inst_.n_patterns);
            Bitset remaining = trial.active;
            int peak = 0;
            for (int a : order) {
                if (!remaining.test(a)) return std::numeric_limits<int>::max()/4;
                opened.inplace_or(trial.rows[a]);
                peak = std::max(peak,opened.bit_and(remaining).count());
                remaining.clear(a);
            }
            return remaining.empty() ? peak : std::numeric_limits<int>::max()/4;
        };
        for (int skip=static_cast<int>(merges.size())-1;skip>=0;--skip) {
            if (interrupted()) break;
            // Replay from the original graph (unambiguous graph/version identity).
            RelaxationGraph trial(cross_);
            bool valid=true;
            for (int j=0;j<static_cast<int>(merges.size());j++) {
                if (j==skip) continue;
                if (!trial.merge(merges[j].first,merges[j].second)) {valid=false;break;}
            }
            if (!valid) continue;
            std::vector<int> candidate=relaxed_order;
            int removed=merges[skip].first, rep=merges[skip].second;
            auto pos=std::find(candidate.begin(),candidate.end(),rep);
            candidate.insert(pos,removed);
            // Recreate the original extendsolution greedy subset insertion.
            Bitset opened(inst_.n_patterns);
            for (size_t j=0;j<candidate.size();j++) {
                size_t best=j;
                for (size_t h=j+1;h<candidate.size();h++) {
                    if (trial.rows[candidate[h]].minus(opened).subset_of(trial.rows[candidate[best]].minus(opened))) best=h;
                }
                int moved=candidate[best];
                for (size_t h=best;h>j;--h) candidate[h]=candidate[h-1];
                candidate[j]=moved;
                opened.inplace_or(trial.rows[moved]);
            }
            if (evaluate_relaxed_order(trial,candidate)>k) { selected=skip; break; }
        }
        RelaxationGraph rebuilt(cross_);
        for (int j=0;j<static_cast<int>(merges.size());j++) {
            if (j==selected) continue;
            if (!rebuilt.merge(merges[j].first,merges[j].second)) {
                // Some contractions require the removed edge; fail safe by undoing LIFO.
                graph.unmerge();
                ++relaxation_full_unmerges_;
                return true;
            }
        }
        graph=std::move(rebuilt);
        ++relaxation_full_unmerges_;
        return true;
    }

    // Exact branch-and-bound proof by graph contraction, progressively undoing
    // merges whenever the relaxed decision problem is feasible. An infeasible
    // minor certifies infeasibility of the ORIGINAL graph for the same target.
    DecisionResult full_relaxation_decision(int k, std::atomic<std::uint64_t>& global_nodes) {
        const auto t0 = Clock::now();
        ++relaxation_probes_;
        RelaxationGraph graph(cross_);
        const int remaining_needed = std::max(0, graph.active.count() - k);
        const int quota = opt_.relax_max_merges > 0 ? std::min(opt_.relax_max_merges, remaining_needed) : remaining_needed;
        // Share is GLOBAL across all decision targets, not a fresh allowance
        // for every iteration of k.
        const bool bounded_share = opt_.time_limit_seconds > 0.0;
        const double total_relax_budget = bounded_share
            ? opt_.time_limit_seconds * opt_.relax_max_time_share : 0.0;
        const double remaining_relax_budget = bounded_share
            ? std::max(0.0, total_relax_budget - relaxation_full_seconds_) : 0.0;
        const std::optional<Clock::time_point> relax_deadline = bounded_share
            ? std::make_optional(t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(remaining_relax_budget)))
            : std::nullopt;
        auto stage_expired = [&]() {
            return timed_out() || (relax_deadline && Clock::now() >= *relax_deadline);
        };
        for (int i = 0; i < quota; ++i) {
            if (stage_expired() || node_limited(global_nodes.load(std::memory_order_relaxed))) break;
            auto [a,b] = graph.choose_original_pair(stage_expired);
            if (a < 0 || !graph.merge(a,b)) break;
            ++relaxation_full_merges_;
        }
        if (!graph.valid()) throw std::runtime_error("Invalid contracted graph symmetry/loops");
        while (!graph.log.empty()) {
            if (timed_out()) {
                relaxed_proof_status_ = "TIME_LIMIT";
                relaxation_full_seconds_ += elapsed_since(t0);
                return {DecisionResult::Status::Timeout};
            }
            if (node_limited(global_nodes.load(std::memory_order_relaxed))) {
                relaxed_proof_status_ = "NODE_LIMIT";
                relaxation_full_seconds_ += elapsed_since(t0);
                return {DecisionResult::Status::NodeLimit};
            }
            if (stage_expired()) break;
            std::vector<int> contracted_order;
            DecisionResult dr;
            try {
                dr = solve_contracted_exact(graph, k, global_nodes, contracted_order, relax_deadline);
            } catch (const std::bad_alloc&) {
                relaxed_proof_status_ = "UNKNOWN_RESOURCE";
                break;  // No proof; original-graph fallback remains mandatory.
            }
            if (dr.status == DecisionResult::Status::Infeasible) {
                relaxed_proof_status_ = "INFEASIBLE_PROVED";
                proof_source_ = "RELAXED_GRAPH_EXACT";
                relaxation_full_seconds_ += elapsed_since(t0);
                return dr;
            }
            if (dr.status == DecisionResult::Status::RelaxBudget) {
                relaxed_proof_status_ = "UNKNOWN_RELAX_BUDGET";
                break;
            }
            if (dr.status == DecisionResult::Status::Timeout || dr.status == DecisionResult::Status::NodeLimit) {
                relaxed_proof_status_ = dr.status == DecisionResult::Status::Timeout ? "TIME_LIMIT" : "NODE_LIMIT";
                relaxation_full_seconds_ += elapsed_since(t0);
                return dr;
            }
            if (dr.status == DecisionResult::Status::Feasible) {
                const auto lifted = extend_relaxed_order(contracted_order, graph.log);
                if (validate_permutation(lifted) && evaluate_pattern_order_as_customer_order(lifted) <= k) {
                    decision_found_order_ = lifted;
                    ++relaxation_successes_;
                    relaxed_proof_status_ = "FEASIBLE_LIFTED";
                    proof_source_ = "RELAXED_WITNESS_VALIDATED";
                    relaxation_full_seconds_ += elapsed_since(t0);
                    return dr;
                }
            }
            selective_unmerge(graph, contracted_order, k, stage_expired);
            if (!graph.valid()) throw std::runtime_error("Contracted graph undo invariant failed");
        }
        if (relaxed_proof_status_ != "UNKNOWN_RELAX_BUDGET" && relaxed_proof_status_ != "UNKNOWN_RESOURCE")
            relaxed_proof_status_ = stage_expired() ? "UNKNOWN_RELAX_BUDGET" : "UNKNOWN_FALLBACK";
        original_fallback_used_ = true;
        relaxation_full_seconds_ += elapsed_since(t0);
        return {DecisionResult::Status::Infeasible}; // Sentinel only: caller MUST fall back; not a certificate.
    }

    bool relaxation_probe(int k, std::vector<int>& found_order, ProfileStats* prof = nullptr) {
        if (opt_.relaxation_mode == "off" || opt_.relaxation_mode == "full") return false;
        if (opt_.relaxation_mode != "root-merge-probe") throw std::runtime_error("Invalid relaxation mode: " + opt_.relaxation_mode);
        const auto t0 = Clock::now();
        ++relaxation_probes_;
        std::vector<Bitset> local_cross = cross_;
        Bitset custs = all_patterns_;
        Bitset custs_open(inst_.n_patterns);
        std::vector<std::pair<int,int>> merges;
        merges.reserve(static_cast<size_t>(std::max(0, inst_.n_patterns - k)));

        while (custs.count() > k) {
            // Legacy probe must also respect the global wall-clock budget;
            // without this guard it can overrun a small --time-limit by minutes.
            if (timed_out()) break;
            std::vector<double> w(static_cast<size_t>(inst_.n_patterns), 0.0);
            custs.for_each_set_bit([&](int i) {
                const int denom = std::max(1, local_cross[i].minus(custs_open).count());
                w[static_cast<size_t>(i)] = 1.0 / static_cast<double>(denom);
            });
            double safest = std::numeric_limits<double>::infinity();
            int best_i = -1;
            int best_j = -1;
            custs.for_each_set_bit([&](int i) {
                if (timed_out()) return;
                if (custs_open.test(i)) return;
                double total_danger = 0.0;
                double total_weight = 0.0;
                double most_dangerous = -1.0;
                int local_best_j = -1;
                custs.for_each_set_bit([&](int j) {
                    if (i == j || !local_cross[i].test(j)) return;
                    const double danger = w[static_cast<size_t>(j)] * static_cast<double>(local_cross[i].minus(custs_open).minus(local_cross[j]).count());
                    total_danger += danger;
                    total_weight += w[static_cast<size_t>(j)];
                    if (danger > most_dangerous) {
                        most_dangerous = danger;
                        local_best_j = j;
                    }
                });
                if (local_best_j < 0 || total_weight <= 0.0) return;
                total_danger -= most_dangerous;
                const double safety = total_danger / total_weight;
                if (safety < safest) {
                    safest = safety;
                    best_i = i;
                    best_j = local_best_j;
                }
            });
            if (best_i < 0 || best_j < 0) break;

            for (int i = 0; i < inst_.n_patterns; ++i) {
                if (local_cross[best_i].test(i)) local_cross[i].set(best_j);
            }
            local_cross[best_j].inplace_or(local_cross[best_i]);
            custs.clear(best_i);
            custs_open.set(best_i);
            merges.emplace_back(best_i, best_j);
        }

        relaxation_merges_ += merges.size();
        std::vector<int> candidate = greedy_order_from_cross(local_cross);
        std::vector<char> present(static_cast<size_t>(inst_.n_patterns), 0);
        for (int p : candidate) if (p >= 0 && p < inst_.n_patterns) present[static_cast<size_t>(p)] = 1;
        for (auto it = merges.rbegin(); it != merges.rend(); ++it) {
            const int removed = it->first;
            const int merged = it->second;
            if (present[static_cast<size_t>(removed)]) continue;
            auto pos = std::find(candidate.begin(), candidate.end(), merged);
            if (pos == candidate.end()) candidate.push_back(removed);
            else candidate.insert(pos, removed);
            present[static_cast<size_t>(removed)] = 1;
        }
        for (int p = 0; p < inst_.n_patterns; ++p) if (!present[static_cast<size_t>(p)]) candidate.push_back(p);

        const int value = evaluate_pattern_order_as_customer_order(candidate);
        if (opt_.profile) {
            const double dt = elapsed_since(t0);
            profile_.relaxation_probe_seconds += dt;
            if (prof) prof->relaxation_probe_seconds += dt;
        }
        if (value <= k) {
            found_order = std::move(candidate);
            ++relaxation_successes_;
            return true;
        }
        return false;
    }

    bool timed_out() const {
        if (opt_.time_limit_seconds <= 0.0) return false;
        const double elapsed = std::chrono::duration<double>(Clock::now() - start_).count();
        return elapsed >= opt_.time_limit_seconds;
    }

    bool node_limited(std::uint64_t n) const {
        return opt_.node_limit > 0 && n >= opt_.node_limit;
    }

    bool force_close(Bitset& opened, Bitset& remaining, std::vector<int>& closed_order) const {
        bool changed = true;
        while (changed) {
            changed = false;
            std::vector<int> to_close;
            remaining.for_each_set_bit([&](int p) {
                if (cross_[p].subset_of(opened)) to_close.push_back(p);
            });
            if (!to_close.empty()) {
                changed = true;
                for (int p : to_close) {
                    if (remaining.test(p)) {
                        remaining.clear(p);
                        closed_order.push_back(p);
                    }
                }
            }
        }
        return remaining.empty();
    }

    std::vector<Move> generate_moves(const Bitset& opened, const Bitset& remaining, int free_stacks) const {
        std::vector<Move> moves;
        moves.reserve(static_cast<size_t>(remaining.count()));

        if (opt_.group_moves) {
            std::vector<Move> raw;
            raw.reserve(static_cast<size_t>(remaining.count()));
            remaining.for_each_set_bit([&](int p) {
                Bitset opens = cross_[p].minus(opened);
                const int oc = opens.count();
                if (oc <= free_stacks) {
                    Move m;
                    m.representative = p;
                    m.open_set = std::move(opens);
                    m.close_set = Bitset(inst_.n_patterns);
                    m.close_set.set(p);
                    m.open_count = oc;
                    m.close_count = 1;
                    raw.push_back(std::move(m));
                }
            });
            std::sort(raw.begin(), raw.end(), [](const Move& a, const Move& b) {
                if (a.open_count != b.open_count) return a.open_count < b.open_count;
                return a.open_set.lex_less(b.open_set);
            });
            for (Move& m : raw) {
                if (moves.empty() || m.open_set != moves.back().open_set) {
                    moves.push_back(std::move(m));
                } else {
                    moves.back().close_set.set(m.representative);
                    ++moves.back().close_count;
                    if (m.representative < moves.back().representative) moves.back().representative = m.representative;
                }
            }
        } else {
            remaining.for_each_set_bit([&](int p) {
                Bitset opens = cross_[p].minus(opened);
                const int oc = opens.count();
                if (oc <= free_stacks) {
                    Move m;
                    m.representative = p;
                    m.open_set = std::move(opens);
                    m.close_set = Bitset(inst_.n_patterns);
                    m.close_set.set(p);
                    m.open_count = oc;
                    m.close_count = 1;
                    moves.push_back(std::move(m));
                }
            });
        }

        if (opt_.sort_moves) {
            for (Move& m : moves) m.score = static_cast<double>(m.open_count) - static_cast<double>(m.close_count);
            std::stable_sort(moves.begin(), moves.end(), [](const Move& a, const Move& b) {
                if (a.score != b.score) return a.score < b.score;
                if (a.open_count != b.open_count) return a.open_count < b.open_count;
                if (a.close_count != b.close_count) return a.close_count > b.close_count;
                return a.representative < b.representative;
            });
        }
        return moves;
    }

    bool residual_bound_prunes(const Bitset& opened, const Bitset& remaining,
                               int k, std::uint64_t& calls,
                               std::uint64_t& core_tests, std::uint64_t& prunes,
                               double& seconds) const {
        if (opt_.residual_lb_mode == "off" || remaining.empty()) return false;
        const auto start = Clock::now();
        ++calls;
        bool fail = ResidualBounds::first_closure(cross_, opened, remaining) > k;
        if (!fail && opt_.residual_lb_mode == "core" &&
            (opt_.residual_core_max_vertices == 0 || remaining.count() <= opt_.residual_core_max_vertices)) {
            ++core_tests;
            fail = ResidualBounds::core_exceeds_target(cross_, remaining, k);
        }
        if (fail) ++prunes;
        if (opt_.profile) seconds += std::chrono::duration<double>(Clock::now() - start).count();
        return fail;
    }

    DecisionResult dfs(Bitset& opened, Bitset& remaining, std::vector<int>& closed_order, int k, SearchContext& ctx) {
        if (ctx.stop && ctx.stop->load(std::memory_order_relaxed)) return {DecisionResult::Status::Feasible};

        const std::uint64_t current_nodes = ctx.global_nodes ? ctx.global_nodes->fetch_add(1, std::memory_order_relaxed) + 1ULL : ++nodes_;
        ++ctx.local_nodes;
        if (ctx.relaxed_phase) nodes_relaxed_.fetch_add(1, std::memory_order_relaxed);
        else nodes_customer_.fetch_add(1, std::memory_order_relaxed);

        if (node_limited(current_nodes)) return {DecisionResult::Status::NodeLimit};
        if ((current_nodes & 1023ULL) == 0ULL || opt_.time_limit_seconds > 0.0) {
            if (timed_out()) return {DecisionResult::Status::Timeout};
        }
        if (ctx.relax_deadline && Clock::now() >= *ctx.relax_deadline)
            return {DecisionResult::Status::RelaxBudget};

        const size_t old_order_size = closed_order.size();
        auto t_force = Clock::now();
        const bool closed_all = force_close(opened, remaining, closed_order);
        if (opt_.profile) ctx.profile.force_close_seconds += elapsed_since(t_force);
        if (closed_all) {
            if (ctx.stop) ctx.stop->store(true, std::memory_order_relaxed);
            if (ctx.result_mutex && ctx.shared_found_order) {
                std::lock_guard<std::mutex> lock(*ctx.result_mutex);
                if (ctx.shared_found_order->empty()) *ctx.shared_found_order = closed_order;
            } else {
                decision_found_order_ = closed_order;
            }
            while (closed_order.size() > old_order_size) closed_order.pop_back();
            return {DecisionResult::Status::Feasible};
        }

        Bitset key_p = opened.bit_and(remaining);
        const int now_open = key_p.count();
        if (now_open >= k) {
            while (closed_order.size() > old_order_size) closed_order.pop_back();
            return {DecisionResult::Status::Infeasible};
        }
        const int free_stacks = k - now_open;
        if (residual_bound_prunes(opened, remaining, k,
                                  ctx.local_residual_calls, ctx.local_residual_core_tests,
                                  ctx.local_residual_prunes, ctx.profile.residual_lb_seconds)) {
            while (closed_order.size() > old_order_size) closed_order.pop_back();
            return {DecisionResult::Status::Infeasible};
        }

        bool dominated_state = false;
        if (opt_.use_hash) {
            auto t_hash = Clock::now();
            dominated_state = ctx.table.dominated(key_p, remaining);
            if (opt_.profile) ctx.profile.hash_lookup_seconds += elapsed_since(t_hash);
        }
        if (dominated_state) {
            ++ctx.local_hash_hits;
            while (closed_order.size() > old_order_size) closed_order.pop_back();
            return {DecisionResult::Status::Infeasible};
        }

        auto t_moves = Clock::now();
        std::vector<Move> moves = generate_moves(opened, remaining, free_stacks);
        if (opt_.profile) ctx.profile.move_generation_seconds += elapsed_since(t_moves);
        if (moves.empty()) {
            if (opt_.use_hash) { auto t_hi = Clock::now(); ctx.table.insert(key_p, remaining); if (opt_.profile) ctx.profile.hash_insert_seconds += elapsed_since(t_hi); }
            while (closed_order.size() > old_order_size) closed_order.pop_back();
            return {DecisionResult::Status::Infeasible};
        }

        for (const Move& m : moves) {
            if (ctx.stop && ctx.stop->load(std::memory_order_relaxed)) {
                while (closed_order.size() > old_order_size) closed_order.pop_back();
                return {DecisionResult::Status::Feasible};
            }
            Bitset next_opened = opened.bit_or(m.open_set);
            Bitset next_remaining = remaining.minus(m.close_set);
            const size_t before_move_order = closed_order.size();
            m.close_set.for_each_set_bit([&](int p) {
                if (remaining.test(p)) closed_order.push_back(p);
            });
            DecisionResult child = dfs(next_opened, next_remaining, closed_order, k, ctx);
            while (closed_order.size() > before_move_order) closed_order.pop_back();
            if (child.status != DecisionResult::Status::Infeasible) {
                while (closed_order.size() > old_order_size) closed_order.pop_back();
                return child;
            }
        }

        if (opt_.use_hash) { auto t_hi = Clock::now(); ctx.table.insert(key_p, remaining); if (opt_.profile) ctx.profile.hash_insert_seconds += elapsed_since(t_hi); }
        while (closed_order.size() > old_order_size) closed_order.pop_back();
        return {DecisionResult::Status::Infeasible};
    }

    DecisionResult expand_frontier(Bitset& opened, Bitset& remaining, std::vector<int>& closed_order,
                                   int k, int depth, std::vector<FrontierState>& frontier,
                                   std::atomic<std::uint64_t>& global_nodes,
                                   std::vector<int>& immediate_found_order,
                                   const std::optional<Clock::time_point>& relax_deadline = std::nullopt,
                                   size_t frontier_cap = 0, size_t reserved_siblings = 0) {
        const std::uint64_t current_nodes = global_nodes.fetch_add(1, std::memory_order_relaxed) + 1ULL;
        nodes_frontier_.fetch_add(1, std::memory_order_relaxed);
        if (node_limited(current_nodes)) return {DecisionResult::Status::NodeLimit};
        if (relax_deadline && Clock::now() >= *relax_deadline)
            return {DecisionResult::Status::RelaxBudget};
        if ((current_nodes & 1023ULL) == 0ULL || opt_.time_limit_seconds > 0.0) {
            if (timed_out()) return {DecisionResult::Status::Timeout};
        }

        const size_t old_order_size = closed_order.size();
        auto t_force = Clock::now();
        const bool closed_all = force_close(opened, remaining, closed_order);
        if (opt_.profile) profile_.force_close_seconds += elapsed_since(t_force);
        if (closed_all) {
            immediate_found_order = closed_order;
            while (closed_order.size() > old_order_size) closed_order.pop_back();
            return {DecisionResult::Status::Feasible};
        }

        Bitset key_p = opened.bit_and(remaining);
        const int now_open = key_p.count();
        if (now_open >= k) {
            while (closed_order.size() > old_order_size) closed_order.pop_back();
            return {DecisionResult::Status::Infeasible};
        }

        if (residual_bound_prunes(opened, remaining, k,
                                  residual_lb_calls_, residual_lb_core_tests_, residual_lb_prunes_,
                                  profile_.residual_lb_seconds)) {
            while (closed_order.size() > old_order_size) closed_order.pop_back();
            return {DecisionResult::Status::Infeasible};
        }

        if (depth <= 0) {
            if (frontier_cap && frontier.size() + reserved_siblings + 1 > frontier_cap)
                throw std::logic_error("Incomplete contracted frontier: capacity reservation invariant");
            frontier.push_back({opened, remaining, closed_order});
            while (closed_order.size() > old_order_size) closed_order.pop_back();
            return {DecisionResult::Status::Infeasible};
        }

        const int free_stacks = k - now_open;
        std::vector<Move> moves = generate_moves(opened, remaining, free_stacks);
        if (moves.empty()) {
            while (closed_order.size() > old_order_size) closed_order.pop_back();
            return {DecisionResult::Status::Infeasible};
        }
        // Reserve one leaf for each still-unexplored sibling. If splitting
        // could exhaust the cap, keep this WHOLE subtree as a frontier leaf.
        // Thus no state is discarded and an all-infeasible result stays exact.
        if (frontier_cap && frontier.size() + reserved_siblings + moves.size() > frontier_cap) {
            if (frontier.size() + reserved_siblings + 1 > frontier_cap)
                throw std::logic_error("Incomplete contracted frontier: insufficient capacity");
            frontier.push_back({opened, remaining, closed_order});
            while (closed_order.size() > old_order_size) closed_order.pop_back();
            return {DecisionResult::Status::Infeasible};
        }

        for (size_t child_index = 0; child_index < moves.size(); ++child_index) {
            const Move& m = moves[child_index];
            Bitset next_opened = opened.bit_or(m.open_set);
            Bitset next_remaining = remaining.minus(m.close_set);
            const size_t before_move_order = closed_order.size();
            m.close_set.for_each_set_bit([&](int p) {
                if (remaining.test(p)) closed_order.push_back(p);
            });
            DecisionResult dr = expand_frontier(next_opened, next_remaining, closed_order, k, depth - 1, frontier, global_nodes, immediate_found_order, relax_deadline,
                                                frontier_cap, reserved_siblings + moves.size() - child_index - 1);
            while (closed_order.size() > before_move_order) closed_order.pop_back();
            if (dr.status == DecisionResult::Status::Feasible || dr.status == DecisionResult::Status::Timeout ||
                dr.status == DecisionResult::Status::NodeLimit || dr.status == DecisionResult::Status::RelaxBudget) {
                while (closed_order.size() > old_order_size) closed_order.pop_back();
                return dr;
            }
        }
        while (closed_order.size() > old_order_size) closed_order.pop_back();
        return {DecisionResult::Status::Infeasible};
    }


    void force_empty_item_closures(const Bitset& processed, Bitset& closed_items) const {
        Bitset open_items = all_pieces_.minus(closed_items);
        std::vector<int> to_close;
        open_items.for_each_set_bit([&](int item) {
            if (plays_[item].subset_of(processed)) to_close.push_back(item);
        });
        for (int item : to_close) closed_items.set(item);
    }

    Bitset union_items_of_patterns(const Bitset& patterns) const {
        Bitset items(inst_.n_pieces);
        patterns.for_each_set_bit([&](int p) { items.inplace_or(pattern_pieces_[p]); });
        return items;
    }

    std::vector<int> order_block_patterns_for_closure(Bitset block, Bitset processed, const Bitset& closed_items) const {
        std::vector<int> ordered;
        ordered.reserve(static_cast<size_t>(block.count()));
        while (!block.empty()) {
            int best = -1;
            int best_closes = -1;
            int best_new_items = std::numeric_limits<int>::max();
            block.for_each_set_bit([&](int p) {
                Bitset candidate_processed = processed;
                candidate_processed.set(p);
                int closes = 0;
                Bitset candidates = all_pieces_.minus(closed_items);
                candidates.for_each_set_bit([&](int item) {
                    if (plays_[item].subset_of(candidate_processed)) ++closes;
                });
                const int new_items = pattern_pieces_[p].minus(union_items_of_patterns(processed)).count();
                if (closes > best_closes ||
                    (closes == best_closes && new_items < best_new_items) ||
                    (closes == best_closes && new_items == best_new_items && p < best)) {
                    best = p;
                    best_closes = closes;
                    best_new_items = new_items;
                }
            });
            if (best < 0) break;
            ordered.push_back(best);
            processed.set(best);
            block.clear(best);
        }
        return ordered;
    }

    struct ClosureCandidate {
        int item = -1;
        Bitset block;
        int block_size = 0;
        int peak = 0;
        int closes = 0;
        int new_items = 0;
    };

    std::vector<ClosureCandidate> generate_closure_candidates(const Bitset& processed,
                                                              const Bitset& closed_items,
                                                              const Bitset& opened_active,
                                                              int k) const {
        std::vector<ClosureCandidate> candidates;
        Bitset open_items = all_pieces_.minus(closed_items);
        open_items.for_each_set_bit([&](int item) {
            Bitset block = plays_[item].minus(processed);
            if (block.empty()) return;
            Bitset block_items = union_items_of_patterns(block);
            Bitset active_before_closure = opened_active.bit_or(block_items).minus(closed_items);
            const int peak = active_before_closure.count();
            if (peak > k) return;
            Bitset new_processed = processed.bit_or(block);
            int closes = 0;
            Bitset still_open_items = all_pieces_.minus(closed_items);
            still_open_items.for_each_set_bit([&](int cand_item) {
                if (plays_[cand_item].subset_of(new_processed)) ++closes;
            });
            ClosureCandidate c;
            c.item = item;
            c.block = std::move(block);
            c.block_size = c.block.count();
            c.peak = peak;
            c.closes = closes;
            c.new_items = block_items.minus(opened_active).minus(closed_items).count();
            candidates.push_back(std::move(c));
        });
        std::stable_sort(candidates.begin(), candidates.end(), [](const ClosureCandidate& a, const ClosureCandidate& b) {
            if (a.peak != b.peak) return a.peak < b.peak;
            if (a.new_items != b.new_items) return a.new_items < b.new_items;
            if (a.closes != b.closes) return a.closes > b.closes;
            if (a.block_size != b.block_size) return a.block_size < b.block_size;
            return a.item < b.item;
        });
        return candidates;
    }

    // Make one canonical closure transition, identical for frontier construction
    // and the recursive DFS. This ensures that depth/cap affect speed only.
    ClosureFrontierState closure_successor(const ClosureFrontierState& st,
                                           const ClosureCandidate& cand) const {
        ClosureFrontierState next;
        next.processed = st.processed.bit_or(cand.block);
        Bitset block_items = union_items_of_patterns(cand.block);
        Bitset active_before_closure = st.opened_active.bit_or(block_items).minus(st.closed_items);
        next.closed_items = st.closed_items;
        Bitset remaining_items = all_pieces_.minus(next.closed_items);
        remaining_items.for_each_set_bit([&](int item) {
            if (plays_[item].subset_of(next.processed)) next.closed_items.set(item);
        });
        next.opened_active = active_before_closure.minus(next.closed_items);
        next.pattern_order = st.pattern_order;
        std::vector<int> block_order = order_block_patterns_for_closure(cand.block, st.processed, st.closed_items);
        next.pattern_order.insert(next.pattern_order.end(), block_order.begin(), block_order.end());
        return next;
    }

    DecisionResult close_stack_dfs(ClosureFrontierState& state, int k,
                                   ClosureSearchContext& ctx,
                                   std::atomic<std::uint64_t>& global_nodes,
                                   std::vector<int>& found_order) {
        // Stop can indicate either a verified solution or an interrupted sibling.
        // In both cases no incomplete subtree is treated as a proof.
        if (ctx.stop && ctx.stop->load(std::memory_order_acquire))
            return {DecisionResult::Status::Infeasible};
        const std::uint64_t current_nodes = global_nodes.fetch_add(1, std::memory_order_relaxed) + 1ULL;
        ++ctx.local_nodes;
        nodes_closure_.fetch_add(1, std::memory_order_relaxed);
        if (node_limited(current_nodes)) return {DecisionResult::Status::NodeLimit};
        if ((current_nodes & 1023ULL) == 0ULL || opt_.time_limit_seconds > 0.0) {
            if (timed_out()) return {DecisionResult::Status::Timeout};
        }
        force_empty_item_closures(state.processed, state.closed_items);
        if (state.processed == all_patterns_) {
            if (validate_permutation(state.pattern_order) &&
                evaluate_pattern_order_as_customer_order(state.pattern_order) <= k) {
                found_order = state.pattern_order;
                ++ctx.local_successes;
                return {DecisionResult::Status::Feasible};
            }
            return {DecisionResult::Status::Infeasible};
        }
        if (ctx.dead.find(state.processed) != ctx.dead.end())
            return {DecisionResult::Status::Infeasible};
        std::vector<ClosureCandidate> candidates = generate_closure_candidates(
            state.processed, state.closed_items, state.opened_active, k);
        for (const ClosureCandidate& cand : candidates) {
            ClosureFrontierState next = closure_successor(state, cand);
            DecisionResult child = close_stack_dfs(next, k, ctx, global_nodes, found_order);
            if (child.status != DecisionResult::Status::Infeasible) return child;
            // A different worker might have found a witness, interrupted, or
            // exhausted the global budget; do not memoize this partial state.
            if (ctx.stop && ctx.stop->load(std::memory_order_acquire))
                return {DecisionResult::Status::Infeasible};
        }
        if (!ctx.stop || !ctx.stop->load(std::memory_order_acquire)) {
            ctx.dead.insert(state.processed);
            ctx.local_dead_peak = std::max<std::uint64_t>(ctx.local_dead_peak, ctx.dead.size());
        }
        return {DecisionResult::Status::Infeasible};
    }

    // Expand a bounded, *complete* frontier. If splitting all siblings would
    // exceed the cap, retain their parent subtree as one task instead.
    DecisionResult expand_closure_frontier(ClosureFrontierState state, int k, int depth,
                                           std::vector<ClosureFrontierState>& frontier,
                                           std::atomic<std::uint64_t>& global_nodes,
                                           std::vector<int>& immediate_found,
                                           size_t reserved_siblings = 0) {
        const auto current_nodes = global_nodes.fetch_add(1, std::memory_order_relaxed) + 1ULL;
        ++closure_branch_nodes_;  // frontier built exclusively by the calling thread
        nodes_frontier_.fetch_add(1, std::memory_order_relaxed);
        if (node_limited(current_nodes)) return {DecisionResult::Status::NodeLimit};
        if (timed_out()) return {DecisionResult::Status::Timeout};
        force_empty_item_closures(state.processed, state.closed_items);
        if (state.processed == all_patterns_) {
            if (validate_permutation(state.pattern_order) &&
                evaluate_pattern_order_as_customer_order(state.pattern_order) <= k) {
                immediate_found = state.pattern_order;
                ++closure_branch_successes_;
                return {DecisionResult::Status::Feasible};
            }
            return {DecisionResult::Status::Infeasible};
        }
        const size_t cap = static_cast<size_t>(opt_.closure_frontier_cap);
        if (depth <= 0) {
            if (frontier.size() + reserved_siblings + 1 > cap)
                throw std::logic_error("Incomplete close-stack frontier: capacity invariant");
            frontier.push_back(std::move(state));
            return {DecisionResult::Status::Infeasible};
        }
        auto candidates = generate_closure_candidates(state.processed, state.closed_items, state.opened_active, k);
        if (candidates.empty()) return {DecisionResult::Status::Infeasible};
        if (frontier.size() + reserved_siblings + candidates.size() > cap) {
            if (frontier.size() + reserved_siblings + 1 > cap)
                throw std::logic_error("Incomplete close-stack frontier: insufficient capacity");
            frontier.push_back(std::move(state));
            return {DecisionResult::Status::Infeasible};
        }
        for (size_t i = 0; i < candidates.size(); ++i) {
            auto next = closure_successor(state, candidates[i]);
            auto dr = expand_closure_frontier(
                std::move(next), k, depth - 1, frontier, global_nodes, immediate_found,
                reserved_siblings + candidates.size() - i - 1);
            if (dr.status != DecisionResult::Status::Infeasible) return dr;
        }
        return {DecisionResult::Status::Infeasible};
    }

    DecisionResult decision_search_close_stack(int k, std::atomic<std::uint64_t>& global_nodes) {
        ++closure_branch_calls_;
        ClosureFrontierState root{Bitset(inst_.n_patterns), Bitset(inst_.n_pieces),
                                  Bitset(inst_.n_pieces), {}};
        const int requested_threads = effective_closure_threads();
        const int depth = effective_closure_parallel_depth();
        if (requested_threads <= 1 || depth == 0) {
            ClosureSearchContext ctx;
            ctx.dead.reserve(4096);
            std::vector<int> found;
            auto t_serial=Clock::now();
            auto dr = close_stack_dfs(root, k, ctx, global_nodes, found);
            add_workers(workers_closure_, {WorkerMetric{ctx.local_nodes, 1, elapsed_since(t_serial), 0.0}}, elapsed_since(t_serial));
            closure_branch_nodes_ += ctx.local_nodes;
            closure_branch_successes_ += ctx.local_successes;
            closure_branch_dead_states_ = std::max(closure_branch_dead_states_, ctx.local_dead_peak);
            if (dr.status == DecisionResult::Status::Feasible) decision_found_order_ = std::move(found);
            return dr;
        }

        std::vector<ClosureFrontierState> frontier;
        frontier.reserve(static_cast<size_t>(std::min(4096, opt_.closure_frontier_cap)));
        std::vector<int> immediate_found;
        const auto start = Clock::now();
        auto front = expand_closure_frontier(root, k, depth, frontier, global_nodes, immediate_found);
        if (opt_.profile) profile_.frontier_seconds += elapsed_since(start);
        closure_frontier_states_ += frontier.size();
        if (front.status == DecisionResult::Status::Feasible) {
            decision_found_order_ = std::move(immediate_found);
            return front;
        }
        if (front.status == DecisionResult::Status::Timeout || front.status == DecisionResult::Status::NodeLimit)
            return front;
        if (frontier.empty()) return {DecisionResult::Status::Infeasible};

        const int nworkers = std::min<int>(requested_threads, static_cast<int>(frontier.size()));
        closure_max_workers_ = std::max(closure_max_workers_, nworkers);
        if (nworkers > 1) ++closure_parallel_calls_;
        std::atomic<bool> stop{false};
        std::atomic<int> terminal{0}; // 1 = time, 2 = nodes
        std::mutex result_mutex;
        std::vector<int> shared_found;
        std::vector<ClosureSearchContext> contexts(static_cast<size_t>(nworkers));
        for (auto& ctx : contexts) {
            ctx.stop = &stop;
            ctx.dead.reserve(4096);
        }
        std::vector<WorkerMetric> worker_metrics(static_cast<size_t>(nworkers));
        const auto dispatch_start=Clock::now();
        dispatch_frontier(frontier, nworkers, stop, [&](int id, const ClosureFrontierState& task) {
            WorkerMetric &wm=worker_metrics[static_cast<size_t>(id)];
            auto task_start=Clock::now();
            auto before_nodes=contexts[static_cast<size_t>(id)].local_nodes;
            ClosureFrontierState state = task;
            std::vector<int> found;
            auto dr = close_stack_dfs(state, k, contexts[static_cast<size_t>(id)], global_nodes, found);
            wm.tasks++; wm.nodes += contexts[static_cast<size_t>(id)].local_nodes - before_nodes;
            wm.active_seconds += elapsed_since(task_start);
            if (dr.status == DecisionResult::Status::Feasible && !found.empty()) {
                {
                    std::lock_guard<std::mutex> lock(result_mutex);
                    if (shared_found.empty()) shared_found = std::move(found);
                }
                stop.store(true, std::memory_order_release);
            } else if (dr.status == DecisionResult::Status::Timeout ||
                       dr.status == DecisionResult::Status::NodeLimit) {
                int expected = 0;
                terminal.compare_exchange_strong(expected,
                    dr.status == DecisionResult::Status::Timeout ? 1 : 2);
                stop.store(true, std::memory_order_release);
            }
        });
        add_workers(workers_closure_,worker_metrics,elapsed_since(dispatch_start));
        for (const auto& ctx : contexts) {
            closure_branch_nodes_ += ctx.local_nodes;
            closure_branch_successes_ += ctx.local_successes;
            closure_branch_dead_states_ = std::max(closure_branch_dead_states_, ctx.local_dead_peak);
        }
        if (!shared_found.empty()) {
            decision_found_order_ = std::move(shared_found);
            return {DecisionResult::Status::Feasible};
        }
        const int term = terminal.load(std::memory_order_acquire);
        if (term == 1) return {DecisionResult::Status::Timeout};
        if (term == 2) return {DecisionResult::Status::NodeLimit};
        if (stop.load(std::memory_order_acquire))
            throw std::runtime_error("Close-stack frontier interrupted without witness or resource status");
        return {DecisionResult::Status::Infeasible};
    }

    DecisionResult decision_search_serial(int k, std::atomic<std::uint64_t>& global_nodes) {
        std::atomic<bool> stop(false);
        std::vector<int> found;
        std::mutex result_mutex;
        SearchContext ctx;
        ctx.stop = &stop;
        ctx.global_nodes = &global_nodes;
        ctx.result_mutex = &result_mutex;
        ctx.shared_found_order = &found;
        Bitset opened(inst_.n_patterns);
        Bitset remaining = all_patterns_;
        std::vector<int> closed_order;
        closed_order.reserve(inst_.n_patterns);
        auto serial_start = Clock::now();
        DecisionResult dr = dfs(opened, remaining, closed_order, k, ctx);
        add_workers(workers_customer_, {WorkerMetric{ctx.local_nodes, 1, elapsed_since(serial_start), 0.0}}, elapsed_since(serial_start));
        residual_lb_calls_ += ctx.local_residual_calls;
        residual_lb_core_tests_ += ctx.local_residual_core_tests;
        residual_lb_prunes_ += ctx.local_residual_prunes;
        hash_hits_ += ctx.local_hash_hits;
        hash_entries_ += ctx.table.entries();
        dominance_keys_ += ctx.table.keys();
        if (opt_.profile) profile_.add(ctx.profile);
        if (dr.status == DecisionResult::Status::Feasible && !found.empty()) decision_found_order_ = found;
        return dr;
    }

    DecisionResult decision_search_parallel(int k, std::atomic<std::uint64_t>& global_nodes) {
        std::vector<FrontierState> frontier;
        frontier.reserve(4096);
        std::vector<int> immediate_found;
        Bitset opened(inst_.n_patterns);
        Bitset remaining = all_patterns_;
        std::vector<int> closed_order;
        closed_order.reserve(inst_.n_patterns);

        auto t_frontier = Clock::now();
        DecisionResult front = expand_frontier(opened, remaining, closed_order, k, opt_.parallel_depth, frontier, global_nodes, immediate_found);
        if (opt_.profile) profile_.frontier_seconds += elapsed_since(t_frontier);
        last_frontier_states_ += frontier.size();
        if (front.status == DecisionResult::Status::Feasible) {
            decision_found_order_ = immediate_found;
            return front;
        }
        if (front.status == DecisionResult::Status::Timeout || front.status == DecisionResult::Status::NodeLimit) return front;
        if (frontier.empty()) return {DecisionResult::Status::Infeasible};

        std::atomic<bool> stop(false);
        std::atomic<int> terminal(0); // 0 none, 1 timeout, 2 node limit
        std::mutex result_mutex;
        std::vector<int> shared_found_order;
        std::atomic<std::uint64_t> aggregate_hash_hits(0), aggregate_hash_entries(0), aggregate_keys(0);
        std::atomic<std::uint64_t> aggregate_residual_calls(0), aggregate_residual_core(0), aggregate_residual_prunes(0);
        ProfileStats aggregate_profile;

        const int nthreads = std::min<int>(opt_.threads, static_cast<int>(frontier.size()));
        std::vector<SearchContext> contexts(static_cast<size_t>(nthreads));
        for (auto& ctx : contexts) {
            ctx.stop = &stop;
            ctx.global_nodes = &global_nodes;
            ctx.result_mutex = &result_mutex;
            ctx.shared_found_order = &shared_found_order;
        }
        std::vector<WorkerMetric> worker_metrics(static_cast<size_t>(nthreads));
        const auto dispatch_start = Clock::now();
        dispatch_frontier(frontier, nthreads, stop, [&](int id, const FrontierState& task) {
            auto &wm = worker_metrics[static_cast<size_t>(id)];
            const auto task_start = Clock::now();
            const auto before_nodes = contexts[static_cast<size_t>(id)].local_nodes;
            FrontierState st = task;
            auto& ctx = contexts[static_cast<size_t>(id)];
            auto dr = dfs(st.opened, st.remaining, st.closed_order, k, ctx);
            wm.tasks++; wm.nodes += ctx.local_nodes - before_nodes;
            wm.active_seconds += elapsed_since(task_start);
            if (dr.status == DecisionResult::Status::Feasible) {
                stop.store(true, std::memory_order_release);
            } else if (dr.status == DecisionResult::Status::Timeout ||
                       dr.status == DecisionResult::Status::NodeLimit) {
                int expected = 0;
                terminal.compare_exchange_strong(expected,
                    dr.status == DecisionResult::Status::Timeout ? 1 : 2);
                stop.store(true, std::memory_order_release);
            }
        });
        add_workers(workers_customer_, worker_metrics, elapsed_since(dispatch_start));
        for (const auto& ctx : contexts) {
            aggregate_residual_calls.fetch_add(ctx.local_residual_calls, std::memory_order_relaxed);
            aggregate_residual_core.fetch_add(ctx.local_residual_core_tests, std::memory_order_relaxed);
            aggregate_residual_prunes.fetch_add(ctx.local_residual_prunes, std::memory_order_relaxed);
            aggregate_hash_hits.fetch_add(ctx.local_hash_hits, std::memory_order_relaxed);
            aggregate_hash_entries.fetch_add(ctx.table.entries(), std::memory_order_relaxed);
            aggregate_keys.fetch_add(ctx.table.keys(), std::memory_order_relaxed);
            if (opt_.profile) aggregate_profile.add(ctx.profile);
        }

        residual_lb_calls_ += aggregate_residual_calls.load(std::memory_order_relaxed);
        residual_lb_core_tests_ += aggregate_residual_core.load(std::memory_order_relaxed);
        residual_lb_prunes_ += aggregate_residual_prunes.load(std::memory_order_relaxed);
        hash_hits_ += aggregate_hash_hits.load(std::memory_order_relaxed);
        hash_entries_ += aggregate_hash_entries.load(std::memory_order_relaxed);
        dominance_keys_ += aggregate_keys.load(std::memory_order_relaxed);
        if (opt_.profile) profile_.add(aggregate_profile);

        if (!shared_found_order.empty()) {
            decision_found_order_ = shared_found_order;
            return {DecisionResult::Status::Feasible};
        }
        const int term = terminal.load(std::memory_order_relaxed);
        if (term == 1) return {DecisionResult::Status::Timeout};
        if (term == 2) return {DecisionResult::Status::NodeLimit};
        return {DecisionResult::Status::Infeasible};
    }

    DecisionResult decision_search_customer(int k, std::atomic<std::uint64_t>& global_nodes) {
        if (opt_.threads > 1 && opt_.parallel_depth > 0) return decision_search_parallel(k, global_nodes);
        return decision_search_serial(k, global_nodes);
    }

    DecisionResult decision_search(int k) {
        active_target_ = k;
        std::atomic<std::uint64_t> global_nodes(nodes_);
        if (opt_.relaxation_mode == "full") {
            DecisionResult relaxed = full_relaxation_decision(k, global_nodes);
            nodes_ = global_nodes.load(std::memory_order_relaxed);
            if (relaxed_proof_status_ == "INFEASIBLE_PROVED" ||
                relaxed_proof_status_ == "FEASIBLE_LIFTED" ||
                relaxed.status == DecisionResult::Status::Timeout ||
                relaxed.status == DecisionResult::Status::NodeLimit) return relaxed;
            // UNKNOWN_FALLBACK: never interpret the sentinel as a certificate.
        }
        std::vector<int> relaxed_order;
        if (relaxation_probe(k, relaxed_order)) {
            decision_found_order_ = std::move(relaxed_order);
            return {DecisionResult::Status::Feasible};
        }
        DecisionResult dr;
        if (opt_.branching_mode == "close-stack" || opt_.branching_mode == "auto") {
            dr = decision_search_close_stack(k, global_nodes);
            if (opt_.relaxation_mode == "full" && dr.status == DecisionResult::Status::Feasible &&
                evaluate_pattern_order_as_customer_order(decision_found_order_) > k) {
                // A pattern-order witness from the experimental closure layer
                // cannot establish feasibility for customer search unless its
                // value is rechecked in the ORIGINAL customer graph.
                dr.status = DecisionResult::Status::Infeasible;
                decision_found_order_.clear();
            }
            if (dr.status == DecisionResult::Status::Feasible ||
                dr.status == DecisionResult::Status::Timeout ||
                dr.status == DecisionResult::Status::NodeLimit) {
                nodes_ = global_nodes.load(std::memory_order_relaxed);
                if (opt_.relaxation_mode == "full" && dr.status == DecisionResult::Status::Feasible)
                    proof_source_ = "CLOSE_STACK_FEASIBLE";
                return dr;
            }
            // Exactness safeguard: a failed closure-order exploration is not used as a
            // proof of infeasibility. Non-feasibility is certified by the original
            // Chu--Stuckey customer-search backbone, preserving compatibility with
            // previous releases.
            ++closure_branch_fallbacks_;
        }
        dr = decision_search_customer(k, global_nodes);
        nodes_ = global_nodes.load(std::memory_order_relaxed);
        if (opt_.relaxation_mode == "full" && dr.status == DecisionResult::Status::Infeasible)
            proof_source_ = "ORIGINAL_CUSTOMER_EXACT";
        if (opt_.relaxation_mode == "full" && dr.status == DecisionResult::Status::Feasible)
            proof_source_ = "ORIGINAL_CUSTOMER_FEASIBLE";
        return dr;
    }

    SolveResult finalize(const std::string& status) const {
        SolveResult r;
        r.status = status;
        r.upper_bound = incumbent_;
        r.lower_bound = certified_lb_;
        r.applied_initial_lower_bound = applied_initial_lower_bound_;
        r.legacy_lower_bound = legacy_lower_bound_;
        r.model_cardinality_lower_bound = model_cardinality_lower_bound_;
        r.graph_min_degree_lower_bound = graph_min_degree_lower_bound_;
        r.graph_degeneracy_lower_bound = graph_degeneracy_lower_bound_;
        r.customer_graph_core_lower_bound = customer_graph_core_lower_bound_;
        r.residual_lb_mode = opt_.residual_lb_mode;
        r.residual_lb_calls = residual_lb_calls_;
        r.residual_lb_core_tests = residual_lb_core_tests_;
        r.residual_lb_prunes = residual_lb_prunes_;
        r.active_target = active_target_;
        r.last_completed_target = last_completed_target_;
        if (incumbent_ > 0 && certified_lb_ > 0) {
            r.gap_percent = 100.0 * static_cast<double>(incumbent_ - certified_lb_) / static_cast<double>(certified_lb_);
            if (r.gap_percent < 0.0) r.gap_percent = 0.0;
        }
        r.nodes = nodes_;
        r.hash_hits = hash_hits_;
        r.hash_entries = hash_entries_;
        r.dominance_keys = dominance_keys_;
        r.elapsed_seconds = std::chrono::duration<double>(Clock::now() - start_).count();
        r.cpu_seconds = static_cast<double>(std::clock() - cpu_start_) / CLOCKS_PER_SEC;
        struct rusage ru{};
        if (getrusage(RUSAGE_SELF, &ru) == 0) {
#ifdef __APPLE__
            r.peak_rss_mb = ru.ru_maxrss / (1024.0 * 1024.0);
#else
            r.peak_rss_mb = ru.ru_maxrss / 1024.0;
#endif
        }
        r.nodes_customer = nodes_customer_.load(std::memory_order_relaxed);
        r.nodes_relaxed_proofs = nodes_relaxed_.load(std::memory_order_relaxed);
        r.nodes_close_stack = nodes_closure_.load(std::memory_order_relaxed);
        r.nodes_frontier = nodes_frontier_.load(std::memory_order_relaxed);
        r.nodes_per_second = r.elapsed_seconds > 0.0 ? static_cast<double>(r.nodes) / r.elapsed_seconds : 0.0;
        r.workers_customer = workers_customer_;
        r.workers_relaxed = workers_relaxed_;
        r.workers_closure = workers_closure_;
        r.progress_interval_seconds = opt_.progress_interval_seconds;
        r.progress_file = opt_.progress_file;
        r.best_pattern_order = best_pattern_order_;
        r.threads = opt_.threads;
        r.parallel_depth = opt_.parallel_depth;
        r.frontier_states = last_frontier_states_;
        r.relaxation_probes = relaxation_probes_;
        r.relaxation_successes = relaxation_successes_;
        r.relaxation_merges = relaxation_merges_;
        r.profile = profile_;
        r.relaxed_proof_status = relaxed_proof_status_;
        r.proof_source = proof_source_;
        r.relaxed_proofs_attempted = relaxed_proofs_attempted_;
        r.relaxed_proofs_completed = relaxed_proofs_completed_;
        r.relaxed_proof_infeasibilities = relaxed_proof_infeasibilities_;
        r.relaxation_full_merges = relaxation_full_merges_;
        r.relaxation_full_unmerges = relaxation_full_unmerges_;
        r.relaxation_customers_at_proof = relaxation_customers_at_proof_;
        r.relaxation_full_seconds = relaxation_full_seconds_;
        r.relaxed_proof_seconds = relaxed_proof_seconds_;
        r.relaxed_proof_parallel_calls = relaxed_proof_parallel_calls_;
        r.relaxed_proof_frontier_states = relaxed_proof_frontier_states_;
        r.relaxed_proof_max_workers = relaxed_proof_max_workers_;
        r.relax_proof_threads = effective_relax_proof_threads();
        r.relax_proof_depth = effective_relax_proof_depth();
        r.relax_proof_frontier_cap = opt_.relax_proof_frontier_cap;
        r.original_fallback_used = original_fallback_used_;
        r.full_verified_initial_upper_bound = full_verified_initial_upper_bound_;
        r.full_unverified_warm_ignored = full_unverified_warm_ignored_;
        r.full_explicit_upper_ignored = full_explicit_upper_ignored_;
        r.warm_start_value = warm_start_value_;
        r.warm_start_sequence_value = warm_start_sequence_value_;
        r.warm_start_faggioli_value = warm_start_faggioli_value_;
        r.warm_start_evaluators_agree = warm_start_evaluators_agree_;
        r.anchor_pattern = selected_anchor_pattern_;
        r.reverse_anchor_applied = reverse_anchor_applied_;
        // The branch-and-bound stores an internal customer-search order, not necessarily a
        // pattern-sequencing order. The independent Faggioli evaluator is therefore reported
        // only for imported pattern-sequence warm starts.
        r.best_sequence_faggioli_value = -1;
        r.branching_mode = opt_.branching_mode;
        r.closure_branch_calls = closure_branch_calls_;
        r.closure_branch_nodes = closure_branch_nodes_;
        r.closure_branch_dead_states = closure_branch_dead_states_;
        r.closure_branch_successes = closure_branch_successes_;
        r.closure_branch_fallbacks = closure_branch_fallbacks_;
        r.closure_parallel_calls = closure_parallel_calls_;
        r.closure_frontier_states = closure_frontier_states_;
        r.closure_max_workers = closure_max_workers_;
        r.closure_threads = effective_closure_threads();
        r.closure_parallel_depth = effective_closure_parallel_depth();
        r.closure_frontier_cap = opt_.closure_frontier_cap;
        r.warm_start_accepted = warm_start_accepted_;
        r.warm_start_sequence_accepted = warm_start_sequence_accepted_;
        r.warm_start_verified = warm_start_verified_;
        r.warm_start_validation_status = warm_start_validation_status_;
        r.warm_start_declared_value = warm_start_declared_value_;
        r.warm_start_verified_value = warm_start_verified_value_;
        r.warm_start_sequence_domain = warm_start_sequence_domain_;
        return r;
    }
};

Options parse_options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("Missing value for " + name);
            return argv[++i];
        };
        if (a == "--input" || a == "-i") o.input_path = need(a);
        else if (a == "--input-format") o.input_format = need(a);
        else if (a == "--presence") o.presence_mode = need(a);
        else if (a == "--warm-start-file") o.warm_start_file = need(a);
        else if (a == "--warm-start-policy") o.warm_start_policy = need(a);
        else if (a == "--upper") o.explicit_upper = std::stoi(need(a));
        else if (a == "--lower") { o.explicit_lower = std::stoi(need(a)); o.auto_lower_bound = false; o.auto_lower_mode = "off"; }
        else if (a == "--auto-lower") { o.auto_lower_bound = (need(a) != "0"); if (!o.auto_lower_bound) o.auto_lower_mode = "off"; }
        else if (a == "--residual-lb") o.residual_lb_mode = need(a);
        else if (a == "--residual-core-max-vertices") o.residual_core_max_vertices = std::stoi(need(a));
        else if (a == "--auto-lower-mode") { o.auto_lower_mode = need(a); o.auto_lower_bound = (o.auto_lower_mode != "off"); }
        else if (a == "--relaxation") o.relaxation_mode = need(a);
        else if (a == "--relax-merge-policy") o.relax_merge_policy = need(a);
        else if (a == "--relax-unmerge-policy") o.relax_unmerge_policy = need(a);
        else if (a == "--relax-max-merges") o.relax_max_merges = std::stoi(need(a));
        else if (a == "--relax-max-time-share") o.relax_max_time_share = std::stod(need(a));
        else if (a == "--relax-proof-threads") o.relax_proof_threads = std::stoi(need(a));
        else if (a == "--relax-proof-depth") o.relax_proof_depth = std::stoi(need(a));
        else if (a == "--relax-proof-frontier-cap") o.relax_proof_frontier_cap = std::stoi(need(a));
        else if (a == "--branching") o.branching_mode = need(a);
        else if (a == "--canonical-closures") { o.canonical_stack_closure = (need(a) != "0"); o.group_moves = o.canonical_stack_closure; }
        else if (a == "--sequence-evaluator") o.sequence_evaluator = need(a);
        else if (a == "--symmetry-break") o.symmetry_break = need(a);
        else if (a == "--anchor-pattern") {
            std::string v = need(a);
            if (v == "auto") o.anchor_pattern = -1;
            else { o.anchor_pattern = std::stoi(v) - 1; if (o.anchor_pattern < 0) throw std::runtime_error("--anchor-pattern must be 1-based or auto."); }
        }
        else if (a == "--emit-stack-profile") o.stack_profile_file = need(a);
        else if (a == "--profile") o.profile = (need(a) != "0");
        else if (a == "--progress-interval") o.progress_interval_seconds = std::stod(need(a));
        else if (a == "--progress-file") o.progress_file = need(a);
        else if (a == "--time-limit") o.time_limit_seconds = std::stod(need(a));
        else if (a == "--node-limit") o.node_limit = static_cast<std::uint64_t>(std::stoull(need(a)));
        else if (a == "--threads") o.threads = std::stoi(need(a));
        else if (a == "--parallel-depth") o.parallel_depth = std::stoi(need(a));
        else if (a == "--closure-threads") o.closure_threads = std::stoi(need(a));
        else if (a == "--closure-parallel-depth") o.closure_parallel_depth = std::stoi(need(a));
        else if (a == "--closure-frontier-cap") o.closure_frontier_cap = std::stoi(need(a));
        else if (a == "--hash") o.use_hash = (need(a) != "0");
        else if (a == "--sort-moves") o.sort_moves = (need(a) != "0");
        else if (a == "--group-moves") o.group_moves = (need(a) != "0");
        else if (a == "--json") o.output_json = true;
        else if (a == "--quiet") o.quiet = true;
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: csbb_modern --input FILE [--input-format cs|scoop] "
                         "[--presence positive|legacy-one] [--warm-start-file FILE] "
                         "[--upper U] [--lower L|--auto-lower 0|1] [--auto-lower-mode off|legacy|model|graph] [--residual-lb off|next|core] [--residual-core-max-vertices N] [--warm-start-policy value|sequence|strict] "
                         "[--relaxation off|root-merge-probe|full] [--relax-merge-policy original] [--relax-unmerge-policy original|lifo] [--relax-max-merges N] [--relax-max-time-share P] [--relax-proof-threads N] [--relax-proof-depth D] [--relax-proof-frontier-cap N] [--branching customer|close-stack|auto] [--canonical-closures 0|1] [--sequence-evaluator native|faggioli|both] "
                         "[--symmetry-break off|reverse-anchor] [--anchor-pattern auto|ID] [--emit-stack-profile FILE] "
                         "[--profile 0|1] [--progress-interval SECONDS --progress-file FILE] [--time-limit SECONDS] [--node-limit N] "
                         "[--threads N] [--parallel-depth D] [--closure-threads N] [--closure-parallel-depth D] [--closure-frontier-cap N] "
                         "[--hash 0|1] [--sort-moves 0|1] [--group-moves 0|1] [--json] [--quiet]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + a);
        }
    }
    if (o.input_path.empty()) throw std::runtime_error("Missing --input FILE");
    if (o.input_format != "cs" && o.input_format != "scoop") throw std::runtime_error("Invalid --input-format.");
    if (o.presence_mode != "positive" && o.presence_mode != "legacy-one") throw std::runtime_error("Invalid --presence.");
    if (o.auto_lower_mode != "off" && o.auto_lower_mode != "legacy" && o.auto_lower_mode != "model" && o.auto_lower_mode != "graph") throw std::runtime_error("Invalid --auto-lower-mode.");
    if (o.residual_lb_mode != "off" && o.residual_lb_mode != "next" && o.residual_lb_mode != "core") throw std::runtime_error("Invalid --residual-lb (off|next|core).");
    if (o.residual_core_max_vertices < 0) throw std::runtime_error("Invalid --residual-core-max-vertices (must be >= 0).");
    if (o.warm_start_policy != "value" && o.warm_start_policy != "sequence" && o.warm_start_policy != "strict") throw std::runtime_error("Invalid --warm-start-policy.");
    if (o.relaxation_mode != "off" && o.relaxation_mode != "root-merge-probe" && o.relaxation_mode != "full") throw std::runtime_error("Invalid --relaxation.");
    if (o.relax_unmerge_policy != "original" && o.relax_unmerge_policy != "lifo") throw std::runtime_error("Invalid --relax-unmerge-policy.");
    if (o.relax_merge_policy != "original") throw std::runtime_error("Only --relax-merge-policy original is implemented.");
    if (o.relax_max_merges < 0) throw std::runtime_error("--relax-max-merges must be nonnegative.");
    if (o.relax_max_time_share < 0.0 || o.relax_max_time_share > 1.0) throw std::runtime_error("--relax-max-time-share must be within [0,1].");
    if (o.relax_proof_threads < 0) throw std::runtime_error("--relax-proof-threads must be nonnegative (0 = inherit --threads).");
    if (o.relax_proof_depth < -1) throw std::runtime_error("--relax-proof-depth must be >= -1 (-1 = inherit --parallel-depth).");
    if (o.relax_proof_frontier_cap < 1) throw std::runtime_error("--relax-proof-frontier-cap must be positive.");
    if (o.branching_mode != "customer" && o.branching_mode != "close-stack" && o.branching_mode != "auto") throw std::runtime_error("Invalid --branching.");
    if (o.sequence_evaluator != "native" && o.sequence_evaluator != "faggioli" && o.sequence_evaluator != "both") throw std::runtime_error("Invalid --sequence-evaluator.");
    if (o.symmetry_break != "off" && o.symmetry_break != "reverse-anchor") throw std::runtime_error("Invalid --symmetry-break.");
    if (o.threads < 1) throw std::runtime_error("--threads must be positive.");
    if (o.parallel_depth < 0) throw std::runtime_error("--parallel-depth must be nonnegative.");
    if (o.closure_threads < 0) throw std::runtime_error("--closure-threads must be >= 0 (0 = inherit --threads).");
    if (o.closure_parallel_depth < -1) throw std::runtime_error("--closure-parallel-depth must be >= -1 (-1 = inherit --parallel-depth).");
    if (o.closure_frontier_cap < 1) throw std::runtime_error("--closure-frontier-cap must be positive.");
    if (o.progress_interval_seconds < 0.0 || !std::isfinite(o.progress_interval_seconds))
        throw std::runtime_error("Invalid --progress-interval");
    if (o.progress_interval_seconds > 0.0 && o.progress_file.empty())
        throw std::runtime_error("--progress-interval requires --progress-file");
    return o;
}

void print_result_text(const SolveResult& r) {
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "status " << r.status << "\n";
    std::cout << "upper_bound " << r.upper_bound << "\n";
    std::cout << "lower_bound " << r.lower_bound << "\n";
    std::cout << "applied_initial_lower_bound " << r.applied_initial_lower_bound << "\n";
    std::cout << "legacy_lower_bound " << r.legacy_lower_bound << "\n";
    std::cout << "model_cardinality_lower_bound " << r.model_cardinality_lower_bound << "\n";
    std::cout << "graph_min_degree_lower_bound " << r.graph_min_degree_lower_bound << "\n";
    std::cout << "graph_degeneracy_lower_bound " << r.graph_degeneracy_lower_bound << "\n";
    std::cout << "customer_graph_core_lower_bound " << r.customer_graph_core_lower_bound << "\n";
    std::cout << "residual_lb_mode " << r.residual_lb_mode << "\n";
    std::cout << "residual_lb_calls " << r.residual_lb_calls << "\n";
    std::cout << "residual_lb_core_tests " << r.residual_lb_core_tests << "\n";
    std::cout << "residual_lb_prunes " << r.residual_lb_prunes << "\n";
    std::cout << "active_target " << r.active_target << "\n";
    std::cout << "last_completed_target " << r.last_completed_target << "\n";
    std::cout << "gap_percent " << r.gap_percent << "\n";
    std::cout << "nodes " << r.nodes << "\n";
    std::cout << "hash_hits " << r.hash_hits << "\n";
    std::cout << "hash_entries " << r.hash_entries << "\n";
    std::cout << "dominance_keys " << r.dominance_keys << "\n";
    std::cout << "frontier_states " << r.frontier_states << "\n";
    std::cout << "relaxation_probes " << r.relaxation_probes << "\n";
    std::cout << "relaxation_successes " << r.relaxation_successes << "\n";
    std::cout << "relaxation_merges " << r.relaxation_merges << "\n";
    std::cout << "relaxed_proof_status " << r.relaxed_proof_status << "\n";
    std::cout << "proof_source " << r.proof_source << "\n";
    std::cout << "relaxed_proofs_attempted " << r.relaxed_proofs_attempted << "\n";
    std::cout << "relaxed_proofs_completed " << r.relaxed_proofs_completed << "\n";
    std::cout << "relaxed_proof_infeasibilities " << r.relaxed_proof_infeasibilities << "\n";
    std::cout << "relaxation_full_merges " << r.relaxation_full_merges << "\n";
    std::cout << "relaxation_full_unmerges " << r.relaxation_full_unmerges << "\n";
    std::cout << "relaxation_customers_at_proof " << r.relaxation_customers_at_proof << "\n";
    std::cout << "relaxation_full_seconds " << r.relaxation_full_seconds << "\n";
    std::cout << "relaxed_proof_seconds " << r.relaxed_proof_seconds << "\n";
    std::cout << "relaxed_proof_parallel_calls " << r.relaxed_proof_parallel_calls << "\n";
    std::cout << "relaxed_proof_frontier_states " << r.relaxed_proof_frontier_states << "\n";
    std::cout << "relaxed_proof_max_workers " << r.relaxed_proof_max_workers << "\n";
    std::cout << "relax_proof_threads " << r.relax_proof_threads << "\n";
    std::cout << "relax_proof_depth " << r.relax_proof_depth << "\n";
    std::cout << "relax_proof_frontier_cap " << r.relax_proof_frontier_cap << "\n";
    std::cout << "original_fallback_used " << (r.original_fallback_used ? 1 : 0) << "\n";
    std::cout << "full_verified_initial_upper_bound " << r.full_verified_initial_upper_bound << "\n";
    std::cout << "full_unverified_warm_ignored " << (r.full_unverified_warm_ignored ? 1 : 0) << "\n";
    std::cout << "full_explicit_upper_ignored " << (r.full_explicit_upper_ignored ? 1 : 0) << "\n";
    std::cout << "unverified_upper_ignored " << (r.full_explicit_upper_ignored ? 1 : 0) << "\n";
    std::cout << "profile_force_close_seconds " << r.profile.force_close_seconds << "\n";
    std::cout << "profile_move_generation_seconds " << r.profile.move_generation_seconds << "\n";
    std::cout << "profile_hash_lookup_seconds " << r.profile.hash_lookup_seconds << "\n";
    std::cout << "profile_hash_insert_seconds " << r.profile.hash_insert_seconds << "\n";
    std::cout << "profile_relaxation_probe_seconds " << r.profile.relaxation_probe_seconds << "\n";
    std::cout << "profile_frontier_seconds " << r.profile.frontier_seconds << "\n";
    std::cout << "profile_warm_start_seconds " << r.profile.warm_start_seconds << "\n";
    std::cout << "profile_residual_lb_seconds " << r.profile.residual_lb_seconds << "\n";
    std::cout << "threads " << r.threads << "\n";
    std::cout << "parallel_depth " << r.parallel_depth << "\n";
    std::cout << "warm_start_value " << r.warm_start_value << "\n";
    std::cout << "warm_start_sequence_value " << r.warm_start_sequence_value << "\n";
    std::cout << "warm_start_faggioli_value " << r.warm_start_faggioli_value << "\n";
    std::cout << "warm_start_evaluators_agree " << (r.warm_start_evaluators_agree ? 1 : 0) << "\n";
    std::cout << "anchor_pattern_1_based " << (r.anchor_pattern >= 0 ? r.anchor_pattern + 1 : -1) << "\n";
    std::cout << "reverse_anchor_applied " << (r.reverse_anchor_applied ? 1 : 0) << "\n";
    std::cout << "best_sequence_faggioli_value " << r.best_sequence_faggioli_value << "\n";
    std::cout << "branching_mode " << r.branching_mode << "\n";
    std::cout << "closure_branch_calls " << r.closure_branch_calls << "\n";
    std::cout << "closure_branch_nodes " << r.closure_branch_nodes << "\n";
    std::cout << "closure_branch_dead_states " << r.closure_branch_dead_states << "\n";
    std::cout << "closure_branch_successes " << r.closure_branch_successes << "\n";
    std::cout << "closure_branch_fallbacks " << r.closure_branch_fallbacks << "\n";
    std::cout << "closure_parallel_calls " << r.closure_parallel_calls << "\n";
    std::cout << "closure_frontier_states " << r.closure_frontier_states << "\n";
    std::cout << "closure_max_workers " << r.closure_max_workers << "\n";
    std::cout << "closure_threads " << r.closure_threads << "\n";
    std::cout << "closure_parallel_depth " << r.closure_parallel_depth << "\n";
    std::cout << "closure_frontier_cap " << r.closure_frontier_cap << "\n";
    std::cout << "warm_start_accepted " << (r.warm_start_accepted ? 1 : 0) << "\n";
    std::cout << "warm_start_sequence_accepted " << (r.warm_start_sequence_accepted ? 1 : 0) << "\n";
    std::cout << "warm_start_verified " << (r.warm_start_verified ? 1 : 0) << "\n";
    std::cout << "warm_start_validation_status " << r.warm_start_validation_status << "\n";
    std::cout << "warm_start_declared_value " << r.warm_start_declared_value << "\n";
    std::cout << "warm_start_verified_value " << r.warm_start_verified_value << "\n";
    std::cout << "warm_start_sequence_domain " << r.warm_start_sequence_domain << "\n";
    std::cout << "elapsed_seconds " << r.elapsed_seconds << "\n";
    std::cout << "cpu_seconds " << r.cpu_seconds << "\n";
    std::cout << "peak_rss_mb " << r.peak_rss_mb << "\n";
    std::cout << "nodes_customer " << r.nodes_customer << "\n";
    std::cout << "nodes_relaxed_proofs " << r.nodes_relaxed_proofs << "\n";
    std::cout << "nodes_close_stack " << r.nodes_close_stack << "\n";
    std::cout << "nodes_frontier " << r.nodes_frontier << "\n";
    std::cout << "nodes_per_second " << r.nodes_per_second << "\n";
    if (!r.best_pattern_order.empty()) {
        std::cout << "best_sequence_0_based";
        for (int p : r.best_pattern_order) std::cout << ' ' << p;
        std::cout << "\n";
        std::cout << "best_sequence_1_based";
        for (int p : r.best_pattern_order) std::cout << ' ' << (p + 1);
        std::cout << "\n";
    }
}

void print_result_json(const SolveResult& r) {
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "{\n";
    std::cout << "  \"status\": \"" << r.status << "\",\n";
    std::cout << "  \"upper_bound\": " << r.upper_bound << ",\n";
    std::cout << "  \"lower_bound\": " << r.lower_bound << ",\n";
    std::cout << "  \"applied_initial_lower_bound\": " << r.applied_initial_lower_bound << ",\n";
    std::cout << "  \"legacy_lower_bound\": " << r.legacy_lower_bound << ",\n";
    std::cout << "  \"model_cardinality_lower_bound\": " << r.model_cardinality_lower_bound << ",\n";
    std::cout << "  \"graph_min_degree_lower_bound\": " << r.graph_min_degree_lower_bound << ",\n";
    std::cout << "  \"graph_degeneracy_lower_bound\": " << r.graph_degeneracy_lower_bound << ",\n";
    std::cout << "  \"customer_graph_core_lower_bound\": " << r.customer_graph_core_lower_bound << ",\n";
    std::cout << "  \"residual_lb_mode\": \"" << r.residual_lb_mode << "\",\n";
    std::cout << "  \"residual_lb_calls\": " << r.residual_lb_calls << ",\n";
    std::cout << "  \"residual_lb_core_tests\": " << r.residual_lb_core_tests << ",\n";
    std::cout << "  \"residual_lb_prunes\": " << r.residual_lb_prunes << ",\n";
    std::cout << "  \"active_target\": " << r.active_target << ",\n";
    std::cout << "  \"last_completed_target\": " << r.last_completed_target << ",\n";
    std::cout << "  \"gap_percent\": " << r.gap_percent << ",\n";
    std::cout << "  \"nodes\": " << r.nodes << ",\n";
    std::cout << "  \"nodes_customer\": " << r.nodes_customer << ",\n";
    std::cout << "  \"nodes_relaxed_proofs\": " << r.nodes_relaxed_proofs << ",\n";
    std::cout << "  \"nodes_close_stack\": " << r.nodes_close_stack << ",\n";
    std::cout << "  \"nodes_frontier\": " << r.nodes_frontier << ",\n";
    std::cout << "  \"nodes_per_second\": " << r.nodes_per_second << ",\n";
    std::cout << "  \"cpu_seconds\": " << r.cpu_seconds << ",\n";
    std::cout << "  \"peak_rss_mb\": " << r.peak_rss_mb << ",\n";
    std::cout << "  \"progress_interval_seconds\": " << r.progress_interval_seconds << ",\n";

    std::cout << "  \"hash_hits\": " << r.hash_hits << ",\n";
    std::cout << "  \"hash_entries\": " << r.hash_entries << ",\n";
    std::cout << "  \"dominance_keys\": " << r.dominance_keys << ",\n";
    std::cout << "  \"frontier_states\": " << r.frontier_states << ",\n";
    std::cout << "  \"relaxation_probes\": " << r.relaxation_probes << ",\n";
    std::cout << "  \"relaxation_successes\": " << r.relaxation_successes << ",\n";
    std::cout << "  \"relaxation_merges\": " << r.relaxation_merges << ",\n";
    std::cout << "  \"relaxed_proof_status\": \"" << r.relaxed_proof_status << "\",\n";
    std::cout << "  \"proof_source\": \"" << r.proof_source << "\",\n";
    std::cout << "  \"relaxed_proofs_attempted\": " << r.relaxed_proofs_attempted << ",\n";
    std::cout << "  \"relaxed_proofs_completed\": " << r.relaxed_proofs_completed << ",\n";
    std::cout << "  \"relaxed_proof_infeasibilities\": " << r.relaxed_proof_infeasibilities << ",\n";
    std::cout << "  \"relaxation_full_merges\": " << r.relaxation_full_merges << ",\n";
    std::cout << "  \"relaxation_full_unmerges\": " << r.relaxation_full_unmerges << ",\n";
    std::cout << "  \"relaxation_customers_at_proof\": " << r.relaxation_customers_at_proof << ",\n";
    std::cout << "  \"relaxation_full_seconds\": " << r.relaxation_full_seconds << ",\n";
    std::cout << "  \"relaxed_proof_seconds\": " << r.relaxed_proof_seconds << ",\n";
    std::cout << "  \"relaxed_proof_parallel_calls\": " << r.relaxed_proof_parallel_calls << ",\n";
    std::cout << "  \"relaxed_proof_frontier_states\": " << r.relaxed_proof_frontier_states << ",\n";
    std::cout << "  \"relaxed_proof_max_workers\": " << r.relaxed_proof_max_workers << ",\n";
    std::cout << "  \"relax_proof_threads\": " << r.relax_proof_threads << ",\n";
    std::cout << "  \"relax_proof_depth\": " << r.relax_proof_depth << ",\n";
    std::cout << "  \"relax_proof_frontier_cap\": " << r.relax_proof_frontier_cap << ",\n";
    std::cout << "  \"original_fallback_used\": " << (r.original_fallback_used ? "true" : "false") << ",\n";
    std::cout << "  \"full_verified_initial_upper_bound\": " << r.full_verified_initial_upper_bound << ",\n";
    std::cout << "  \"full_unverified_warm_ignored\": " << (r.full_unverified_warm_ignored ? "true" : "false") << ",\n";
    std::cout << "  \"full_explicit_upper_ignored\": " << (r.full_explicit_upper_ignored ? "true" : "false") << ",\n";
    std::cout << "  \"unverified_upper_ignored\": " << (r.full_explicit_upper_ignored ? "true" : "false") << ",\n";
    std::cout << "  \"profile_force_close_seconds\": " << r.profile.force_close_seconds << ",\n";
    std::cout << "  \"profile_move_generation_seconds\": " << r.profile.move_generation_seconds << ",\n";
    std::cout << "  \"profile_hash_lookup_seconds\": " << r.profile.hash_lookup_seconds << ",\n";
    std::cout << "  \"profile_hash_insert_seconds\": " << r.profile.hash_insert_seconds << ",\n";
    std::cout << "  \"profile_relaxation_probe_seconds\": " << r.profile.relaxation_probe_seconds << ",\n";
    std::cout << "  \"profile_frontier_seconds\": " << r.profile.frontier_seconds << ",\n";
    std::cout << "  \"profile_warm_start_seconds\": " << r.profile.warm_start_seconds << ",\n";
    std::cout << "  \"profile_residual_lb_seconds\": " << r.profile.residual_lb_seconds << ",\n";
    std::cout << "  \"threads\": " << r.threads << ",\n";
    std::cout << "  \"parallel_depth\": " << r.parallel_depth << ",\n";
    std::cout << "  \"warm_start_value\": " << r.warm_start_value << ",\n";
    std::cout << "  \"warm_start_sequence_value\": " << r.warm_start_sequence_value << ",\n";
    std::cout << "  \"warm_start_faggioli_value\": " << r.warm_start_faggioli_value << ",\n";
    std::cout << "  \"warm_start_evaluators_agree\": " << (r.warm_start_evaluators_agree ? "true" : "false") << ",\n";
    std::cout << "  \"anchor_pattern_1_based\": " << (r.anchor_pattern >= 0 ? r.anchor_pattern + 1 : -1) << ",\n";
    std::cout << "  \"reverse_anchor_applied\": " << (r.reverse_anchor_applied ? "true" : "false") << ",\n";
    std::cout << "  \"best_sequence_faggioli_value\": " << r.best_sequence_faggioli_value << ",\n";
    std::cout << "  \"branching_mode\": \"" << r.branching_mode << "\",\n";
    std::cout << "  \"closure_branch_calls\": " << r.closure_branch_calls << ",\n";
    std::cout << "  \"closure_branch_nodes\": " << r.closure_branch_nodes << ",\n";
    std::cout << "  \"closure_branch_dead_states\": " << r.closure_branch_dead_states << ",\n";
    std::cout << "  \"closure_branch_successes\": " << r.closure_branch_successes << ",\n";
    std::cout << "  \"closure_branch_fallbacks\": " << r.closure_branch_fallbacks << ",\n";
    std::cout << "  \"closure_parallel_calls\": " << r.closure_parallel_calls << ",\n";
    std::cout << "  \"closure_frontier_states\": " << r.closure_frontier_states << ",\n";
    std::cout << "  \"closure_max_workers\": " << r.closure_max_workers << ",\n";
    std::cout << "  \"closure_threads\": " << r.closure_threads << ",\n";
    std::cout << "  \"closure_parallel_depth\": " << r.closure_parallel_depth << ",\n";
    std::cout << "  \"closure_frontier_cap\": " << r.closure_frontier_cap << ",\n";
    std::cout << "  \"warm_start_accepted\": " << (r.warm_start_accepted ? "true" : "false") << ",\n";
    std::cout << "  \"warm_start_sequence_accepted\": " << (r.warm_start_sequence_accepted ? "true" : "false") << ",\n";
    std::cout << "  \"warm_start_verified\": " << (r.warm_start_verified ? "true" : "false") << ",\n";
    std::cout << "  \"warm_start_validation_status\": \"" << r.warm_start_validation_status << "\",\n";
    std::cout << "  \"warm_start_declared_value\": " << r.warm_start_declared_value << ",\n";
    std::cout << "  \"warm_start_verified_value\": " << r.warm_start_verified_value << ",\n";
    std::cout << "  \"warm_start_sequence_domain\": \"" << r.warm_start_sequence_domain << "\",\n";
    std::cout << "  \"elapsed_seconds\": " << r.elapsed_seconds << ",\n";
    auto print_workers = [&](const char* name, const std::vector<WorkerMetric>& ws) {
        std::cout << "  \"" << name << "\": [";
        for (size_t i=0; i<ws.size(); ++i) {
            if (i) std::cout << ", ";
            std::cout << "{\"slot\":" << i << ",\"nodes\":" << ws[i].nodes
                      << ",\"tasks\":" << ws[i].tasks
                      << ",\"active_seconds\":" << ws[i].active_seconds
                      << ",\"available_seconds\":" << ws[i].available_seconds
                      << ",\"idle_or_overhead_seconds\":" << std::max(0.0, ws[i].available_seconds-ws[i].active_seconds)
                      << "}";
        }
        std::cout << "],\n";
    };
    print_workers("workers_customer", r.workers_customer);
    print_workers("workers_relaxed", r.workers_relaxed);
    print_workers("workers_closure", r.workers_closure);
    std::cout << "  \"best_sequence_0_based\": [";
    for (size_t i = 0; i < r.best_pattern_order.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << r.best_pattern_order[i];
    }
    std::cout << "]\n";
    std::cout << "}\n";
}

} // namespace csbb

#ifndef CSBB_LIBRARY_ONLY
int main(int argc, char** argv) {
    try {
        csbb::Options opt = csbb::parse_options(argc, argv);
        const bool output_json = opt.output_json;
        csbb::Solver solver(std::move(opt));
        csbb::SolveResult result = solver.run();
        if (output_json) csbb::print_result_json(result);
        else csbb::print_result_text(result);
        return (result.status == "OPTIMAL" || result.status == "TIME_LIMIT" || result.status == "NODE_LIMIT" || result.status == "COMPLETE_BUT_GAP") ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
}

#endif // CSBB_LIBRARY_ONLY
