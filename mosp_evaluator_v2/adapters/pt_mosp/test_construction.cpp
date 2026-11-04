#include "MOSP.h"
#include <iostream>
int main(int argc,char** argv){
    if(argc!=3)return 2;
    for(int sequence:{0,1}){
        MOSP rows(argv[1],1,2,sequence,mosp::Backend::Full,mosp::Energy::MMOSP,true);
        MOSP columns(argv[2],0,2,sequence,mosp::Backend::Full,mosp::Energy::MMOSP,true);
        if(rows.getPatternPieces()!=columns.getPatternPieces())return 3;
        const auto a=rows.construction(),b=columns.construction();
        if(a.sol!=b.sol||a.evalSol!=b.evalSol||a.mospValue!=b.mospValue||a.totalOpen!=b.totalOpen)return 4;
    }
    std::cout<<"{\"warm_start_layout_consistency\":\"PASS\"}\n";
}
