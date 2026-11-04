#include "PT.h"
#include <type_traits>
#include <stdexcept>
struct SmallSolution:solution{};
struct SmallProblem:Problem<SmallSolution>{
    int calls=0;
    SmallSolution construction()override{SmallSolution s;s.evalSol=calls++==0?4.75:4.8125;return s;}
    SmallSolution neighbor(SmallSolution s)override{return s;}
    double evaluate(SmallSolution& s)override{return s.evalSol;}
};
int main(){
    static_assert(std::is_same_v<decltype(std::declval<NodeMCMC<SmallSolution>>().getEnergia()),double>);
    Consumer<SmallSolution> pool(1);SmallProblem problem;std::atomic<int> limit{10},trigger{0};
    NodeMCMC<SmallSolution> left(1,&limit,&trigger,1.0,&problem,&pool,"left");
    NodeMCMC<SmallSolution> right(1,&limit,&trigger,0.1,&problem,&pool,"right");
    if(left.getEnergia()!=4.75||right.getEnergia()!=4.8125)return 1;
    NodeSwap<SmallSolution> swap(&left,&right,&pool,&limit,"swap");
    // Exercise the actual exchange routine; inverse temperatures give certain acceptance.
    const auto a=left.getSol(),b=right.getSol();swap.run();
    if(left.getEnergia()!=b.evalSol||right.getEnergia()!=a.evalSol)return 2;
    std::cout<<"{\"fractional_exchange_precision\":\"PASS\"}\n";
}
