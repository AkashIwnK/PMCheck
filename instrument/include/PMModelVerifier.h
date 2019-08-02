
#ifndef PM_MODEL_VERIFIER_H_
#define PM_MODEL_VERIFIER_H_

#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/Dominators.h"

#include <vector>

#include "InstsSet.h"

namespace llvm {

// This class maintains temporary record of peristency operations that can be
// analyzed at compile-time.
template<typename T = Instruction>
class TempPersistencyRecord {
	typedef std::pair<SerialInstsSet<T>, SerialInstsSet<T>> InstVectPairTy;

// Vector of temporary writes and flushes
	std::vector<InstVectPairTy> PairVect;

// Vector of redundant flushes
	std::vector<SerialInstsSet<T>> RedFencesVectVect;

public:
	void addWritesAndFlushes(SerialInstsSet<T> &SW, SerialInstsSet<T> &SF) {
		auto Pair = std::make_pair(SW, SF);
		PairVect.push_back(Pair);
	}

	void addRedFences(SerialInstsSet<T> &SFC) {
		RedFencesVectVect.push_back(SFC);
	}

	void clear() {
		PairVect.clear();
		RedFencesVectVect.clear();
	}

	void printRecord() const;
};

template<typename T>
void TempPersistencyRecord<T>::printRecord() const {
// Lambda function to get the line number. We use debug information to do so.
	auto GetLineNumber = [](T &I) {
		if(MDNode *N = I.getMetadata("dbg")) {
			if(DILocation *Loc = dyn_cast<DILocation>(N))
				return ConstantInt::get(Type::getInt32Ty(I.getContext()), Loc->getLine());
		}
		return (ConstantInt *)nullptr;
	};

// Make sure that the stores and flushes pair exist
	errs() << "+++++++++++++++++ PRINTING REDUNDANT PERSIST OPERATIONS ++++++++++++++++\n";
	for(auto &Pair : PairVect) {
		auto &SW  = std::get<0>(Pair);
		auto &SF  = std::get<1>(Pair);
		if(!SW.size()) {
		// The flushes have no writes to go with them
			for(auto *I : SF) {
				errs() << "Flushes at line " << GetLineNumber(*I) << " ";
				I->print(errs());
				errs() << "does not have a store to go with it.\n";
			}
			continue;
		}
		if(!SF.size()) {
		// The writes have no flushes to go with them
			for(auto *I : SW) {
				errs() << "Write at line " << GetLineNumber(*I) << " ";
				I->print(errs());
				errs() << "does not have a flush to go with it.\n";
			}
		}
		continue;
	}

// Print the redundant flushes
	errs() << "--------------------- PRINTING REDUNDANT FENCES RECORD ------------------\n";
	for(auto &SF : RedFencesVectVect) {
		errs() << "Fence at: ";
		for(auto *I : SF) {
			errs() << "line " << GetLineNumber(*I) << " ";
			I->print(errs());
			errs() << "is redundant.\n";
		}
	}
}

//--------------------------------------------------------------------------------------//
// Passes for the getting flush sets
//--------------------------------------------------------------------------------------//

void initializeModelVerififierPassPass(PassRegistry &);

class ModelVerififierPass : public FunctionPass {
	PerfCheckerInfo<> WritePCI;
	PerfCheckerInfo<> FlushPCI;
	PMInterfaces<> PMI;
	std::vector<Instruction *> FencesVect;

public:
	static char ID;

	ModelVerififierPass() : FunctionPass(ID) {
		//initializeModelVerififierWrapperPassPass(
				//					*PassRegistry::getPassRegistry());
		initializeGenCondBlockSetLoopInfoWrapperPassPass(
									*PassRegistry::getPassRegistry());
	}

	bool runOnFunction(Function &F) override;

	void getAnalysisUsage(AnalysisUsage &AU) const {
		AU.addRequired<DominatorTreeWrapperPass>();
		AU.addRequired<GenCondBlockSetLoopInfoWrapperPass>();
		/*
		AU.addRequired<CFLSteensAAWrapperPass>();
		AU.addRequired<CFLAndersAAWrapperPass>();
		AU.addRequired<SCEVAAWrapperPass>();
		AU.addRequired<GlobalsAAWrapperPass>();
		//AU.addRequired<ObjCARCAAWrapperPass>();
		AU.addRequired<TypeBasedAAWrapperPass>();
		AU.addRequired<ScopedNoAliasAAWrapperPass>();
		AU.addRequired<BasicAAWrapperPass>();
		*/
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


void initializeModelVerifierWrapperPassPass(PassRegistry &);

class ModelVerifierWrapperPass : public FunctionPass {
	PerfCheckerInfo<> WritePCI;
	PerfCheckerInfo<> FlushPCI;
	PMInterfaces<> PMI;
	std::vector<Instruction *> FencesVect;

public:
	static char ID;

	ModelVerifierWrapperPass() : FunctionPass(ID) {
		//initializeModelVerififierWrapperPassPass(
				//					*PassRegistry::getPassRegistry());
		initializeGenCondBlockSetLoopInfoWrapperPassPass(
									*PassRegistry::getPassRegistry());
	}

	bool runOnFunction(Function &F) override;

	void getAnalysisUsage(AnalysisUsage &AU) const {
		AU.addRequired<DominatorTreeWrapperPass>();
		AU.addRequired<GenCondBlockSetLoopInfoWrapperPass>();
		/*
		AU.addRequired<CFLSteensAAWrapperPass>();
		AU.addRequired<CFLAndersAAWrapperPass>();
		AU.addRequired<SCEVAAWrapperPass>();
		AU.addRequired<GlobalsAAWrapperPass>();
		//AU.addRequired<ObjCARCAAWrapperPass>();
		AU.addRequired<TypeBasedAAWrapperPass>();
		AU.addRequired<ScopedNoAliasAAWrapperPass>();
		AU.addRequired<BasicAAWrapperPass>();
		*/
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

	const std::vector<Instruction *> &getFencesVect() const {
		return FencesVect;
	}

	const PerfCheckerInfo<> &getPerfCheckerWriteInfo() const {
		return WritePCI;
	}

	const PerfCheckerInfo<> &getPerfCheckerFlushInfo() const {
		return FlushPCI;
	}

	const PMInterfaces<> &getPmemInterfaces() const {
		return PMI;
	}
};

} // end of namespace llvm

#endif  // PM_MODEL_VERIFIER_H_
