// PT-MOSP adapter for MOSP Evaluator 2.0.0; generated with Codex, 2026-10-29.
// Original PT-MOSP authors retain ownership of their code.
#ifndef __MOSP_H__
#define __MOSP_H__
#include <atomic>
#include <bitset>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include "../PTAPI-main/include/Problem.h"
#include "mosp_evaluator/include/mosp/adaptive.hpp"

struct solMOSP : public solution {
    std::vector<int> sol;
    int maxNumberPiecesPerPatern = 0;
    int construcTime = 0;
    int mospValue = 0;
    std::int64_t totalOpen = 0;
    std::uint64_t evaluationTicket = 0;
    bool hasMove = false;
    mosp::Move move = mosp::Move::swap(0,0);
};

class MOSPEvaluationSession;
class MOSP : public Problem<solMOSP> {
private:
    int sequenceForm, numberPatterns, numberPieces;
    int maxNumberPiecesPerPatern = 0, neighborStrt, readForm_;
    std::atomic<int> constructSolutionTime{0}, constructionCallIndex{0};
    std::atomic<std::uint64_t> nextTicket{1};
    std::string fn;
    std::vector<int> stackSizeEvaluation;
    std::vector<std::vector<int>> patternPieces;
    std::vector<std::bitset<1000>> patternBitsets;
    mosp::Instance::Ptr instance_;
    mosp::ItemOrderDecoder::Data::Ptr decoderData_;
    mosp::SelectionPlan::Ptr plan_;
    mosp::Backend backend_;
    mosp::Energy energy_;
    bool audit_;
    std::atomic<std::uint64_t> fullEvaluations{0}, incrementalEvaluations{0};
    std::atomic<std::uint64_t> localApplications{0}, exchangeSynchronizations{0}, auditedEvaluations{0};
    friend class MOSPEvaluationSession;
public:
    MOSP(std::string filename, int readForm, int neighborStrategy, int sequenceForm,
         mosp::Backend backend = mosp::Backend::Auto,
         mosp::Energy energy = mosp::Energy::MOSP, bool audit = false);
    ~MOSP() override = default;
    solMOSP construction() override;
    solMOSP neighbor(solMOSP sol) override;
    solMOSP neighborSwap(solMOSP sol);
    solMOSP neighbor2Opt(solMOSP sol);
    solMOSP neighborInsertion(solMOSP sol);
    double evaluate(solMOSP& sol) override;
    std::unique_ptr<EvaluationSession<solMOSP>> makeEvaluationSession(const solMOSP& initial) override;
    std::vector<int> patternOrder(const solMOSP& sol) const;
    void writeMetadata(std::ostream& output, const solMOSP& sol, double seconds) const;
    int getNumberPieces() const { return numberPieces; }
    int getNumberPatterns() const { return numberPatterns; }
    const std::vector<int>& getStackSizeEvaluation() const { return stackSizeEvaluation; }
    const std::vector<std::vector<int>>& getPatternPieces() const { return patternPieces; }
    const std::vector<std::bitset<1000>>& getPatternBitsets() const;
};
#endif
