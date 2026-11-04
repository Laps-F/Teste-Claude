// #ifndef __PROBLEM_H__
// #define __PROBLEM_H__

// #include <vector>
// #include <climits>
#include <limits>
#include <memory>


// struct solution{ 
//   double evalSol = std::numeric_limits<double>::infinity(); 
//   bool Nup = false, Ndown = false;
// };

// template<typename S>
// class Problem{
// 	private:
// 		solution bestSol;
// 	public:
// 		Problem();
// 		virtual ~Problem();
// 		void setBestSol(S sol);
// 		virtual S construction()=0;
// 		virtual S neighbor(S sol)=0;
// 		virtual double evaluate(S sol)=0;
// };

// template<typename S>
// Problem<S>::Problem(){
// }

// template<typename S>
// Problem<S>::~Problem(){
// }

// template<typename S>
// void Problem<S>::setBestSol(S sol){
// }


// #endif

#ifndef __PROBLEM_H__
#define __PROBLEM_H__

#include <vector>
#include <climits>
#include <limits>
#include <memory>


struct solution{ 
  double evalSol = std::numeric_limits<double>::infinity(); 
  int ptl = -1;
  int mcmc = -1;
  bool Nup = false, Ndown = false;
};

template<typename S>
class EvaluationSession {
public:
    virtual ~EvaluationSession() = default;
    virtual double evaluate(S& candidate) = 0;
    virtual void accepted(const S& candidate) = 0;
};

template<typename S>
class Problem{
	private:
		solution bestSol;
	public:
		Problem();
		virtual ~Problem();
		void setBestSol(S sol);
		virtual S construction()=0;
		virtual S neighbor(S sol)=0;
		virtual double evaluate(S& sol)=0;
        virtual std::unique_ptr<EvaluationSession<S>> makeEvaluationSession(const S&) { return nullptr; }
};

template<typename S>
Problem<S>::Problem(){
}

template<typename S>
Problem<S>::~Problem(){
}

template<typename S>
void Problem<S>::setBestSol(S sol){
}


#endif
