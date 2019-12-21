
#ifndef PM_FENCE_CHECKER_H__
#define PM_FENCE_CHECKER_H__

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"
#include "llvm/PassSupport.h"
#include "llvm/Pass.h"
#include "llvm/PassAnalysisSupport.h"

#include "GenCondInfo.h"

namespace llvm {

void initializeFenceCheckerLegacyPassPass(PassRegistry &);

class FenceCheckerLegacyPass : public FunctionPass {
	PMInterfaces<> PMI;

public:
	static char ID;

	FenceCheckerLegacyPass() : FunctionPass(ID) {
		initializeFenceCheckerLegacyPassPass(
					*PassRegistry::getPassRegistry());
	}

	bool runOnFunction(Function &F) override;

	void getAnalysisUsage(AnalysisUsage &AU) const {
		AU.addRequired<DominatorTreeWrapperPass>();
		AU.addRequired<GenCondBlockSetLoopInfoWrapperPass>();
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

class FenceCheckerPass : public FunctionPass {
PMInterfaces<> PMI;

public:
	static char ID;

	FenceCheckerPass() : FunctionPass(ID) {
		initializeGenCondBlockSetLoopInfoWrapperPassPass(
							*PassRegistry::getPassRegistry());
	}

	bool runOnFunction(Function &F) override;

	void getAnalysisUsage(AnalysisUsage &AU) const {
		errs() << "ANALYSING\n";
		AU.addRequired<DominatorTreeWrapperPass>();
		AU.addRequired<GenCondBlockSetLoopInfoWrapperPass>();
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

#endif //PM_FENCE_CHECKER_H__
