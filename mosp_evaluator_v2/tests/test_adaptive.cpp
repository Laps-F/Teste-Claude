#include "mosp/adaptive.hpp"
#include <algorithm>
#include <cmath>
#include <future>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <tuple>

using namespace mosp;
namespace {
std::uint64_t checks = 0, scenarios = 0;
void need(bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }
template<class F> void throws(F f) {
    bool caught = false; try { f(); } catch (const std::exception&) { caught = true; }
    need(caught, "expected input error");
}
Permutation moved(Permutation p, Move move) {
    if (move.kind == MoveKind::Insert) {
        int id = p[static_cast<std::size_t>(move.from)];
        p.erase(p.begin() + move.from); p.insert(p.begin() + move.to, id);
    } else if (move.kind == MoveKind::Swap)
        std::swap(p[static_cast<std::size_t>(move.from)], p[static_cast<std::size_t>(move.to)]);
    else std::reverse(p.begin() + std::min(move.from, move.to), p.begin() + std::max(move.from, move.to) + 1);
    return p;
}
Metrics oracle(const Instance& instance, const Permutation& p) {
    const int n = static_cast<int>(p.size());
    std::vector<int> first(instance.item_count(), n), last(instance.item_count(), -1), diff(p.size()+1);
    std::int64_t total = 0;
    for (int t=0;t<n;++t) for(int x:instance.patterns()[static_cast<std::size_t>(p[static_cast<std::size_t>(t)])]) {
        first[static_cast<std::size_t>(x)]=std::min(first[static_cast<std::size_t>(x)],t);
        last[static_cast<std::size_t>(x)]=t;
    }
    for(std::size_t x=0;x<first.size();++x) if(last[x]>=0) {
        total += last[x]-first[x]+1;
        ++diff[static_cast<std::size_t>(first[x])];--diff[static_cast<std::size_t>(last[x]+1)];
    }
    int active=0,peak=0;for(int t=0;t<n;++t) peak=std::max(peak,active+=diff[static_cast<std::size_t>(t)]);
    return {peak,total,p.size()};
}
SelectionPlan::Ptr mixed_plan(Instance::Ptr instance) {
    std::vector<Decision> rows;
    for (auto kind : {InputKind::Solution, InputKind::Insert, InputKind::Swap, InputKind::Reverse}) {
        rows.push_back({kind, 2, Backend::Incremental});
        rows.push_back({kind, std::max<std::size_t>(3,instance->pattern_count()), Backend::Full});
    }
    return SelectionPlan::from_decisions(instance, rows);
}
void score_tests() {
    auto instance=Instance::from_patterns(5,{{0,1,2},{1,2,4},{0,3},{1,4}});
    Evaluator f(instance);
    const auto a=f.evaluate_metrics({0,1,2,3}),b=f.evaluate_metrics({1,0,3,2});
    need(a==Metrics{4,13,4} && b==Metrics{4,12,4},"published MMOSP example metrics");
    need(a.mmosp()==4.8125 && b.mmosp()==4.75,"published MMOSP example energy");
    need((Metrics{3,6,2}).mmosp()==4.0,"unit correction must not be truncated");
    need((Metrics{}).mmosp()==0,"empty energy");
    need((Metrics{100000,INT64_C(5000000000),100000}).mmosp()==100000.5,"64-bit sum");
    need(a.energy(Energy::MOSP)==4,"raw MOSP");
    throws([&]{a.energy(static_cast<Energy>(7));});
    throws([]{parse_backend("bad");}); throws([]{parse_energy("bad");});
    need(instance->density()==0.5,"density calculation");
    // Exhaust all short profiles to check strict primary-objective preservation.
    std::vector<Metrics> profiles;
    for(int mask=0;mask<625;++mask) {
        int bits=mask,peak=0,sum=0;
        for(int t=0;t<4;++t) {int h=bits%5;bits/=5;peak=std::max(peak,h);sum+=h;}
        profiles.push_back({peak,sum,4});
    }
    for(auto x:profiles)for(auto y:profiles) {
        if(x.max_open<y.max_open) need(x.mmosp()<y.mmosp(),"primary objective dominance");
        if(x.max_open==y.max_open && x.total_open<y.total_open) need(x.mmosp()<y.mmosp(),"sum tie break");
    }
}
void decoder_tests() {
    auto instance=Instance::from_patterns(5,{{0,1},{1,2},{},{3},{0,2},{}});
    auto data=ItemOrderDecoder::Data::create(instance);
    ItemOrderDecoder decoder(data);
    Permutation p{0,1,2,3,4};
    do {
        Permutation expected{2,5};std::vector<bool> visited(5),used(6);used[2]=used[5]=true;
        for(int x:p){visited[static_cast<std::size_t>(x)]=true;for(std::size_t j=0;j<6;++j)
            if(!used[j] && std::find(instance->patterns()[j].begin(),instance->patterns()[j].end(),x)!=instance->patterns()[j].end() &&
               std::all_of(instance->patterns()[j].begin(),instance->patterns()[j].end(),[&](int y){return visited[static_cast<std::size_t>(y)];})){
                expected.push_back(static_cast<int>(j));used[j]=true;
            }}
        need(decoder.decode(p)==expected,"decoder completion order");
    }while(std::next_permutation(p.begin(),p.end()));
    throws([&]{decoder.decode({0,1,2,2,4});});throws([&]{decoder.decode({0,1});});
    auto zero=ItemOrderDecoder::Data::create(Instance::from_patterns(0,{{},{}}));
    ItemOrderDecoder zd(zero);need(zd.decode({})==Permutation({0,1}),"empty patterns decoder");
    auto wide=ItemOrderDecoder::Data::create(Instance::from_patterns(1031,{{0,1030},{64},{}}));
    ItemOrderDecoder wd(wide);Permutation order(1031);std::iota(order.begin(),order.end(),0);
    need(wd.decode(order)==Permutation({2,1,0}),"decoder beyond 1000 bits");
}
void chains() {
    std::mt19937 rng(1702);
    for(int m:{0,1,64,65,251,1031})for(int trial=0;trial<6;++trial){
        const int n=3+static_cast<int>(rng()%30);PatternItems patterns(static_cast<std::size_t>(n));
        const double density=trial==0?0.0:trial==1?1.0:trial*0.13;
        for(auto& pattern:patterns)for(int x=0;x<m;++x)
            if(static_cast<double>(rng())/rng.max()<density)pattern.push_back(x);
        auto instance=Instance::from_patterns(static_cast<std::size_t>(m),patterns);
        Permutation p(static_cast<std::size_t>(n));std::iota(p.begin(),p.end(),0);
        auto plan=mixed_plan(instance);
        AdaptiveEvaluator full(instance,p,Backend::Full),inc(instance,p,Backend::Incremental),automatic(instance,p,Backend::Auto,plan);
        std::uint64_t seen_full=0,seen_inc=0;
        for(int k=0;k<400;++k){
            const auto original=automatic.solution();
            Move move{static_cast<MoveKind>(k%3),static_cast<int>(rng()%n),static_cast<int>(rng()%n)};
            if(k%9==0)move=Move::swap(0,1);
            auto q=moved(original,move);const auto expected=oracle(*instance,q);
            need(full.evaluate(q)==expected && inc.evaluate(q)==expected && automatic.evaluate(q)==expected,"solution backend agreement");
            need(full.evaluate(move)==expected && inc.evaluate(move)==expected && automatic.evaluate(move)==expected,"move backend agreement");
            need(automatic.solution()==original && inc.solution()==original && full.solution()==original,"proposal purity");
            if(move.from!=move.to){
                if(automatic.last_backend()==Backend::Full)++seen_full;else++seen_inc;
            }
            if(rng()%2){
                if(k%2){need(full.apply(q)==expected && inc.apply(q)==expected && automatic.apply(q)==expected,"apply solution");}
                else need(full.apply(move)==expected && inc.apply(move)==expected && automatic.apply(move)==expected,"apply move");
            }
            need(full.metrics()==oracle(*instance,full.solution()) && full.solution()==inc.solution() && full.solution()==automatic.solution(),"synchronized states");
            need(full.metrics()==inc.metrics() && inc.metrics()==automatic.metrics(),"synchronized metrics");
            if(k%31==0){
                auto exchanged=automatic.solution();std::shuffle(exchanged.begin(),exchanged.end(),rng);
                const auto e=oracle(*instance,exchanged);
                need(full.apply(exchanged)==e && inc.apply(exchanged)==e && automatic.apply(exchanged)==e,"exchange synchronization");
            }
            ++scenarios;
        }
        need(seen_full>0 && seen_inc>0,"mixed branch coverage");
        const auto current=automatic.solution();
        throws([&]{automatic.evaluate(Permutation(current.size(),0));});
        need(automatic.solution()==current,"invalid proposal preserves state");
    }
}
void calibration_tests(){
    auto ten=Instance::from_patterns(1,PatternItems(10));
    std::vector<Decision> interpolated;
    for(auto kind:{InputKind::Solution,InputKind::Insert,InputKind::Swap,InputKind::Reverse}){
        interpolated.push_back({kind,2,Backend::Incremental,100,20});
        interpolated.push_back({kind,10,Backend::Full,100,140});
    }
    auto interpolation=SelectionPlan::from_decisions(ten,interpolated);
    need(interpolation->choose(InputKind::Insert,5)==Backend::Incremental,"intermediate interval interpolation");
    need(interpolation->choose(InputKind::Insert,9)==Backend::Full,"interpolated crossover");
    auto instance=Instance::from_patterns(65,{{0,1},{0,64},{1,2},{2,64},{3},{0,3}});
    Permutation p{0,1,2,3,4,5};
    auto plan=SelectionPlan::calibrate(instance,p,{3,3,1701});
    need(plan->density()==instance->density() && !plan->decisions().empty(),"calibration metadata");
    auto restored=SelectionPlan::from_decisions(instance,plan->decisions());
    for(const auto& row:plan->decisions()) need(restored->choose(row.input,row.max_interval)==row.backend,"restore choices");
    for(auto kind:{InputKind::Solution,InputKind::Insert,InputKind::Swap,InputKind::Reverse})
        for(std::size_t length=0;length<=p.size();++length)
            need(restored->choose(kind,length)==plan->choose(kind,length),"restore interpolated plan");
    AdaptiveEvaluator state(instance,p,Backend::Auto,restored);need(state.evaluate(Move::reverse(0,5))==oracle(*instance,{5,4,3,2,1,0}),"calibrated evaluation");
    throws([&]{SelectionPlan::calibrate(instance,p,{0,3,1});});
    throws([&]{SelectionPlan::from_decisions(instance,{});});
    auto other=Instance::from_patterns(65,instance->patterns());
    throws([&]{AdaptiveEvaluator bad(other,p,Backend::Auto,restored);});
    for(int n:{0,1,5}){
        auto empty=Instance::from_patterns(0,PatternItems(static_cast<std::size_t>(n)));
        Permutation q(static_cast<std::size_t>(n));std::iota(q.begin(),q.end(),0);
        AdaptiveEvaluator e(empty,q);need(e.evaluate(q)==Metrics{0,0,q.size()},"trivial auto");
    }
    std::vector<std::future<bool>> futures;
    for(int worker=0;worker<8;++worker)futures.push_back(std::async(std::launch::async,[=]{
        AdaptiveEvaluator e(instance,p,Backend::Auto,plan);
        for(int i=0;i<300;++i){auto move=Move::insert(i%6,(i*3)%6);const auto q=moved(e.solution(),move);
            if(e.evaluate(move)!=oracle(*instance,q))return false;
            e.apply(move);}
        return true;
    }));
    for(auto& f:futures)need(f.get(),"shared-plan concurrency");
}
}
int main(){try{score_tests();decoder_tests();chains();calibration_tests();
    std::cout<<"{\"status\":\"PASS\",\"adaptive_chain_scenarios\":"<<scenarios
             <<",\"shared_plan_parallel_scenarios\":2400,\"checks\":"<<checks<<"}\n";
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
