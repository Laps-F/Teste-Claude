#ifndef CSBB_MOSP_BRIDGE_HPP
#define CSBB_MOSP_BRIDGE_HPP
#include "mosp/evaluator.hpp"
#include <memory>
#include <stdexcept>
#include <vector>

namespace csbb {
// Explicit, immutable transpose of the solver's CUSTOMER x PRODUCT incidence.
// The library's pattern order is the PRODUCT permutation; stacks are CUSTOMERS.
class MospEvaluationBridge {
public:
    explicit MospEvaluationBridge(const std::vector<std::vector<unsigned char>>& customer_product) {
        const std::size_t customers = customer_product.size();
        if (!customers) throw std::invalid_argument("MOSP bridge: empty customer matrix");
        const std::size_t products = customer_product.front().size();
        if (!products) throw std::invalid_argument("MOSP bridge: no products");
        mosp::PatternItems by_product(products);
        for (std::size_t c = 0; c < customers; ++c) {
            if (customer_product[c].size() != products)
                throw std::invalid_argument("MOSP bridge: ragged matrix");
            for (std::size_t p = 0; p < products; ++p)
                if (customer_product[c][p] != 0) by_product[p].push_back(static_cast<int>(c));
        }
        instance_ = mosp::Instance::from_patterns(customers, std::move(by_product));
    }

    // Each call owns its mutable evaluator; safe to call concurrently on a shared bridge.
    int evaluate(const std::vector<int>& product_order) const {
        mosp::Evaluator evaluator(instance_);
        return evaluator.evaluate(product_order);
    }
    const mosp::Instance::Ptr& shared_instance() const noexcept { return instance_; }
    std::size_t incidences() const noexcept { return instance_->incidence_count(); }
    std::size_t products() const noexcept { return instance_->pattern_count(); }
    std::size_t customers() const noexcept { return instance_->item_count(); }
private:
    mosp::Instance::Ptr instance_;
};
}
#endif
