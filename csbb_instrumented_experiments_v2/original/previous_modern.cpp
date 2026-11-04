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
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
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
    bool use_hash = true;
    bool sort_moves = true;
    bool group_moves = true;
    bool quiet = false;
    bool output_json = false;
    std::string warm_start_policy = "value"; // value | sequence | strict
    std::string relaxation_mode = "off";      // off | root-merge-probe
    std::string branching_mode = "customer";   // customer | close-stack | auto
    bool canonical_stack_closure = true;       // canonicalize simultaneous closures
    std::string sequence_evaluator = "native"; // native | faggioli | both
    std::string symmetry_break = "off";        // off | reverse-anchor
    int anchor_pattern = -1;                   // 0-based; -1 means automatic
    std::string stack_profile_file;
    bool profile = false;
    double time_limit_seconds = 0.0;           // <=0 means unlimited
    std::uint64_t node_limit = 0;              // 0 means unlimited
    int threads = 1;
    int parallel_depth = 1;
};

struct Instance {
    int n_patterns = 0; // customers in Chu--Stuckey terminology
    int n_pieces = 0;   // products in Chu--Stuckey terminology
    std::vector<std::vector<unsigned char>> pattern_piece; // pattern x piece, boolean
};

struct DecisionResult {
    enum class Status { Feasible, Infeasible, Timeout, NodeLimit } status = Status::Infeasible;
};

struct ProfileStats {
    double force_close_seconds = 0.0;
    double move_generation_seconds = 0.0;
    double hash_lookup_seconds = 0.0;
    double hash_insert_seconds = 0.0;
    double relaxation_probe_seconds = 0.0;
    double frontier_seconds = 0.0;
    double warm_start_seconds = 0.0;

    void add(const ProfileStats& other) noexcept {
        force_close_seconds += other.force_close_seconds;
        move_generation_seconds += other.move_generation_seconds;
        hash_lookup_seconds += other.hash_lookup_seconds;
        hash_insert_seconds += other.hash_insert_seconds;
        relaxation_probe_seconds += other.relaxation_probe_seconds;
        frontier_seconds += other.frontier_seconds;
        warm_start_seconds += other.warm_start_seconds;
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
    int active_target = -1;
    int last_completed_target = -1;
    double gap_percent = std::numeric_limits<double>::infinity();
    std::uint64_t nodes = 0;
    std::uint64_t hash_hits = 0;
    std::uint64_t hash_entries = 0;
    std::uint64_t dominance_keys = 0;
    double elapsed_seconds = 0.0;
    int threads = 1;
    int parallel_depth = 0;
    std::uint64_t frontier_states = 0;
    std::uint64_t relaxation_probes = 0;
    std::uint64_t relaxation_successes = 0;
    std::uint64_t relaxation_merges = 0;
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
    bool warm_start_accepted = false;
    bool warm_start_sequence_accepted = false;
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

    SolveResult run() {
        start_ = Clock::now();
        read_instance();
        build_sets();
        initialize_bounds();
        load_warm_start_if_any();

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
                last_completed_target_ = k;
                incumbent_ = k;
                if (!decision_found_order_.empty()) best_pattern_order_ = decision_found_order_;
                --k;
                continue;
            }
            if (dr.status == DecisionResult::Status::Infeasible) {
                last_completed_target_ = k;
                certified_lb_ = std::max(certified_lb_, k + 1);
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

    struct SearchContext {
        DominanceTable table;
        std::uint64_t local_nodes = 0;
        std::uint64_t local_hash_hits = 0;
        std::uint64_t local_hash_entries = 0;
        std::uint64_t local_dominance_keys = 0;
        std::atomic<bool>* stop = nullptr;
        std::atomic<std::uint64_t>* global_nodes = nullptr;
        std::mutex* result_mutex = nullptr;
        std::vector<int>* shared_found_order = nullptr;
        ProfileStats profile;
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
    std::uint64_t hash_hits_ = 0;
    std::uint64_t hash_entries_ = 0;
    std::uint64_t dominance_keys_ = 0;
    std::uint64_t last_frontier_states_ = 0;
    std::uint64_t relaxation_probes_ = 0;
    std::uint64_t relaxation_successes_ = 0;
    std::uint64_t relaxation_merges_ = 0;
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
    int selected_anchor_pattern_ = -1;
    bool reverse_anchor_applied_ = false;
    bool warm_start_accepted_ = false;
    bool warm_start_sequence_accepted_ = false;
    std::uint64_t closure_branch_calls_ = 0;
    std::uint64_t closure_branch_nodes_ = 0;
    std::uint64_t closure_branch_dead_states_ = 0;
    std::uint64_t closure_branch_successes_ = 0;
    std::uint64_t closure_branch_fallbacks_ = 0;

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

        if (!opt_.auto_lower_bound || opt_.auto_lower_mode == "off") return 1;
        if (opt_.auto_lower_mode == "legacy") return legacy_lower_bound_;
        if (opt_.auto_lower_mode == "model") return model_cardinality_lower_bound_;
        if (opt_.auto_lower_mode == "graph") {
            return std::max(model_cardinality_lower_bound_, graph_degeneracy_lower_bound_);
        }
        throw std::runtime_error("Invalid --auto-lower-mode: " + opt_.auto_lower_mode);
    }

    void initialize_bounds() {
        selected_anchor_pattern_ = choose_anchor_pattern();
        certified_lb_ = opt_.explicit_lower > 0 ? opt_.explicit_lower : compute_auto_lower_bound();
        applied_initial_lower_bound_ = certified_lb_;
        incumbent_ = opt_.explicit_upper > 0 ? opt_.explicit_upper : inst_.n_patterns;
        incumbent_ = std::max(incumbent_, certified_lb_);
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

    void load_warm_start_if_any() {
        const auto warm_t0 = Clock::now();
        if (opt_.warm_start_file.empty()) return;
        std::ifstream in(opt_.warm_start_file);
        if (!in) throw std::runtime_error("Could not open warm-start file: " + opt_.warm_start_file);
        std::string line;
        int declared_value = -1;
        std::vector<int> seq0;
        std::vector<int> seq1_fallback;
        while (std::getline(in, line)) {
            if (line.rfind("value", 0) == 0) {
                std::istringstream iss(line);
                std::string tag; iss >> tag >> declared_value;
            } else if (line.rfind("sequence_0_based", 0) == 0) {
                std::istringstream iss(line);
                std::string tag; iss >> tag;
                int x;
                seq0.clear();
                while (iss >> x) seq0.push_back(x);
            } else if (line.rfind("sequence_1_based", 0) == 0) {
                std::istringstream iss(line);
                std::string tag; iss >> tag;
                int x;
                seq1_fallback.clear();
                while (iss >> x) seq1_fallback.push_back(x - 1);
            }
        }
        if (seq0.empty() && !seq1_fallback.empty()) seq0 = std::move(seq1_fallback);

        int sequence_value = -1;
        int faggioli_value = -1;
        bool sequence_valid = false;
        bool faggioli_valid = false;
        if (!seq0.empty()) {
            if (static_cast<int>(seq0.size()) != inst_.n_patterns) {
                throw std::runtime_error("Warm-start sequence length does not match number of patterns/customers.");
            }
            apply_reverse_anchor_canonicalization(seq0);
            sequence_value = evaluate_pattern_order_as_customer_order(seq0);
            sequence_valid = sequence_value < std::numeric_limits<int>::max() / 8;
            faggioli_value = evaluate_pattern_order_faggioli(seq0, opt_.stack_profile_file);
            faggioli_valid = faggioli_value < std::numeric_limits<int>::max() / 8;
        }

        warm_start_sequence_value_ = sequence_value;
        warm_start_faggioli_value_ = faggioli_value;
        warm_start_evaluators_agree_ = (!sequence_valid || !faggioli_valid) ? true : (sequence_value == faggioli_value);

        const bool use_faggioli_for_sequence = (opt_.sequence_evaluator == "faggioli");
        const bool use_both_for_validation = (opt_.sequence_evaluator == "both");
        const int selected_sequence_value = use_faggioli_for_sequence ? faggioli_value : sequence_value;
        const bool selected_sequence_valid = use_faggioli_for_sequence ? faggioli_valid : sequence_valid;
        if (use_both_for_validation && sequence_valid && faggioli_valid && sequence_value != faggioli_value && !opt_.quiet) {
            std::cerr << "warning: native and Faggioli sequence evaluators differ: native="
                      << sequence_value << " faggioli=" << faggioli_value << "\n";
        }

        int ub = -1;
        bool attach_sequence = false;
        if (opt_.warm_start_policy == "sequence") {
            if (!selected_sequence_valid) throw std::runtime_error("Warm-start policy 'sequence' requires a valid sequence under the selected evaluator.");
            ub = selected_sequence_value;
            attach_sequence = sequence_valid;
        } else if (opt_.warm_start_policy == "strict") {
            if (declared_value <= 0 || !selected_sequence_valid || declared_value != selected_sequence_value) {
                std::ostringstream oss;
                oss << "Warm-start strict validation failed: declared_value=" << declared_value
                    << " selected_sequence_value=" << selected_sequence_value;
                throw std::runtime_error(oss.str());
            }
            ub = declared_value;
            attach_sequence = sequence_valid;
        } else if (opt_.warm_start_policy == "value") {
            // Trust a declared MOSP upper-bound value, as produced by the integrated MCNh wrapper.
            // The optional sequence is evaluated by the selected/auxiliary evaluator only to decide
            // whether it can be attached as an internal customer-order incumbent; it is not required
            // to match the pattern-sequencing value.
            if (declared_value > 0) {
                ub = declared_value;
                attach_sequence = sequence_valid && sequence_value == declared_value;
            } else {
                if (!selected_sequence_valid) throw std::runtime_error("Warm-start file has neither declared value nor valid sequence.");
                ub = selected_sequence_value;
                attach_sequence = sequence_valid;
            }
        } else {
            throw std::runtime_error("Invalid warm-start policy: " + opt_.warm_start_policy);
        }

        warm_start_value_ = ub;
        if (ub > 0 && ub < incumbent_) {
            incumbent_ = ub;
            warm_start_accepted_ = true;
            if (attach_sequence) {
                best_pattern_order_ = seq0;
                warm_start_sequence_accepted_ = true;
            }
        }
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

    bool relaxation_probe(int k, std::vector<int>& found_order, ProfileStats* prof = nullptr) {
        if (opt_.relaxation_mode == "off") return false;
        if (opt_.relaxation_mode != "root-merge-probe") throw std::runtime_error("Invalid relaxation mode: " + opt_.relaxation_mode);
        const auto t0 = Clock::now();
        ++relaxation_probes_;
        std::vector<Bitset> local_cross = cross_;
        Bitset custs = all_patterns_;
        Bitset custs_open(inst_.n_patterns);
        std::vector<std::pair<int,int>> merges;
        merges.reserve(static_cast<size_t>(std::max(0, inst_.n_patterns - k)));

        while (custs.count() > k) {
            std::vector<double> w(static_cast<size_t>(inst_.n_patterns), 0.0);
            custs.for_each_set_bit([&](int i) {
                const int denom = std::max(1, local_cross[i].minus(custs_open).count());
                w[static_cast<size_t>(i)] = 1.0 / static_cast<double>(denom);
            });
            double safest = std::numeric_limits<double>::infinity();
            int best_i = -1;
            int best_j = -1;
            custs.for_each_set_bit([&](int i) {
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

    DecisionResult dfs(Bitset& opened, Bitset& remaining, std::vector<int>& closed_order, int k, SearchContext& ctx) {
        if (ctx.stop && ctx.stop->load(std::memory_order_relaxed)) return {DecisionResult::Status::Feasible};

        const std::uint64_t current_nodes = ctx.global_nodes ? ctx.global_nodes->fetch_add(1, std::memory_order_relaxed) + 1ULL : ++nodes_;
        ++ctx.local_nodes;

        if (node_limited(current_nodes)) return {DecisionResult::Status::NodeLimit};
        if ((current_nodes & 1023ULL) == 0ULL || opt_.time_limit_seconds > 0.0) {
            if (timed_out()) return {DecisionResult::Status::Timeout};
        }

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
                                   std::vector<int>& immediate_found_order) {
        const std::uint64_t current_nodes = global_nodes.fetch_add(1, std::memory_order_relaxed) + 1ULL;
        if (node_limited(current_nodes)) return {DecisionResult::Status::NodeLimit};
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

        if (depth <= 0) {
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

        for (const Move& m : moves) {
            Bitset next_opened = opened.bit_or(m.open_set);
            Bitset next_remaining = remaining.minus(m.close_set);
            const size_t before_move_order = closed_order.size();
            m.close_set.for_each_set_bit([&](int p) {
                if (remaining.test(p)) closed_order.push_back(p);
            });
            DecisionResult dr = expand_frontier(next_opened, next_remaining, closed_order, k, depth - 1, frontier, global_nodes, immediate_found_order);
            while (closed_order.size() > before_move_order) closed_order.pop_back();
            if (dr.status == DecisionResult::Status::Feasible || dr.status == DecisionResult::Status::Timeout || dr.status == DecisionResult::Status::NodeLimit) {
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

    DecisionResult close_stack_dfs(Bitset& processed,
                                   Bitset& closed_items,
                                   Bitset& opened_active,
                                   std::vector<int>& pattern_order,
                                   int k,
                                   std::unordered_set<Bitset, BitsetHash>& dead,
                                   std::atomic<std::uint64_t>& global_nodes,
                                   std::vector<int>& found_order) {
        const std::uint64_t current_nodes = global_nodes.fetch_add(1, std::memory_order_relaxed) + 1ULL;
        ++closure_branch_nodes_;
        if (node_limited(current_nodes)) return {DecisionResult::Status::NodeLimit};
        if ((current_nodes & 1023ULL) == 0ULL || opt_.time_limit_seconds > 0.0) {
            if (timed_out()) return {DecisionResult::Status::Timeout};
        }

        force_empty_item_closures(processed, closed_items);

        if (processed == all_patterns_) {
            if (validate_permutation(pattern_order) && evaluate_pattern_order_as_customer_order(pattern_order) <= k) {
                found_order = pattern_order;
                ++closure_branch_successes_;
                return {DecisionResult::Status::Feasible};
            }
            return {DecisionResult::Status::Infeasible};
        }

        if (dead.find(processed) != dead.end()) return {DecisionResult::Status::Infeasible};

        std::vector<ClosureCandidate> candidates = generate_closure_candidates(processed, closed_items, opened_active, k);
        if (candidates.empty()) {
            dead.insert(processed);
            closure_branch_dead_states_ = std::max<std::uint64_t>(closure_branch_dead_states_, dead.size());
            return {DecisionResult::Status::Infeasible};
        }

        for (const ClosureCandidate& cand : candidates) {
            Bitset next_processed = processed.bit_or(cand.block);
            Bitset block_items = union_items_of_patterns(cand.block);
            Bitset active_before_closure = opened_active.bit_or(block_items).minus(closed_items);
            if (active_before_closure.count() > k) continue;

            Bitset next_closed = closed_items;
            Bitset close_candidates = all_pieces_.minus(next_closed);
            close_candidates.for_each_set_bit([&](int item) {
                if (plays_[item].subset_of(next_processed)) next_closed.set(item);
            });
            Bitset next_opened_active = active_before_closure.minus(next_closed);

            const size_t old_size = pattern_order.size();
            std::vector<int> block_order = order_block_patterns_for_closure(cand.block, processed, closed_items);
            for (int p : block_order) pattern_order.push_back(p);

            DecisionResult child = close_stack_dfs(next_processed, next_closed, next_opened_active,
                                                   pattern_order, k, dead, global_nodes, found_order);
            while (pattern_order.size() > old_size) pattern_order.pop_back();
            if (child.status != DecisionResult::Status::Infeasible) return child;
        }

        dead.insert(processed);
        closure_branch_dead_states_ = std::max<std::uint64_t>(closure_branch_dead_states_, dead.size());
        return {DecisionResult::Status::Infeasible};
    }

    DecisionResult decision_search_close_stack(int k, std::atomic<std::uint64_t>& global_nodes) {
        ++closure_branch_calls_;
        Bitset processed(inst_.n_patterns);
        Bitset closed_items(inst_.n_pieces);
        Bitset opened_active(inst_.n_pieces);
        std::vector<int> pattern_order;
        pattern_order.reserve(inst_.n_patterns);
        std::vector<int> found_order;
        std::unordered_set<Bitset, BitsetHash> dead;
        dead.reserve(4096);
        DecisionResult dr = close_stack_dfs(processed, closed_items, opened_active, pattern_order, k, dead, global_nodes, found_order);
        closure_branch_dead_states_ = std::max<std::uint64_t>(closure_branch_dead_states_, dead.size());
        if (dr.status == DecisionResult::Status::Feasible && !found_order.empty()) decision_found_order_ = found_order;
        return dr;
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
        DecisionResult dr = dfs(opened, remaining, closed_order, k, ctx);
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
        std::atomic<size_t> next_idx(0);
        std::atomic<int> terminal(0); // 0 none, 1 timeout, 2 node limit
        std::mutex result_mutex;
        std::vector<int> shared_found_order;
        std::atomic<std::uint64_t> aggregate_hash_hits(0), aggregate_hash_entries(0), aggregate_keys(0);
        std::mutex profile_mutex;
        ProfileStats aggregate_profile;

        const int nthreads = std::min<int>(opt_.threads, static_cast<int>(frontier.size()));
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(nthreads));
        for (int t = 0; t < nthreads; ++t) {
            workers.emplace_back([&, t]() {
                SearchContext ctx;
                ctx.stop = &stop;
                ctx.global_nodes = &global_nodes;
                ctx.result_mutex = &result_mutex;
                ctx.shared_found_order = &shared_found_order;
                while (!stop.load(std::memory_order_relaxed)) {
                    const size_t idx = next_idx.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= frontier.size()) break;
                    FrontierState st = frontier[idx];
                    DecisionResult dr = dfs(st.opened, st.remaining, st.closed_order, k, ctx);
                    if (dr.status == DecisionResult::Status::Feasible) {
                        stop.store(true, std::memory_order_relaxed);
                        break;
                    }
                    if (dr.status == DecisionResult::Status::Timeout) {
                        terminal.store(1, std::memory_order_relaxed);
                        stop.store(true, std::memory_order_relaxed);
                        break;
                    }
                    if (dr.status == DecisionResult::Status::NodeLimit) {
                        terminal.store(2, std::memory_order_relaxed);
                        stop.store(true, std::memory_order_relaxed);
                        break;
                    }
                }
                aggregate_hash_hits.fetch_add(ctx.local_hash_hits, std::memory_order_relaxed);
                aggregate_hash_entries.fetch_add(ctx.table.entries(), std::memory_order_relaxed);
                aggregate_keys.fetch_add(ctx.table.keys(), std::memory_order_relaxed);
                if (opt_.profile) {
                    std::lock_guard<std::mutex> lock(profile_mutex);
                    aggregate_profile.add(ctx.profile);
                }
            });
        }
        for (auto& th : workers) th.join();

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
        std::vector<int> relaxed_order;
        if (relaxation_probe(k, relaxed_order)) {
            decision_found_order_ = std::move(relaxed_order);
            return {DecisionResult::Status::Feasible};
        }
        std::atomic<std::uint64_t> global_nodes(nodes_);
        DecisionResult dr;
        if (opt_.branching_mode == "close-stack" || opt_.branching_mode == "auto") {
            dr = decision_search_close_stack(k, global_nodes);
            if (dr.status == DecisionResult::Status::Feasible ||
                dr.status == DecisionResult::Status::Timeout ||
                dr.status == DecisionResult::Status::NodeLimit) {
                nodes_ = global_nodes.load(std::memory_order_relaxed);
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
        r.best_pattern_order = best_pattern_order_;
        r.threads = opt_.threads;
        r.parallel_depth = opt_.parallel_depth;
        r.frontier_states = last_frontier_states_;
        r.relaxation_probes = relaxation_probes_;
        r.relaxation_successes = relaxation_successes_;
        r.relaxation_merges = relaxation_merges_;
        r.profile = profile_;
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
        r.warm_start_accepted = warm_start_accepted_;
        r.warm_start_sequence_accepted = warm_start_sequence_accepted_;
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
        else if (a == "--auto-lower-mode") { o.auto_lower_mode = need(a); o.auto_lower_bound = (o.auto_lower_mode != "off"); }
        else if (a == "--relaxation") o.relaxation_mode = need(a);
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
        else if (a == "--time-limit") o.time_limit_seconds = std::stod(need(a));
        else if (a == "--node-limit") o.node_limit = static_cast<std::uint64_t>(std::stoull(need(a)));
        else if (a == "--threads") o.threads = std::stoi(need(a));
        else if (a == "--parallel-depth") o.parallel_depth = std::stoi(need(a));
        else if (a == "--hash") o.use_hash = (need(a) != "0");
        else if (a == "--sort-moves") o.sort_moves = (need(a) != "0");
        else if (a == "--group-moves") o.group_moves = (need(a) != "0");
        else if (a == "--json") o.output_json = true;
        else if (a == "--quiet") o.quiet = true;
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: csbb_modern --input FILE [--input-format cs|scoop] "
                         "[--presence positive|legacy-one] [--warm-start-file FILE] "
                         "[--upper U] [--lower L|--auto-lower 0|1] [--auto-lower-mode off|legacy|model|graph] [--warm-start-policy value|sequence|strict] "
                         "[--relaxation off|root-merge-probe] [--branching customer|close-stack|auto] [--canonical-closures 0|1] [--sequence-evaluator native|faggioli|both] "
                         "[--symmetry-break off|reverse-anchor] [--anchor-pattern auto|ID] [--emit-stack-profile FILE] "
                         "[--profile 0|1] [--time-limit SECONDS] [--node-limit N] "
                         "[--threads N] [--parallel-depth D] "
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
    if (o.warm_start_policy != "value" && o.warm_start_policy != "sequence" && o.warm_start_policy != "strict") throw std::runtime_error("Invalid --warm-start-policy.");
    if (o.relaxation_mode != "off" && o.relaxation_mode != "root-merge-probe") throw std::runtime_error("Invalid --relaxation.");
    if (o.branching_mode != "customer" && o.branching_mode != "close-stack" && o.branching_mode != "auto") throw std::runtime_error("Invalid --branching.");
    if (o.sequence_evaluator != "native" && o.sequence_evaluator != "faggioli" && o.sequence_evaluator != "both") throw std::runtime_error("Invalid --sequence-evaluator.");
    if (o.symmetry_break != "off" && o.symmetry_break != "reverse-anchor") throw std::runtime_error("Invalid --symmetry-break.");
    if (o.threads < 1) throw std::runtime_error("--threads must be positive.");
    if (o.parallel_depth < 0) throw std::runtime_error("--parallel-depth must be nonnegative.");
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
    std::cout << "profile_force_close_seconds " << r.profile.force_close_seconds << "\n";
    std::cout << "profile_move_generation_seconds " << r.profile.move_generation_seconds << "\n";
    std::cout << "profile_hash_lookup_seconds " << r.profile.hash_lookup_seconds << "\n";
    std::cout << "profile_hash_insert_seconds " << r.profile.hash_insert_seconds << "\n";
    std::cout << "profile_relaxation_probe_seconds " << r.profile.relaxation_probe_seconds << "\n";
    std::cout << "profile_frontier_seconds " << r.profile.frontier_seconds << "\n";
    std::cout << "profile_warm_start_seconds " << r.profile.warm_start_seconds << "\n";
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
    std::cout << "warm_start_accepted " << (r.warm_start_accepted ? 1 : 0) << "\n";
    std::cout << "warm_start_sequence_accepted " << (r.warm_start_sequence_accepted ? 1 : 0) << "\n";
    std::cout << "elapsed_seconds " << r.elapsed_seconds << "\n";
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
    std::cout << "  \"active_target\": " << r.active_target << ",\n";
    std::cout << "  \"last_completed_target\": " << r.last_completed_target << ",\n";
    std::cout << "  \"gap_percent\": " << r.gap_percent << ",\n";
    std::cout << "  \"nodes\": " << r.nodes << ",\n";
    std::cout << "  \"hash_hits\": " << r.hash_hits << ",\n";
    std::cout << "  \"hash_entries\": " << r.hash_entries << ",\n";
    std::cout << "  \"dominance_keys\": " << r.dominance_keys << ",\n";
    std::cout << "  \"frontier_states\": " << r.frontier_states << ",\n";
    std::cout << "  \"relaxation_probes\": " << r.relaxation_probes << ",\n";
    std::cout << "  \"relaxation_successes\": " << r.relaxation_successes << ",\n";
    std::cout << "  \"relaxation_merges\": " << r.relaxation_merges << ",\n";
    std::cout << "  \"profile_force_close_seconds\": " << r.profile.force_close_seconds << ",\n";
    std::cout << "  \"profile_move_generation_seconds\": " << r.profile.move_generation_seconds << ",\n";
    std::cout << "  \"profile_hash_lookup_seconds\": " << r.profile.hash_lookup_seconds << ",\n";
    std::cout << "  \"profile_hash_insert_seconds\": " << r.profile.hash_insert_seconds << ",\n";
    std::cout << "  \"profile_relaxation_probe_seconds\": " << r.profile.relaxation_probe_seconds << ",\n";
    std::cout << "  \"profile_frontier_seconds\": " << r.profile.frontier_seconds << ",\n";
    std::cout << "  \"profile_warm_start_seconds\": " << r.profile.warm_start_seconds << ",\n";
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
    std::cout << "  \"warm_start_accepted\": " << (r.warm_start_accepted ? "true" : "false") << ",\n";
    std::cout << "  \"warm_start_sequence_accepted\": " << (r.warm_start_sequence_accepted ? "true" : "false") << ",\n";
    std::cout << "  \"elapsed_seconds\": " << r.elapsed_seconds << ",\n";
    std::cout << "  \"best_sequence_0_based\": [";
    for (size_t i = 0; i < r.best_pattern_order.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << r.best_pattern_order[i];
    }
    std::cout << "]\n";
    std::cout << "}\n";
}

} // namespace csbb

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
