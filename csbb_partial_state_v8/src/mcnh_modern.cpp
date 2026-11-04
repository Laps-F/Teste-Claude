/*
 * Modernized MCNh / HNCM heuristic for the Minimization of Open Stacks Problem.
 *
 * This file is an engineering modernization scaffold derived from the legacy HNCM.h
 * and LinkedList.h implementation. It preserves the main algorithmic flow while
 * separating parsing, optional dominance preprocessing, heuristic construction,
 * evaluation, and solution export.
 *
 * Compatibility notes:
 * - The legacy SCOOP-style input order is preserved: first line nPieces nPatterns,
 *   followed by nPieces rows and nPatterns columns, internally read with pieces in
 *   reverse order, as in the original code.
 * - Dominance preprocessing is optional (--preprocess 0/1).
 * - Presence can be interpreted as positive entries (--presence positive) or only
 *   entries equal to one (--presence legacy-one). The legacy code was designed for
 *   binary matrices, although some benchmark formats store positive multiplicities.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mcnh {

enum class PresenceMode { Positive, LegacyOne };

struct Options {
    bool preprocess = true;
    bool append_unseen_pieces = true;
    PresenceMode presence_mode = PresenceMode::Positive;
    std::string input_path;
    std::string input_format = "scoop"; // scoop | cs
    std::string objective_orientation = "piece"; // piece (legacy) | customer (CS B&B warm start)
    std::string output_path;
    std::string warm_start_path;
};

struct Instance {
    int n_pieces = 0;
    int n_patterns = 0;
    std::vector<std::vector<int>> matrix; // pattern x piece
};

struct Result {
    int active_value = -1;
    int expanded_value = -1;
    double time_seconds = 0.0;
    int n_removed_patterns = 0;
    std::vector<int> active_sequence;   // 0-based original pattern ids, without dominated insertions
    std::vector<int> expanded_sequence; // 0-based original pattern ids, including dominated insertions
    std::vector<int> piece_sequence;    // 0-based piece ids
};

class BitMatrix {
public:
    BitMatrix() = default;
    explicit BitMatrix(int n) { reset(n); }

    void reset(int n_) {
        n = n_;
        words_per_row = (n + 63) / 64;
        data.assign(static_cast<size_t>(n) * words_per_row, 0ULL);
    }

    bool test(int i, int j) const {
        const size_t idx = static_cast<size_t>(i) * words_per_row + static_cast<size_t>(j >> 6);
        return (data[idx] >> (j & 63)) & 1ULL;
    }

    void set(int i, int j) {
        const size_t idx = static_cast<size_t>(i) * words_per_row + static_cast<size_t>(j >> 6);
        data[idx] |= (1ULL << (j & 63));
    }

    void clear(int i, int j) {
        const size_t idx = static_cast<size_t>(i) * words_per_row + static_cast<size_t>(j >> 6);
        data[idx] &= ~(1ULL << (j & 63));
    }

private:
    int n = 0;
    int words_per_row = 0;
    std::vector<std::uint64_t> data;
};

class Solver {
public:
    explicit Solver(Options opts) : options(std::move(opts)) {}

    Result run() {
        const auto t0 = std::chrono::steady_clock::now();
        read_instance();
        initialize_state();

        if (options.preprocess) {
            dominance_preprocessing();
        }

        build_graph();
        run_hncm();
        build_active_pattern_sequence();
        build_expanded_sequence();

        Result result;
        result.active_sequence = active_sequence;
        result.expanded_sequence = expanded_sequence;
        result.piece_sequence = piece_sequence;
        result.n_removed_patterns = removed_count;
        result.active_value = evaluate(active_sequence);
        result.expanded_value = evaluate(expanded_sequence);

        const auto t1 = std::chrono::steady_clock::now();
        result.time_seconds = std::chrono::duration<double>(t1 - t0).count();

        if (!options.output_path.empty()) {
            write_solution(result);
        }
        if (!options.warm_start_path.empty()) {
            write_warm_start(result);
        }
        return result;
    }

private:
    Options options;
    Instance inst;

    std::vector<int> pattern_size;
    std::vector<char> removed;
    std::vector<char> blocked;
    std::vector<std::vector<int>> dominated_by; // dominated_by[dominator] = dominated pattern ids

    BitMatrix adjacency;
    std::vector<int> degree;
    std::vector<char> vertex_removed;
    std::vector<char> vertex_open;
    std::vector<int> open_vertices;
    std::vector<std::pair<int, int>> arcs;
    std::vector<int> piece_sequence;
    std::vector<int> active_sequence;
    std::vector<int> expanded_sequence;

    int n_arcs = 0;
    int removed_count = 0;

    bool present(int pattern_id, int piece_id) const {
        const int v = inst.matrix[pattern_id][piece_id];
        return options.presence_mode == PresenceMode::Positive ? (v > 0) : (v == 1);
    }

    void read_instance() {
        std::ifstream in(options.input_path);
        if (!in) {
            throw std::runtime_error("Could not open input file: " + options.input_path);
        }

        if (options.input_format == "scoop") {
            in >> inst.n_pieces >> inst.n_patterns;
            if (!in || inst.n_pieces <= 0 || inst.n_patterns <= 0) {
                throw std::runtime_error("Invalid SCOOP instance header in: " + options.input_path);
            }
            inst.matrix.assign(inst.n_patterns, std::vector<int>(inst.n_pieces, 0));

            // Preserve legacy reading order: pieces are read from nPieces-1 down to 0.
            for (int j = inst.n_pieces - 1; j >= 0; --j) {
                for (int i = 0; i < inst.n_patterns; ++i) {
                    if (!(in >> inst.matrix[i][j])) {
                        throw std::runtime_error("Unexpected end of SCOOP input while reading matrix.");
                    }
                }
            }
        } else if (options.input_format == "cs") {
            // Frinhani--Carvalho--Soma / Chu--Stuckey random format:
            // first line m n, followed by an m x n pattern-by-piece matrix.
            in >> inst.n_patterns >> inst.n_pieces;
            if (!in || inst.n_pieces <= 0 || inst.n_patterns <= 0) {
                throw std::runtime_error("Invalid CS instance header in: " + options.input_path);
            }
            inst.matrix.assign(inst.n_patterns, std::vector<int>(inst.n_pieces, 0));
            for (int i = 0; i < inst.n_patterns; ++i) {
                for (int j = 0; j < inst.n_pieces; ++j) {
                    if (!(in >> inst.matrix[i][j])) {
                        throw std::runtime_error("Unexpected end of CS input while reading matrix.");
                    }
                }
            }
        } else {
            throw std::runtime_error("Invalid --input-format: " + options.input_format);
        }
        if (options.objective_orientation == "customer") {
            // MCNh traditionally sequences ROW patterns and counts COLUMN stacks.
            // Chu--Stuckey CS instead counts ROW customer stacks and sequences
            // COLUMN products. Transpose the same original incidence matrix so
            // the heuristic generates a PRODUCT permutation valid for CS B&B.
            std::vector<std::vector<int>> dual(
                static_cast<size_t>(inst.n_pieces), std::vector<int>(static_cast<size_t>(inst.n_patterns)));
            for (int row = 0; row < inst.n_patterns; ++row)
                for (int col = 0; col < inst.n_pieces; ++col)
                    dual[static_cast<size_t>(col)][static_cast<size_t>(row)] = inst.matrix[row][col];
            std::swap(inst.n_pieces, inst.n_patterns);
            inst.matrix = std::move(dual);
        }
    }

    void initialize_state() {
        pattern_size.assign(inst.n_patterns, 0);
        removed.assign(inst.n_patterns, 0);
        blocked.assign(inst.n_patterns, 0);
        dominated_by.assign(inst.n_patterns, {});

        for (int i = 0; i < inst.n_patterns; ++i) {
            for (int j = 0; j < inst.n_pieces; ++j) {
                pattern_size[i] += present(i, j) ? 1 : 0;
            }
        }
    }

    bool subset_of(int a, int b) const {
        // Is pattern a contained in pattern b?
        for (int k = 0; k < inst.n_pieces; ++k) {
            if (present(a, k) && !present(b, k)) return false;
        }
        return true;
    }

    bool identical(int a, int b) const {
        for (int k = 0; k < inst.n_pieces; ++k) {
            if (present(a, k) != present(b, k)) return false;
        }
        return true;
    }

    void transfer_dominated(int to, int from) {
        auto &dst = dominated_by[to];
        auto &src = dominated_by[from];
        dst.insert(dst.end(), src.begin(), src.end());
        src.clear();
    }

    void dominance_preprocessing() {
        int counter = 0;
        for (int i = 0; i < inst.n_patterns; ++i) {
            if (removed[i]) continue;
            for (int j = i + 1; j < inst.n_patterns; ++j) {
                if (removed[j]) continue;

                int dominated = -1;
                int dominator = -1;

                if (pattern_size[i] < pattern_size[j]) {
                    if (subset_of(i, j)) {
                        dominated = i;
                        dominator = j;
                    }
                } else if (pattern_size[i] == pattern_size[j]) {
                    if (identical(i, j)) {
                        dominated = j;
                        dominator = i;
                    }
                } else { // pattern_size[i] > pattern_size[j]
                    if (subset_of(j, i)) {
                        dominated = j;
                        dominator = i;
                    }
                }

                if (dominated >= 0) {
                    removed[dominated] = 1;
                    ++counter;
                    dominated_by[dominator].push_back(dominated);
                    if (!dominated_by[dominated].empty()) {
                        transfer_dominated(dominator, dominated);
                    }
                    if (dominated == i) break;
                }
            }
        }
        removed_count = counter;
    }

    std::vector<int> active_patterns() const {
        std::vector<int> active;
        active.reserve(inst.n_patterns - removed_count);
        for (int p = 0; p < inst.n_patterns; ++p) {
            if (!removed[p]) active.push_back(p);
        }
        return active;
    }

    void build_graph() {
        adjacency.reset(inst.n_pieces);
        degree.assign(inst.n_pieces, 0);
        vertex_removed.assign(inst.n_pieces, 0);
        vertex_open.assign(inst.n_pieces, 0);
        n_arcs = 0;

        for (int p = 0; p < inst.n_patterns; ++p) {
            if (removed[p]) continue;
            std::vector<int> pieces;
            pieces.reserve(pattern_size[p]);
            for (int j = 0; j < inst.n_pieces; ++j) {
                if (present(p, j)) pieces.push_back(j);
            }
            for (size_t a = 0; a < pieces.size(); ++a) {
                for (size_t b = a + 1; b < pieces.size(); ++b) {
                    const int u = pieces[a];
                    const int v = pieces[b];
                    if (!adjacency.test(u, v)) {
                        adjacency.set(u, v);
                        adjacency.set(v, u);
                        ++degree[u];
                        ++degree[v];
                        ++n_arcs;
                    }
                }
            }
        }
    }

    int find_minimum_degree_vertex() const {
        int best = -1;
        int best_degree = std::numeric_limits<int>::max();
        for (int i = 0; i < inst.n_pieces; ++i) {
            if (!vertex_removed[i] && degree[i] > 0 && degree[i] < best_degree) {
                best = i;
                best_degree = degree[i];
            }
        }
        return best;
    }

    void remove_edge(int u, int v) {
        if (!adjacency.test(u, v)) return;
        adjacency.clear(u, v);
        adjacency.clear(v, u);
        arcs.emplace_back(u, v);
        --degree[u];
        --degree[v];
        if (degree[u] == 0) vertex_removed[u] = 1;
        if (degree[v] == 0) vertex_removed[v] = 1;
    }

    void open_vertex_if_needed(int v) {
        if (!vertex_open[v]) {
            vertex_open[v] = 1;
            open_vertices.push_back(v);
        }
    }

    void run_hncm() {
        int removed_edges = 0;
        while (removed_edges < n_arcs) {
            int omega = find_minimum_degree_vertex();
            if (omega < 0) break;

            int best_pairwise = std::numeric_limits<int>::max();
            int n1 = -1, n2 = -1;

            for (int i = 0; i < inst.n_pieces; ++i) {
                if (degree[i] != degree[omega]) continue;
                for (int j = 0; j < inst.n_pieces; ++j) {
                    if (adjacency.test(i, j) && degree[i] + degree[j] < best_pairwise) {
                        best_pairwise = degree[i] + degree[j];
                        n1 = i;
                        n2 = j;
                    }
                }
            }

            if (n1 < 0 || n2 < 0) break;

            open_vertex_if_needed(n1);
            open_vertex_if_needed(n2);
            if (adjacency.test(n1, n2)) {
                remove_edge(n1, n2);
                ++removed_edges;
            }

            // Preserve the legacy logic: after opening vertices, remove all edges among open vertices.
            for (size_t a = 0; a < open_vertices.size(); ++a) {
                for (size_t b = 0; b < open_vertices.size(); ++b) {
                    const int u = open_vertices[a];
                    const int v = open_vertices[b];
                    if (u != v && adjacency.test(u, v)) {
                        remove_edge(u, v);
                        ++removed_edges;
                    }
                }
            }
        }

        piece_sequence = open_vertices;
        if (options.append_unseen_pieces) {
            std::vector<char> seen(inst.n_pieces, 0);
            for (int v : piece_sequence) seen[v] = 1;
            for (int v = 0; v < inst.n_pieces; ++v) {
                if (!seen[v]) piece_sequence.push_back(v);
            }
        }
    }

    void build_active_pattern_sequence() {
        active_sequence.clear();
        active_sequence.reserve(inst.n_patterns - removed_count);
        std::vector<int> completion(inst.n_patterns, 0);

        for (int p = 0; p < inst.n_patterns; ++p) {
            if (removed[p]) continue;
            for (int j = 0; j < inst.n_pieces; ++j) {
                if (present(p, j)) ++completion[p];
            }
        }

        std::fill(blocked.begin(), blocked.end(), 0);
        for (int piece : piece_sequence) {
            for (int p = 0; p < inst.n_patterns; ++p) {
                if (removed[p] || blocked[p]) continue;
                if (present(p, piece)) {
                    --completion[p];
                    if (completion[p] == 0) {
                        blocked[p] = 1;
                        active_sequence.push_back(p);
                    }
                }
            }
        }

        // Safety: append any active pattern that could not be completed due to degenerate inputs.
        for (int p = 0; p < inst.n_patterns; ++p) {
            if (!removed[p] && !blocked[p]) {
                blocked[p] = 1;
                active_sequence.push_back(p);
            }
        }
    }

    void build_expanded_sequence() {
        expanded_sequence.clear();
        expanded_sequence.reserve(inst.n_patterns);
        for (int p : active_sequence) {
            expanded_sequence.push_back(p);
            for (int q : dominated_by[p]) {
                expanded_sequence.push_back(q);
            }
        }
        // Safety: append any pattern not yet present, preserving feasibility for all inputs.
        std::vector<char> in_sequence(inst.n_patterns, 0);
        for (int p : expanded_sequence) in_sequence[p] = 1;
        for (int p = 0; p < inst.n_patterns; ++p) {
            if (!in_sequence[p]) expanded_sequence.push_back(p);
        }
    }

    int evaluate(const std::vector<int>& sequence) const {
        if (sequence.empty()) return 0;
        std::vector<int> demand(inst.n_pieces, 0);
        std::vector<char> open(inst.n_pieces, 0);
        for (int p : sequence) {
            for (int j = 0; j < inst.n_pieces; ++j) {
                if (present(p, j)) ++demand[j];
            }
        }

        int open_stacks = 0;
        int max_open_stacks = 0;
        for (int p : sequence) {
            int closed_now = 0;
            for (int j = 0; j < inst.n_pieces; ++j) {
                if (!present(p, j)) continue;
                --demand[j];
                if (!open[j]) {
                    open[j] = 1;
                    ++open_stacks;
                }
                if (demand[j] == 0) {
                    ++closed_now;
                }
            }
            max_open_stacks = std::max(max_open_stacks, open_stacks);
            open_stacks -= closed_now;
        }
        return max_open_stacks;
    }

    void write_warm_start(const Result& result) const {
        std::ofstream out(options.warm_start_path);
        if (!out) {
            throw std::runtime_error("Could not write warm-start file: " + options.warm_start_path);
        }
        out << "# MCNh warm start for MOSP branch-and-bound\n";
        out << "value " << result.expanded_value << "\n";
        if (options.objective_orientation == "customer") {
            out << "sequence_domain products\n";
            out << "objective_domain customers\n";
            out << "n_customers " << inst.n_pieces << "\n";
            out << "n_products " << inst.n_patterns << "\n";
        } else {
            out << "sequence_domain patterns\n";
            out << "objective_domain pieces\n";
        }
        out << "n_patterns " << inst.n_patterns << "\n";
        out << "sequence_1_based";
        for (int p : result.expanded_sequence) out << " " << (p + 1);
        out << "\n";
        out << "sequence_0_based";
        for (int p : result.expanded_sequence) out << " " << p;
        out << "\n";
    }

    void write_solution(const Result& result) const {
        std::ofstream out(options.output_path);
        if (!out) {
            throw std::runtime_error("Could not write output file: " + options.output_path);
        }
        out << "Maximum Open Stacks: " << result.expanded_value << "\n";
        out << "Active-only Maximum Open Stacks: " << result.active_value << "\n";
        out << "Number of Pieces: " << inst.n_pieces
            << ", Number of Patterns: " << inst.n_patterns
            << ", Patterns eliminated in the Pre-processing: " << result.n_removed_patterns << "\n";
        out << "Patterns Sequence:\n";
        for (int p : result.expanded_sequence) {
            out << (p + 1);
            if (removed[p]) out << " [dominated]";
            out << "\n";
        }
        out << "\nPiece Sequence:\n";
        for (int v : result.piece_sequence) out << (v + 1) << "\n";
    }
};

Options parse_options(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto require_value = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("Missing value for " + name);
            return argv[++i];
        };

        if (arg == "--input" || arg == "-i") {
            opts.input_path = require_value(arg);
        } else if (arg == "--input-format") {
            opts.input_format = require_value(arg);
        } else if (arg == "--output" || arg == "-o") {
            opts.output_path = require_value(arg);
        } else if (arg == "--warm-start-output") {
            opts.warm_start_path = require_value(arg);
        } else if (arg == "--objective-orientation") {
            opts.objective_orientation = require_value(arg);
        } else if (arg == "--preprocess") {
            opts.preprocess = (require_value(arg) != "0");
        } else if (arg == "--presence") {
            const std::string v = require_value(arg);
            if (v == "positive") opts.presence_mode = PresenceMode::Positive;
            else if (v == "legacy-one") opts.presence_mode = PresenceMode::LegacyOne;
            else throw std::runtime_error("Invalid --presence value: " + v);
        } else if (arg == "--append-unseen-pieces") {
            opts.append_unseen_pieces = (require_value(arg) != "0");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: mcnh_modern --input FILE [--input-format scoop|cs] [--output FILE] [--preprocess 0|1] "
                         "[--presence positive|legacy-one] [--objective-orientation piece|customer] [--append-unseen-pieces 0|1] [--warm-start-output FILE]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }
    if (opts.input_path.empty()) throw std::runtime_error("Missing --input FILE");
    if (opts.objective_orientation != "piece" && opts.objective_orientation != "customer")
        throw std::runtime_error("Invalid --objective-orientation. Use piece or customer.");
    if (opts.input_format != "scoop" && opts.input_format != "cs") {
        throw std::runtime_error("Invalid --input-format. Use scoop or cs.");
    }
    return opts;
}

} // namespace mcnh

int main(int argc, char** argv) {
    try {
        auto opts = mcnh::parse_options(argc, argv);
        mcnh::Solver solver(opts);
        const mcnh::Result result = solver.run();
        std::cout << "value=" << result.expanded_value
                  << " active_value=" << result.active_value
                  << " removed=" << result.n_removed_patterns
                  << " patterns=" << result.expanded_sequence.size()
                  << " time=" << result.time_seconds << "\n";
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
