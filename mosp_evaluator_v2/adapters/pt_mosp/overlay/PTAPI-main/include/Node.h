#ifndef __NODE_H__
#define __NODE_H__

#include <vector>
#include <memory>
#include <cmath> 
#include <mutex>
#include <atomic>
#include <algorithm>
#include <iostream>
#include "Problem.h"

using namespace std;

std::mutex cout_mutex;

class Node{
	//private:
		
	protected:
		atomic<int>* execMax;
		atomic<int>* passoGatilho;
		mutex mtxNode;
		int execAtual = 0; 
		bool endN = false; 
		atomic<int>* indexPT;
		vector<std::pair<Node*,bool>> edgeFrom;
		vector<std::pair<Node*,bool>> edgeto;
		string nodeName;
	public:
		Node();
		virtual ~Node();
		virtual void run() = 0;
		virtual bool ready() = 0;
		virtual bool notify() = 0;
		virtual bool observer(Node* from) = 0;
		bool addEdge(Node* from, Node* to);
		bool theEnd();	
		bool theEnd(int ptlToBestSol);	
		bool finish();
		void reset();
		void printEdgeto();
		void printEdgeFrom();

		void lockPrint(string str){
			cout_mutex.lock();
			cout << str << endl;
			cout_mutex.unlock();
		}
};

Node::Node(){
}

Node::~Node(){
}

bool Node::addEdge(Node* from, Node* to){
 if (from == this){
	edgeFrom.push_back(std::make_pair(to,true));
	return true;
 } else if(to == this){
	edgeto.push_back(std::make_pair(from,false));
	return true; 
 }else{
	std::cout<<"This edge isn't related with this node!!!!"<<std::endl;
	return false;
 }
}

void Node::reset(){
	for(vector<std::pair<Node*,bool>>::iterator it = edgeto.begin(); it != edgeto.end(); it++){
		it->second = false;		
	}
}

bool Node::theEnd(){
	execAtual++;
	
	#ifdef DEBUG
	lockPrint("theEnd = execAtual: " + to_string(execAtual) + " execMax: " + to_string(*execMax) + " node: " + nodeName);
	#endif
	
	return (*execMax <= execAtual);
}

bool Node::theEnd(int ptlToBestSol){
	execAtual++;
	
	#ifdef DEBUG
	lockPrint("theEnd MCMC = execAtual: " + to_string(execAtual) + " execMax: " + to_string(*execMax) + " ptlToBestSol: " + to_string(ptlToBestSol) + " node: " + nodeName);
	#endif

	#ifdef GATILHO
	//if the ptlToBestSol is greater or iqual to 90% of the execMax, then increase the execMax in 10%
	if(ptlToBestSol >= 0.9*(*execMax)){
		#ifdef DEBUG
		lockPrint("Aumentou");
		#endif
		*execMax = *execMax + *passoGatilho;
		// *execMax = *execMax + 0.1*(*execMax);
	}
	#endif
	
	return (*execMax <= execAtual);
}

bool Node::finish(){
	return (*execMax <= execAtual);
}

void Node::printEdgeto(){
	cout<<"Node:"<<this<<endl;
	
	for(vector<std::pair<Node*,bool>>::iterator it = edgeto.begin(); it != edgeto.end(); it++){
		cout<<"Pointer:"<<it->first<<" status:"<<it->second<<endl;
	}
}

void Node::printEdgeFrom(){
	cout<<"Node:"<<this<<endl;
	
	for(vector<std::pair<Node*,bool>>::iterator it = edgeFrom.begin(); it != edgeFrom.end(); it++){
		cout<<"Pointer:"<<it->first<<" status:"<<it->second<<endl;
	}
}

#endif
