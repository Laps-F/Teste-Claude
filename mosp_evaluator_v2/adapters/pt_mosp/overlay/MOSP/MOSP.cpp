// PT-MOSP adapter for MOSP Evaluator 2.0.0; generated with Codex, 2026-10-29.
// Original PT-MOSP authors retain ownership of their code.
#include "MOSP.h"
#include "MCNH/HNCM.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <cstring>
using namespace std;

namespace {
void setMetrics(solMOSP& sol, mosp::Metrics metrics, mosp::Energy energy) {
    sol.mospValue=metrics.max_open;sol.totalOpen=metrics.total_open;
    sol.evalSol=metrics.energy(energy);
}
}

class MOSPEvaluationSession : public EvaluationSession<solMOSP> {
    MOSP& problem_;
    mosp::ItemOrderDecoder decoder_;
    mosp::AdaptiveEvaluator evaluator_;
    mosp::Evaluator auditEvaluator_;
    std::uint64_t pendingTicket_=0;
    mosp::Move pendingMove_=mosp::Move::swap(0,0);
    bool pendingHasMove_=false;
    std::vector<int> pendingPatterns_;
public:
    MOSPEvaluationSession(MOSP& problem,const solMOSP& initial)
        :problem_(problem),decoder_(problem.decoderData_),
         evaluator_(problem.instance_,problem.patternOrder(initial),problem.backend_,problem.plan_),
         auditEvaluator_(problem.instance_){}
    double evaluate(solMOSP& candidate) override {
        const auto oldCounts=evaluator_.counts();
        const bool byMove=!problem_.sequenceForm && candidate.hasMove;
        mosp::Metrics result;
        if(byMove) result=evaluator_.evaluate(candidate.move);
        else {
            pendingPatterns_=problem_.sequenceForm?decoder_.decode(candidate.sol):candidate.sol;
            result=evaluator_.evaluate(pendingPatterns_);
        }
        const auto counts=evaluator_.counts();
        problem_.fullEvaluations.fetch_add(counts.full-oldCounts.full,std::memory_order_relaxed);
        problem_.incrementalEvaluations.fetch_add(counts.incremental-oldCounts.incremental,std::memory_order_relaxed);
        pendingTicket_=candidate.evaluationTicket;pendingMove_=candidate.move;pendingHasMove_=byMove;
        if(problem_.audit_){
            const auto order=problem_.patternOrder(candidate);
            if(auditEvaluator_.evaluate_metrics(order)!=result)throw std::logic_error("PT-MOSP: candidate audit mismatch");
            problem_.auditedEvaluations.fetch_add(1,std::memory_order_relaxed);
        }
        candidate.construcTime=problem_.constructSolutionTime.load();
        setMetrics(candidate,result,problem_.energy_);
        return candidate.evalSol;
    }
    void accepted(const solMOSP& candidate) override {
        mosp::Metrics result;
        if(pendingTicket_ && pendingTicket_==candidate.evaluationTicket){
            result=pendingHasMove_?evaluator_.apply(pendingMove_):evaluator_.apply(pendingPatterns_);
            problem_.localApplications.fetch_add(1,std::memory_order_relaxed);
        }else{
            result=evaluator_.apply(problem_.patternOrder(candidate));
            problem_.exchangeSynchronizations.fetch_add(1,std::memory_order_relaxed);
        }
        pendingTicket_=0;
        if(result.max_open!=candidate.mospValue || result.total_open!=candidate.totalOpen ||
           result.energy(problem_.energy_)!=candidate.evalSol)
            throw std::logic_error("PT-MOSP: accepted energy/state mismatch");
    }
};

MOSP::MOSP(std::string filename,int readForm,int neighborStrategy,int inSequenceForm,
           mosp::Backend backend,mosp::Energy energy,bool audit)
    :sequenceForm(inSequenceForm),neighborStrt(neighborStrategy),readForm_(readForm),
     fn(std::move(filename)),backend_(backend),energy_(energy),audit_(audit){
    if((readForm!=0&&readForm!=1)||(sequenceForm!=0&&sequenceForm!=1)||neighborStrt<0||neighborStrt>2)
        throw std::invalid_argument("PT-MOSP: invalid read/sequence/movement option");
    mosp::backend_name(backend_);mosp::energy_name(energy_);
    std::ifstream file(fn);long long n=0,m=0;
    if(!(file>>n>>m)||n<0||m<0||n>INT_MAX||m>INT_MAX)
        throw std::invalid_argument("PT-MOSP: invalid file or dimensions");
    numberPatterns=static_cast<int>(n);numberPieces=static_cast<int>(m);
    if(m && static_cast<std::size_t>(n)>std::numeric_limits<std::size_t>::max()/static_cast<std::size_t>(m))
        throw std::length_error("PT-MOSP: matrix dimensions overflow");
    std::vector<int> entries(static_cast<std::size_t>(n)*static_cast<std::size_t>(m));
    for(auto& value:entries)if(!(file>>value))throw std::invalid_argument("PT-MOSP: incomplete matrix");
    std::string extra;if(file>>extra)throw std::invalid_argument("PT-MOSP: extra data after matrix");
    instance_=mosp::Instance::from_dense(readForm?static_cast<std::size_t>(n):static_cast<std::size_t>(m),
        readForm?static_cast<std::size_t>(m):static_cast<std::size_t>(n),entries,
        readForm?mosp::MatrixAxis::PatternsInRows:mosp::MatrixAxis::PatternsInColumns,mosp::Presence::EqualToOne);
    patternPieces=instance_->patterns();stackSizeEvaluation.assign(static_cast<std::size_t>(m),0);
    for(const auto& items:patternPieces){
        maxNumberPiecesPerPatern=std::max(maxNumberPiecesPerPatern,static_cast<int>(items.size()));
        for(int x:items)++stackSizeEvaluation[static_cast<std::size_t>(x)];
    }
    if(numberPieces<=1000){
        patternBitsets.resize(static_cast<std::size_t>(n));
        for(std::size_t j=0;j<patternPieces.size();++j)for(int x:patternPieces[j])patternBitsets[j].set(static_cast<std::size_t>(x));
    }
    decoderData_=mosp::ItemOrderDecoder::Data::create(instance_);
    if(backend_==mosp::Backend::Auto){
        mosp::Permutation initial(static_cast<std::size_t>(n));std::iota(initial.begin(),initial.end(),0);
        plan_=mosp::SelectionPlan::calibrate(instance_,initial);
    }
}

const std::vector<std::bitset<1000>>& MOSP::getPatternBitsets() const {
    if(numberPieces>1000)throw std::length_error("PT-MOSP: legacy bitset getter supports at most 1000 items; evaluator/decoder are dynamic");
    return patternBitsets;
}
std::vector<int> MOSP::patternOrder(const solMOSP& sol) const {
    if(!sequenceForm)return sol.sol;
    mosp::ItemOrderDecoder decoder(decoderData_);return decoder.decode(sol.sol);
}
std::unique_ptr<EvaluationSession<solMOSP>> MOSP::makeEvaluationSession(const solMOSP& initial){
    return std::unique_ptr<EvaluationSession<solMOSP>>(new MOSPEvaluationSession(*this,initial));
}
double MOSP::evaluate(solMOSP& sol){
    mosp::Evaluator evaluator(instance_);setMetrics(sol,evaluator.evaluate_metrics(patternOrder(sol)),energy_);
    sol.construcTime=constructSolutionTime.load();return sol.evalSol;
}

solMOSP MOSP::construction(){
    const int idx=constructionCallIndex.fetch_add(1);
    solMOSP ss;
    if((idx==0||idx==2)&&numberPatterns>0&&numberPieces>0){
        int* raw=nullptr;int result=0,size=0;double seconds=0;
        std::vector<char> filename(fn.begin(),fn.end());filename.push_back('\0');
        mainMethod(&result,&seconds,filename.data(),&raw,&size,sequenceForm,readForm_);
        std::unique_ptr<int[]> holder(raw);
        if(sequenceForm)ss.sol.assign(raw,raw+size);
        else {
            const int nonempty=static_cast<int>(std::count_if(patternPieces.begin(),patternPieces.end(),[](const auto& p){return !p.empty();}));
            if(size!=numberPatterns)throw std::logic_error("MCNH: incomplete pattern solution");
            ss.sol.assign(raw,raw+nonempty);
            for(int j=0;j<numberPatterns;++j)if(patternPieces[static_cast<std::size_t>(j)].empty())ss.sol.push_back(j);
        }
        constructSolutionTime.fetch_add(static_cast<int>(std::min(seconds*1000.0,static_cast<double>(INT_MAX))));
    }else{
        ss.sol.resize(static_cast<std::size_t>(sequenceForm?numberPieces:numberPatterns));
        std::iota(ss.sol.begin(),ss.sol.end(),0);
        std::mt19937 generator(std::random_device{}());std::shuffle(ss.sol.begin(),ss.sol.end(),generator);
    }
    ss.evaluationTicket=nextTicket.fetch_add(1);ss.maxNumberPiecesPerPatern=maxNumberPiecesPerPatern;
    ss.evalSol=evaluate(ss);ss.Nup=false;ss.Ndown=false;
    return ss;
}

solMOSP MOSP::neighbor(solMOSP sol){
    solMOSP result;
    if(sol.sol.size()<2)result=sol;
    else if(neighborStrt==0)result=neighborSwap(sol);
    else if(neighborStrt==1)result=neighbor2Opt(sol);
    else result=neighborInsertion(sol);
    result.evaluationTicket=nextTicket.fetch_add(1);
    if(sol.sol.size()<2)result.hasMove=false;
    return result;
}

void MOSP::writeMetadata(std::ostream& out,const solMOSP& sol,double seconds) const {
    const auto order=patternOrder(sol);
    mosp::Evaluator verifier(instance_);const auto metrics=verifier.evaluate_metrics(order);
    if(metrics.max_open!=sol.mospValue||metrics.total_open!=sol.totalOpen||metrics.energy(energy_)!=sol.evalSol)
        throw std::logic_error("PT-MOSP: final solution verification failed");
    out<<std::setprecision(17)<<"{\"library_version\":\"2.0.0\",\"mosp\":"<<metrics.max_open
       <<",\"total_open\":"<<metrics.total_open<<",\"mmosp\":"<<metrics.mmosp()
       <<",\"energy\":"<<sol.evalSol<<",\"energy_mode\":\""<<mosp::energy_name(energy_)
       <<"\",\"evaluator\":\""<<mosp::backend_name(backend_)<<"\",\"sequence\":\""<<(sequenceForm?"items":"patterns")
       <<"\",\"density\":"<<instance_->density()<<",\"wall_seconds\":"<<seconds
       <<",\"calibration_seconds\":"<<(plan_?plan_->calibration_seconds():0)
       <<",\"full_evaluations\":"<<fullEvaluations.load()<<",\"incremental_evaluations\":"<<incrementalEvaluations.load()
       <<",\"local_applications\":"<<localApplications.load()<<",\"exchange_synchronizations\":"<<exchangeSynchronizations.load()
       <<",\"audited_evaluations\":"<<auditedEvaluations.load()<<",\"solution\":[";
    for(std::size_t i=0;i<sol.sol.size();++i)out<<(i?",":"")<<sol.sol[i];
    out<<"],\"pattern_sequence\":[";for(std::size_t i=0;i<order.size();++i)out<<(i?",":"")<<order[i];
    out<<"],\"selection\":[";
    if(plan_){bool first=true;for(const auto& d:plan_->decisions()){
        if(!first)out<<',';
        first=false;out<<"{\"input\":"<<static_cast<int>(d.input)<<",\"max_interval\":"<<d.max_interval
          <<",\"backend\":\""<<mosp::backend_name(d.backend)<<"\",\"full_ns\":"<<d.full_ns<<",\"incremental_ns\":"<<d.incremental_ns<<'}';
    }}
    out<<"]}\n";
}

// 2-opt neighbor moviment
solMOSP MOSP::neighbor2Opt(solMOSP sol){
	solMOSP s;
    s.sol = sol.sol; 
    s.maxNumberPiecesPerPatern = sol.maxNumberPiecesPerPatern;

    std::random_device rnd_device;
    std::mt19937 mersenne_engine {rnd_device()};
    if(sequenceForm){
        std::uniform_int_distribution<int> distPattern(0, numberPieces - 1);
    
        int piece1 = 0, piece2 = 0;
        
        do {
            piece1 = distPattern(mersenne_engine);
            piece2 = distPattern(mersenne_engine);
        } while (piece1 == piece2); 
        
        if(piece1 > piece2) std::swap(piece1, piece2);
    
        while(piece1 < piece2) {
            std::swap(s.sol[piece1], s.sol[piece2]);
            piece1++;
            piece2--;
        }
    }else {
        std::uniform_int_distribution<int> distPattern(0, numberPatterns - 1);
    
        int pattern1 = 0, pattern2 = 0;
        
        do {
            pattern1 = distPattern(mersenne_engine);
            pattern2 = distPattern(mersenne_engine);
        } while (pattern1 == pattern2); 
        
        if(pattern1 > pattern2) std::swap(pattern1, pattern2);
        s.hasMove=true;s.move=mosp::Move::reverse(pattern1,pattern2);
    
        while(pattern1 < pattern2) {
            std::swap(s.sol[pattern1], s.sol[pattern2]);
            pattern1++;
            pattern2--;
        }

    }

    s.Nup = sol.Nup;
    s.Ndown = sol.Ndown;

    return s;
}

// swap neighbor moviment
solMOSP MOSP::neighborSwap(solMOSP sol) {
    solMOSP s;
    s.sol = sol.sol;
    s.maxNumberPiecesPerPatern = sol.maxNumberPiecesPerPatern;

    std::random_device rnd_device;
    std::mt19937 mersenne_engine {rnd_device()};

    if(sequenceForm){
        std::uniform_int_distribution<int> distPattern(0, numberPieces - 1);
    
        int piece1 = 0, piece2 = 0;
    
        do {
            piece1 = distPattern(mersenne_engine);
            piece2 = distPattern(mersenne_engine);
        } while (piece1 == piece2);
    
        std::swap(s.sol[piece1], s.sol[piece2]);
    }else {
        std::uniform_int_distribution<int> distPattern(0, numberPatterns - 1);
    
        int pattern1 = 0, pattern2 = 0;
    
        do {
            pattern1 = distPattern(mersenne_engine);
            pattern2 = distPattern(mersenne_engine);
        } while (pattern1 == pattern2);
    
        std::swap(s.sol[pattern1], s.sol[pattern2]);
        s.hasMove=true;s.move=mosp::Move::swap(pattern1,pattern2);
    }

    s.Nup = sol.Nup;
    s.Ndown = sol.Ndown;

    return s;
}

// random insertion neighbor movement
solMOSP MOSP::neighborInsertion(solMOSP sol) {
    solMOSP s;
    s.sol = sol.sol;
    s.maxNumberPiecesPerPatern = sol.maxNumberPiecesPerPatern;

    std::random_device rnd_device;
    std::mt19937 mersenne_engine {rnd_device()};
    
    if(sequenceForm){
        std::uniform_int_distribution<int> distPattern(0, numberPieces - 1);
    
        int pieceFrom = 0, pieceTo = 0;
    
        do {
            pieceFrom = distPattern(mersenne_engine);
            pieceTo = distPattern(mersenne_engine);
        } while (pieceFrom == pieceTo);
    
        int element = s.sol[pieceFrom];
        s.sol.erase(s.sol.begin() + pieceFrom);
    
        if (pieceTo > pieceFrom) {
            pieceTo--;  // ajuste porque o vetor ficou menor após remoção
        }
    
        s.sol.insert(s.sol.begin() + pieceTo, element);
    } else {
        std::uniform_int_distribution<int> distPattern(0, numberPatterns - 1);
    
        int patternFrom = 0, patternTo = 0;
    
        do {
            patternFrom = distPattern(mersenne_engine);
            patternTo = distPattern(mersenne_engine);
        } while (patternFrom == patternTo);
    
        int element = s.sol[patternFrom];
        s.sol.erase(s.sol.begin() + patternFrom);
    
        if (patternTo > patternFrom) {
            patternTo--;  // ajuste porque o vetor ficou menor após remoção
        }
    
        s.sol.insert(s.sol.begin() + patternTo, element);
        s.hasMove=true;s.move=mosp::Move::insert(patternFrom,patternTo);
    }

    s.Nup = sol.Nup;
    s.Ndown = sol.Ndown;

    return s;
}

