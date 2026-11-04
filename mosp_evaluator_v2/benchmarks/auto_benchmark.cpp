#include "mosp/adaptive.hpp"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
using namespace mosp;
namespace {
Permutation moved(Permutation p,Move move){
    if(move.kind==MoveKind::Insert){int x=p[static_cast<std::size_t>(move.from)];p.erase(p.begin()+move.from);p.insert(p.begin()+move.to,x);}
    else if(move.kind==MoveKind::Swap)std::swap(p[static_cast<std::size_t>(move.from)],p[static_cast<std::size_t>(move.to)]);
    else std::reverse(p.begin()+std::min(move.from,move.to),p.begin()+std::max(move.from,move.to)+1);
    return p;
}
const char* move_name(MoveKind m){return m==MoveKind::Insert?"insert":m==MoveKind::Swap?"swap":"reverse";}
}
int main(){try{
    std::mt19937 rng(20261030);
    std::cout<<std::setprecision(15)<<"patterns,items,density,input,move,distance,accept_fraction,samples,repetition,backend,seconds,calibration_seconds,full_calls,incremental_calls,sum_mosp,sum_profile,sum_energy\n";
    for(int n:{200,1000})for(double density:{0.02,0.2,0.8}){
        PatternItems patterns(static_cast<std::size_t>(n));
        for(auto& p:patterns)for(int x=0;x<n;++x)if(static_cast<double>(rng())/rng.max()<density)p.push_back(x);
        auto instance=Instance::from_patterns(static_cast<std::size_t>(n),std::move(patterns));
        Permutation initial(static_cast<std::size_t>(n));std::iota(initial.begin(),initial.end(),0);std::shuffle(initial.begin(),initial.end(),rng);
        const auto plan=SelectionPlan::calibrate(instance,initial);
        for(bool by_move:{false,true})for(auto kind:{MoveKind::Insert,MoveKind::Swap,MoveKind::Reverse})for(int distance:{1,n/7,n/2}){
            const int samples=n==200?300:100;
            std::vector<Move> moves;std::vector<Permutation> candidates;auto trajectory=initial;
            for(int i=0;i<samples;++i){
                int from=static_cast<int>(rng()%(n-distance)),to=from+distance;if(rng()%2)std::swap(from,to);
                Move move{kind,from,to};auto q=moved(trajectory,move);moves.push_back(move);candidates.push_back(q);
                if(i%2==0)trajectory=q;
            }
            std::int64_t expected_m=0,expected_s=0;double expected_e=0;
            for(int rep=1;rep<=3;++rep)for(auto backend:{Backend::Full,Backend::Incremental,Backend::Auto}){
                AdaptiveEvaluator state(instance,initial,backend,plan);
                auto warm=initial;std::reverse(warm.begin(),warm.end());state.apply(warm);state.apply(initial);
                std::int64_t sum_m=0,sum_s=0;double sum_e=0;
                const auto start=std::chrono::steady_clock::now();
                for(int i=0;i<samples;++i){
                    const auto k=static_cast<std::size_t>(i);auto metrics=by_move?state.evaluate(moves[k]):state.evaluate(candidates[k]);
                    sum_m+=metrics.max_open;sum_s+=metrics.total_open;sum_e+=metrics.mmosp();
                    if(i%2==0){if(by_move)state.apply(moves[k]);else state.apply(candidates[k]);}
                }
                const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
                if(rep==1&&backend==Backend::Full){expected_m=sum_m;expected_s=sum_s;expected_e=sum_e;}
                if(sum_m!=expected_m||sum_s!=expected_s||sum_e!=expected_e||state.solution()!=trajectory)
                    throw std::logic_error("benchmark trajectory/metrics mismatch");
                Evaluator verifier(instance);if(state.metrics()!=verifier.evaluate_metrics(trajectory))throw std::logic_error("benchmark final state");
                std::cout<<n<<','<<n<<','<<density<<','<<(by_move?"move":"solution")<<','<<move_name(kind)<<','<<distance
                    <<",0.5,"<<samples<<','<<rep<<','<<backend_name(backend)<<','<<seconds<<','<<(backend==Backend::Auto?plan->calibration_seconds():0)<<','
                    <<state.counts().full<<','<<state.counts().incremental<<','<<sum_m<<','<<sum_s<<','<<sum_e<<'\n';
            }
        }
    }
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
