// PT-MOSP evaluator integration, generated with Codex, 2026-10-29.
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include "MOSP.h"
#include "../PTAPI-main/include/PT.h"
#include "results.h"

int main(int argc,char** argv){
    try{
        if(argc<2 || std::string(argv[1])=="--help"){
            std::cout<<"Usage: mainMOSP_eval INSTANCE [--EVALUATOR full|incremental|auto] [--ENERGY mosp|mmosp]\n"
                     <<"  --READ 1: patterns in rows; --READ 0: patterns in columns. Header is always patterns items.\n"
                     <<"  --SEQUENCE 1: item order; --SEQUENCE 0: pattern order. --AUDIT 1 enables independent checking.\n"
                     <<"  Remaining PT options keep their original names.\n";
            return argc<2?2:0;
        }
        std::map<std::string,std::string> options;
        const std::vector<std::string> known={"--TEMP_INIT","--TEMP_FIM","--N_REPLICAS","--MCL","--PTL",
            "--TEMP_DIST","--MOV_TYPE","--TYPE_UPDATE","--TEMP_UPDATE","--THREAD_USED","--SEQUENCE",
            "--INST","--OUTDIR","--READ","--EVALUATOR","--ENERGY","--AUDIT"};
        for(int i=2;i<argc;i+=2){
            const std::string key=argv[i];
            if(i+1>=argc || std::find(known.begin(),known.end(),key)==known.end() || !options.emplace(key,argv[i+1]).second)
                throw std::invalid_argument("invalid/duplicate option or missing value: "+key);
        }
        auto get=[&](const char* key,const char* fallback){auto it=options.find(key);return it==options.end()?std::string(fallback):it->second;};
        auto integer=[&](const char* key,const char* fallback){const auto s=get(key,fallback);std::size_t used=0;int v=std::stoi(s,&used);if(used!=s.size())throw std::invalid_argument(key);return v;};
        auto real=[&](const char* key,const char* fallback){const auto s=get(key,fallback);std::size_t used=0;double v=std::stod(s,&used);if(used!=s.size()||!std::isfinite(v))throw std::invalid_argument(key);return v;};
        const auto backend=mosp::parse_backend(get("--EVALUATOR","auto"));
        const auto energy=mosp::parse_energy(get("--ENERGY","mosp"));
        const int replicas=integer("--N_REPLICAS","11"),mcl=integer("--MCL","600"),ptl=integer("--PTL","2000");
        const int threads=integer("--THREAD_USED","12"),update=integer("--TYPE_UPDATE","3");
        const int dist=integer("--TEMP_DIST","2"),move=integer("--MOV_TYPE","2");
        const int sequence=integer("--SEQUENCE","1"),read=integer("--READ","1"),exec=integer("--INST","0");
        const int proportion=integer("--TEMP_UPDATE","5"),audit=integer("--AUDIT","0");
        const double low=real("--TEMP_INIT","0.01"),high=real("--TEMP_FIM","10");
        if(replicas<2||mcl<1||ptl<1||threads<1||threads>1024||proportion<1||update<0||update>3||dist<1||dist>4||
           low<=0||high<=low||low<std::numeric_limits<float>::min()||high>std::numeric_limits<float>::max()||
           (audit!=0&&audit!=1)||(update==3&&replicas<3))throw std::invalid_argument("invalid PT parameter range");
        const auto start=std::chrono::steady_clock::now();
        MOSP problem(argv[1],read,move,sequence,backend,energy,audit!=0);
        PT<solMOSP> algorithm(static_cast<float>(low),static_cast<float>(high),replicas,mcl,ptl,0,dist,update,std::max(1,ptl/proportion));
        const auto result=algorithm.start(threads,&problem);
        const auto& sol=result.best;
        const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        const std::string directory=get("--OUTDIR","resultados")+"/PT/"+mosp::energy_name(energy)+"_"+mosp::backend_name(backend);
        std::filesystem::create_directories(directory);
        SolResult legacy;legacy.evalSol=sol.mospValue;legacy.maxNumberPiecesPerPatern=sol.maxNumberPiecesPerPatern;
        legacy.sol=sol.sol;legacy.construcTime=sol.construcTime;
        const int elapsed=static_cast<int>(std::min(seconds*1000.0,static_cast<double>(INT_MAX)));
        saveResults(argv[1],legacy,elapsed,sol.ptl,directory,exec,replicas);
        const std::string base=std::filesystem::path(argv[1]).stem().string();
        std::ofstream metadata(directory+"/"+base+"("+std::to_string(exec)+")_res.json");
        if(!metadata)throw std::runtime_error("cannot write PT-MOSP metadata");
        problem.writeMetadata(metadata,sol,seconds);
        problem.writeMetadata(std::cout,sol,seconds);
    }catch(const std::exception& e){std::cerr<<"PT-MOSP: "<<e.what()<<'\n';return 2;}
}
