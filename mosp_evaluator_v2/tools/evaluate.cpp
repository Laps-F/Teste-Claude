#include "mosp/adaptive.hpp"

#include <climits>
#include <fstream>
#include <iostream>
#include <limits>
#include <iomanip>
#include <numeric>
#include <map>
#include <stdexcept>
#include <string>

namespace {
void usage(std::ostream& out) {
    out << "MOSP evaluator 2.0.0\n"
        << "Usage: mosp-evaluate --matrix FILE --solution FILE --axis rows|columns\n"
        << "                     --presence one|positive [--index-base 0|1] [--profile]\n"
        << "                     [--evaluator full|incremental|auto] [--energy mosp|mmosp]\n"
        << "                     [--sequence patterns|items]\n"
        << "Matrix: row_count column_count, then exactly that many integer entries.\n"
        << "Axis specifies where the sequenced patterns are stored.\n"
        << "Solution: pattern IDs in order, without a length header.\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::map<std::string, std::string> options;
        bool show_profile = false;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--help") { usage(std::cout); return 0; }
            if (key == "--version") { std::cout << "2.0.0\n"; return 0; }
            if (key == "--profile") {
                if (show_profile) throw std::invalid_argument("duplicate --profile");
                show_profile = true; continue;
            }
            if (key != "--matrix" && key != "--solution" && key != "--axis" &&
                key != "--presence" && key != "--index-base" && key != "--evaluator" &&
                key != "--energy" && key != "--sequence")
                throw std::invalid_argument("unknown option: " + key);
            if (i + 1 == argc) throw std::invalid_argument("missing value for " + key);
            if (!options.emplace(key, argv[++i]).second)
                throw std::invalid_argument("duplicate option: " + key);
        }
        for (const auto* key : {"--matrix", "--solution", "--axis", "--presence"})
            if (!options.count(key)) throw std::invalid_argument(std::string("missing ") + key);
        const std::string axis_text = options.at("--axis"), presence_text = options.at("--presence");
        if (axis_text != "rows" && axis_text != "columns")
            throw std::invalid_argument("--axis must be rows or columns");
        if (presence_text != "one" && presence_text != "positive")
            throw std::invalid_argument("--presence must be one or positive");
        const auto base_it = options.find("--index-base");
        const std::string base_text = base_it == options.end() ? "0" : base_it->second;
        if (base_text != "0" && base_text != "1")
            throw std::invalid_argument("--index-base must be 0 or 1");
        const int id_base = base_text == "1" ? 1 : 0;
        std::ifstream matrix(options.at("--matrix"));
        if (!matrix) throw std::runtime_error("cannot open matrix file");
        long long rows = 0, columns = 0;
        if (!(matrix >> rows >> columns) || rows < 0 || columns < 0 || rows > INT_MAX || columns > INT_MAX)
            throw std::invalid_argument("invalid matrix dimensions");
        const auto r = static_cast<std::size_t>(rows), c = static_cast<std::size_t>(columns);
        if (c && r > std::numeric_limits<std::size_t>::max() / c)
            throw std::length_error("matrix size overflow");
        std::vector<int> entries(r * c);
        for (auto& value : entries) {
            long long v = 0;
            if (!(matrix >> v) || v < INT_MIN || v > INT_MAX)
                throw std::invalid_argument("missing or invalid matrix entry");
            value = static_cast<int>(v);
        }
        std::string extra;
        if (matrix >> extra) throw std::invalid_argument("extra data after matrix");
        auto instance = mosp::Instance::from_dense(r, c, entries,
            axis_text == "rows" ? mosp::MatrixAxis::PatternsInRows : mosp::MatrixAxis::PatternsInColumns,
            presence_text == "one" ? mosp::Presence::EqualToOne : mosp::Presence::Positive);
        const auto evaluator_it=options.find("--evaluator"),energy_it=options.find("--energy"),sequence_it=options.find("--sequence");
        const auto backend=mosp::parse_backend(evaluator_it==options.end()?"full":evaluator_it->second);
        const auto energy=mosp::parse_energy(energy_it==options.end()?"mosp":energy_it->second);
        const std::string sequence=sequence_it==options.end()?"patterns":sequence_it->second;
        if(sequence!="patterns"&&sequence!="items")throw std::invalid_argument("--sequence must be patterns or items");
        const auto order_size=sequence=="patterns"?instance->pattern_count():instance->item_count();
        std::ifstream input(options.at("--solution"));
        if (!input) throw std::runtime_error("cannot open solution file");
        mosp::Permutation solution;
        solution.reserve(order_size);
        for (std::size_t i = 0; i < order_size; ++i) {
            long long id = 0;
            if (!(input >> id) || id < id_base || id - id_base > INT_MAX)
                throw std::invalid_argument("missing or invalid solution ID");
            solution.push_back(static_cast<int>(id - id_base));
        }
        if (input >> extra) throw std::invalid_argument("extra data after solution");
        if(sequence=="items"){
            mosp::ItemOrderDecoder decoder(mosp::ItemOrderDecoder::Data::create(instance));
            solution=decoder.decode(solution);
        }
        mosp::Permutation initial(instance->pattern_count());std::iota(initial.begin(),initial.end(),0);
        mosp::AdaptiveEvaluator adaptive(instance,initial,backend);
        const auto metrics=adaptive.evaluate(solution);
        std::cout << std::setprecision(17) << "{\"objective\":" << metrics.max_open << ",\"patterns\":" << instance->pattern_count()
                  << ",\"items\":" << instance->item_count()<<",\"total_open\":"<<metrics.total_open
                  <<",\"mmosp\":"<<metrics.mmosp()<<",\"energy\":"<<metrics.energy(energy)
                  <<",\"energy_mode\":\""<<mosp::energy_name(energy)<<"\",\"evaluator\":\""<<mosp::backend_name(backend)
                  <<"\",\"density\":"<<instance->density()<<",\"calibration_seconds\":"<<(adaptive.plan()?adaptive.plan()->calibration_seconds():0);
        if (show_profile) {
            mosp::Evaluator evaluator(instance);evaluator.evaluate(solution);
            std::cout << ",\"profile\":[";
            for (std::size_t i = 0; i < evaluator.profile().size(); ++i)
                std::cout << (i ? "," : "") << evaluator.profile()[i];
            std::cout << ']';
        }
        std::cout << "}\n";
    } catch (const std::exception& e) {
        std::cerr << "mosp-evaluate: " << e.what() << '\n';
        return 2;
    }
}
