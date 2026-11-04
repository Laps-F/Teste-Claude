// Functional allocation probe. Reusable buffers should stop allocating once
// the largest interval has been evaluated. This instrumentation is test-only.
#include "mosp/adaptive.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <numeric>

namespace {
std::atomic<std::size_t> allocations{0};
std::atomic<bool> tracking{false};
void* allocate(std::size_t n) {
    if (void* p = std::malloc(n ? n : 1)) {
        if (tracking.load(std::memory_order_relaxed)) ++allocations;
        return p;
    }
    throw std::bad_alloc();
}
}
void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main() {
    mosp::PatternItems patterns(65);
    for (std::size_t j = 0; j < patterns.size(); ++j)
        for (int x = 0; x < 1031; ++x)
            if ((static_cast<int>(j) * 7 + x) % 11 < 3) patterns[j].push_back(x);
    auto instance = mosp::Instance::from_patterns(1031, std::move(patterns));
    mosp::Permutation p(65); std::iota(p.begin(), p.end(), 0);
    auto q = p; std::reverse(q.begin(), q.end());
    mosp::Evaluator full(instance);
    mosp::IncrementalEvaluator state(instance, p);
    full(p); full(q);
    state.evaluate(mosp::Move::reverse(0, 64)); state.evaluate(q);
    state.apply(q); state.apply(p);
    std::vector<mosp::Decision> rows;
    for(auto input:{mosp::InputKind::Solution,mosp::InputKind::Insert,mosp::InputKind::Swap,mosp::InputKind::Reverse}){
        rows.push_back({input,2,mosp::Backend::Incremental});
        rows.push_back({input,65,mosp::Backend::Full});
    }
    auto plan=mosp::SelectionPlan::from_decisions(instance,rows);
    mosp::AdaptiveEvaluator automatic(instance,p,mosp::Backend::Auto,plan);
    automatic.evaluate(q);automatic.apply(q);automatic.apply(p);
    mosp::ItemOrderDecoder decoder(mosp::ItemOrderDecoder::Data::create(instance));
    mosp::Permutation items(1031);std::iota(items.begin(),items.end(),0);decoder.decode(items);
    long long checksum = 0;
    tracking = true;
    for (int i = 0; i < 10000; ++i) {
        checksum += full(q);
        checksum += state.evaluate(q);
        checksum += state.evaluate(mosp::Move::insert(0, 64));
        checksum += state.apply(mosp::Move::reverse(0, 64));
        checksum += state.apply(mosp::Move::reverse(0, 64));
        checksum += state.apply(q);
        checksum += state.apply(p);
        checksum += automatic.evaluate(q).total_open;
        checksum += automatic.evaluate(mosp::Move::swap(0,1)).total_open;
        checksum += automatic.apply(q).total_open;
        checksum += automatic.apply(p).total_open;
        checksum += static_cast<long long>(decoder.decode(items).size());
    }
    tracking = false;
    std::cout << "{\"allocation_probe\":\"" << (allocations == 0 ? "PASS" : "FAIL")
              << "\",\"iterations\":10000,\"allocations_after_warmup\":" << allocations
              << ",\"checksum\":" << checksum << "}\n";
    return allocations == 0 ? 0 : 1;
}
