//============= Performance Checker for PMDK using applications ================//
// 
// Looks for semantics that may detrimant performance of a system using
// persistant memory programming.
//
//===============================================================================//

#ifndef PMDK_PERF_CHECKER_H_
#define PMDK_PERF_CHECKER_H_

#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/DebugInfoMetadata.h"

#include <vector>

#include "InstsSet.h"

namespace llvm {

// This class holds the redundant flushes that are statically detected
class RedFlushesRecord {
	std::vector<std::pair<Instruction *, Instruction *>> RedFlushesPairVect;
	
public:
	RedFlushesRecord() = default;
	
	void addPair(Instruction *Flush1, Instruction *Flush2) {
		auto Pair = std::make_pair(Flush1, Flush2);
		RedFlushesPairVect.push_back(Pair);
	}
	
	bool isEmpty() const {
		return RedFlushesPairVect.size() == 0;
	}
	
	void clear() {
		RedFlushesPairVect.clear();
	}
	
	void printRecord() const;
};

void RedFlushesRecord::printRecord() const {
// Lambda function to get the line number. We use debug information to do so.
	auto GetLineNumber = [](Instruction &I) {
		if(MDNode *N = I.getMetadata("dbg")) {
			if(DILocation *Loc = dyn_cast<DILocation>(N))
				return ConstantInt::get(Type::getInt32Ty(I.getContext()), Loc->getLine());
		}
		return (ConstantInt *)nullptr;
	};
	
// Print flushes
	if(!RedFlushesPairVect.size()) {
		errs() << "------------- PRINTING REDUNDANT FLUSH RECORD EMPTY ----------------\n";
		return;
	}
	errs() << "--------------------- PRINTING REDUNDANT FLUSH RECORD ------------------\n";
	for(auto &Pair : RedFlushesPairVect) {
		//errs() << "Flushes on lines " << GetLineNumber(*std::get<0>(Pair)) 
			//   << " and " << GetLineNumber(*std::get<1>(Pair)) << " are redundant\n";
		errs() << "++++++++ PAIR +++++++++\n";
		errs() << "PARENT: ";
		std::get<0>(Pair)->getParent()->printAsOperand(errs(), false);
		errs() << " ";
		std::get<0>(Pair)->print(errs());
		errs() << "\n";
		errs() << "PARENT: ";
		std::get<1>(Pair)->getParent()->printAsOperand(errs(), false);
		errs() << " ";
		std::get<1>(Pair)->print(errs());
		errs() << "\n";
	}
}

//--------------------------------------------------------------------------------------//
// Wrapper pass for the getting flush sets
//--------------------------------------------------------------------------------------//

void initializeRedFlushesCheckerWrapperPassPass(PassRegistry &);

class RedFlushesCheckerWrapperPass : public FunctionPass {
	PerfCheckerInfo<> PCI;
	PMInterfaces<> PMI;
	std::vector<Instruction *> WriteVect;
	
public:
	static char ID;
	
	RedFlushesCheckerWrapperPass() : FunctionPass(ID) {
		//initializeRedFlushesCheckerWrapperPassPass(
			///		*PassRegistry::getPassRegistry());
		initializeGenCondBlockSetLoopInfoWrapperPassPass(
									*PassRegistry::getPassRegistry());
	}
	
	bool runOnFunction(Function &F) override;
	
	void getAnalysisUsage(AnalysisUsage &AU) const {
		errs() << "HERE\n";
		AU.addRequired<DominatorTreeWrapperPass>();
		errs() << "HERE\n";
		AU.addRequired<GenCondBlockSetLoopInfoWrapperPass>();
		AU.addRequired<CFLSteensAAWrapperPass>();
		AU.addRequired<CFLAndersAAWrapperPass>();
		AU.addRequired<SCEVAAWrapperPass>();
		AU.addRequired<GlobalsAAWrapperPass>();
		//AU.addRequired<ObjCARCAAWrapperPass>();
		AU.addRequired<TypeBasedAAWrapperPass>();
		AU.addRequired<ScopedNoAliasAAWrapperPass>();
		AU.addRequired<BasicAAWrapperPass>();
		AU.addRequired<AAResultsWrapperPass>();
		AU.setPreservesCFG();
		AU.setPreservesAll();
	}
	
	bool doInitialization(Module &M) {
		return false;
	}
	
	bool doFinalization(Module &M) {
		return false;
	}
};

}  // namespace llvm

#endif  // PMDK_PERF_CHECKER_H_





