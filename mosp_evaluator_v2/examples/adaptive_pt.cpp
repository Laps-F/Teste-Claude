// A complete API example with identical trajectories across evaluator backends.
#include "mosp/adaptive.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>

struct Trace {
    std::uint64_t hash=0;
    int accepted=0,rejected=0,worsening=0,exchanges=0;
    bool operator==(const Trace& other)const{return hash==other.hash&&accepted==other.accepted&&rejected==other.rejected&&worsening==other.worsening&&exchanges==other.exchanges;}
};
Trace run(mosp::Instance::Ptr instance,mosp::SelectionPlan::Ptr plan,mosp::Backend backend,mosp::Energy energy){
    std::mt19937 rng(5901);
    std::vector<double> temperatures{0.01,0.1,1.0,10.0};
    std::vector<mosp::AdaptiveEvaluator> replicas;
    mosp::Evaluator verifier(instance);
    mosp::Permutation initial(instance->pattern_count());std::iota(initial.begin(),initial.end(),0);
    for(std::size_t r=0;r<temperatures.size();++r){std::shuffle(initial.begin(),initial.end(),rng);replicas.emplace_back(instance,initial,backend,plan);}
    auto uniform=[&]{return std::generate_canonical<double,53>(rng);};
    Trace trace;
    for(int iteration=0;iteration<3000;++iteration){
        for(std::size_t r=0;r<replicas.size();++r){
            auto& state=replicas[r];const int n=static_cast<int>(state.solution().size());
            auto move=mosp::Move{static_cast<mosp::MoveKind>(rng()%3),static_cast<int>(rng()%n),static_cast<int>(rng()%n)};
            const auto candidate=state.evaluate(move);const double delta=candidate.energy(energy)-state.metrics().energy(energy);
            if(delta<=0||uniform()<std::exp(-delta/temperatures[r])){
                if(state.apply(move)!=candidate)throw std::logic_error("accepted metrics differ");
                ++trace.accepted;if(delta>0)++trace.worsening;
            }else ++trace.rejected;
            if(verifier.evaluate_metrics(state.solution())!=state.metrics())throw std::logic_error("replica verification");
            trace.hash=trace.hash*UINT64_C(1099511628211)+static_cast<std::uint64_t>(state.metrics().total_open);
            for(int id:state.solution())trace.hash=trace.hash*31+static_cast<unsigned>(id);
        }
        if(iteration%10==0)for(std::size_t r=0;r+1<replicas.size();++r){
            const double log_ratio=(1.0/temperatures[r]-1.0/temperatures[r+1])*
                (replicas[r].metrics().energy(energy)-replicas[r+1].metrics().energy(energy));
            if(log_ratio>=0||std::log(uniform())<log_ratio){std::swap(replicas[r],replicas[r+1]);++trace.exchanges;}
        }
    }
    if(!trace.accepted||!trace.rejected||!trace.worsening||!trace.exchanges)throw std::logic_error("incomplete event coverage");
    return trace;
}
int main(){try{
    auto instance=mosp::Instance::from_patterns(10,{{0,1},{1,2},{0,2},{3,4},{4,5},{3,5},{6},{6,7},{7,8},{8,9},{0,9},{2,7}});
    mosp::Permutation identity(12);std::iota(identity.begin(),identity.end(),0);
    auto plan=mosp::SelectionPlan::calibrate(instance,identity);
    for(auto energy:{mosp::Energy::MOSP,mosp::Energy::MMOSP}){
        auto baseline=run(instance,plan,mosp::Backend::Full,energy);
        for(auto backend:{mosp::Backend::Full,mosp::Backend::Incremental,mosp::Backend::Auto}){
            const auto trace=backend==mosp::Backend::Full?baseline:run(instance,plan,backend,energy);
            if(!(trace==baseline))throw std::logic_error("backend changed search trajectory");
            std::cout<<"{\"status\":\"PASS\",\"energy\":\""<<mosp::energy_name(energy)<<"\",\"backend\":\""<<mosp::backend_name(backend)
                <<"\",\"proposals\":12000,\"accepted\":"<<trace.accepted<<",\"rejected\":"<<trace.rejected
                <<",\"worsening_accepted\":"<<trace.worsening<<",\"exchanges\":"<<trace.exchanges<<",\"trajectory_hash\":"<<trace.hash<<"}\n";
        }
    }
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
