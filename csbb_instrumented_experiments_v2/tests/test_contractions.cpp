// Property-based graph-contraction test, independent subset-DP oracle.
#define main csbb_source_entrypoint
#include "../src/csbb_modern.cpp"
#undef main
#include <cassert>
#include <random>
#include <iostream>
using namespace csbb;

int oracle(const std::vector<Bitset>& rows, const Bitset& active) {
    std::vector<int> vertices;
    active.for_each_set_bit([&](int v) {vertices.push_back(v);});
    const int n = static_cast<int>(vertices.size());
    if (n == 0) return 0;
    const int full = (1<<n)-1;
    std::vector<int> adj(n,0);
    for (int i=0;i<n;i++) for (int j=0;j<n;j++)
        if (rows[vertices[i]].test(vertices[j])) adj[i] |= 1<<j;
    std::vector<int> dp(1<<n,-1);
    std::function<int(int)> solve = [&](int remaining)->int {
        if (!remaining) return 0;
        int& ans=dp[remaining];
        if (ans >= 0) return ans;
        int opened = 0;
        int processed=full^remaining;
        for (int i=0;i<n;i++) if (processed & (1<<i)) opened |= adj[i];
        ans=n;
        for (int i=0;i<n;i++) if (remaining & (1<<i)) {
            int peak=__builtin_popcount(static_cast<unsigned>((opened|adj[i])&remaining));
            int val=std::max(peak,solve(remaining & ~(1<<i)));
            ans=std::min(ans,val);
        }
        return ans;
    };
    return solve(full);
}

int main() {
    std::mt19937 gen(0x2fa13);
    int snapshots = 0, comparisons = 0;
    for (int trial=0;trial<320;trial++) {
        int n=3+(gen()%9);
        std::vector<Bitset> rows(n,Bitset(n));
        for (int i=0;i<n;i++) rows[i].set(i);
        int threshold=15+gen()%75;
        for (int i=0;i<n;i++) for(int j=i+1;j<n;j++) if(int(gen()%100)<threshold) {
            rows[i].set(j); rows[j].set(i);
        }
        RelaxationGraph g(rows);
        auto original = rows;
        int ref=oracle(rows,g.active);
        assert(g.valid());
        std::vector<std::vector<Bitset>> past_rows;
        std::vector<Bitset> past_active;
        for (int step=0;step<n-1;step++) {
            std::vector<std::pair<int,int>> edges;
            g.active.for_each_set_bit([&](int a){
                g.rows[a].for_each_set_bit([&](int b){if (a!=b && g.active.test(b)) edges.emplace_back(a,b);});
            });
            if(edges.empty()) break;
            auto [a,b]=edges[gen()%edges.size()];
            past_rows.push_back(g.rows); past_active.push_back(g.active);
            assert(g.merge(a,b));
            assert(g.valid());
            int minor=oracle(g.rows,g.active);
            if(minor>ref) {
                std::cerr<<"INVALID MINOR ",std::cerr << "trial="<<trial<<" minor="<<minor<<" original="<<ref<<"\n";
                return 2;
            }
            comparisons++;
            snapshots++;
        }
        for (int i=static_cast<int>(past_rows.size())-1;i>=0;i--) {
            assert(g.unmerge());
            assert(g.rows==past_rows[i]);
            assert(g.active==past_active[i]);
        }
        assert(g.rows==original);
        assert(g.log.empty());
    }
    std::cout<<"PASS reversible_contractions="<<snapshots<<" lower_bound_monotonicity_checks="<<comparisons<<"\n";
}
