#define CSBB_LIBRARY_ONLY
#include "../src/csbb_modern.cpp"
#include <random>
#include <iostream>
#include <map>

using csbb::Bitset;
using csbb::ResidualBounds;

int main() {
    std::mt19937_64 rng(20261028);
    long checked = 0, core_tests = 0, root_checks = 0;
    for (int experiment = 0; experiment < 240; ++experiment) {
        int n = 2 + (rng() % 9);
        int products = 2 + (rng() % 14);
        std::vector<Bitset> graph(n, Bitset(n));
        for (int i=0;i<n;i++) graph[i].set(i);
        double density = (experiment % 4 == 0) ? 0.12 : (experiment % 4 == 1) ? 0.3 : (experiment % 4 == 2) ? 0.6 : 0.92;
        for (int j=0;j<products;j++) {
            std::vector<int> group;
            for (int i=0;i<n;i++) if (std::generate_canonical<double, 30>(rng) < density) group.push_back(i);
            for (int a:group) for (int b:group) graph[a].set(b);
        }
        // Cross graph may contain isolated customers: these are valid for the
        // standalone graph operator tests. (Input parser enforces nonempty rows.)
        Bitset full(n);
        for (int i=0;i<n;i++) full.set(i);
        int maxm = 1 << n;
        std::vector<int> memo(maxm,-1);
        std::function<int(int)> dp = [&](int mask)->int {
            if(mask==0) return 0;
            if(memo[mask]>=0) return memo[mask];
            Bitset opened(n);
            Bitset rem(n);
            for(int i=0;i<n;i++) {
                if(mask & (1<<i)) rem.set(i);
                else opened.inplace_or(graph[i]);
            }
            int best=n;
            for(int i=0;i<n;i++) if(mask & (1<<i)) {
                auto next=opened.bit_or(graph[i]);
                int cost=next.bit_and(rem).count();
                best=std::min(best,std::max(cost,dp(mask & ~(1<<i))));
            }
            return memo[mask]=best;
        };
        int optimum=dp(maxm-1);
        int base=ResidualBounds::degeneracy_plus_one(graph,full);
        if (base>optimum) {std::cerr << "invalid root core" << experiment <<" "<<base <<" "<< optimum<<"\n";return 1;}
        root_checks++;
        for(int mask=1;mask<maxm;mask++) {
            Bitset opened(n), rem(n);
            for(int i=0;i<n;i++) {
                if(mask & (1<<i)) rem.set(i);
                else opened.inplace_or(graph[i]);
            }
            int exact=dp(mask);
            int first=ResidualBounds::first_closure(graph,opened,rem);
            if(first>exact) {std::cerr<<"invalid first "<<experiment<<" mask "<<mask<<": "<<first<<">"<<exact<<"\n";return 2;}
            ++checked;
            for(int k=1;k<=n;k++) {
                bool core=ResidualBounds::core_exceeds_target(graph,rem,k);
                if (core && exact<=k) {
                    std::cerr <<"invalid core "<<experiment<<" mask "<<mask<<" k "<<k<<" exact "<<exact<<"\n";
                    return 3;
                }
                ++core_tests;
            }
        }
    }
    std::cout<<"PASS root_checks="<<root_checks<<" states="<<checked<<" targeted_core_checks="<<core_tests<<"\n";
}
