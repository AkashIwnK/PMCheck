

#ifndef PM_INSTRUMENTER_H_
#define PM_INSTRUMENTER_H_


#include "CondBlockBase.h"
#include "CondBlockBaseImpl.h"
#include "GenCondInfo.h"
#include "GenCondInfoImpl.h"
#include "Interfaces.h"
#include "SCC_Iterator.h"
#include "FlowAwarePostOrder.h"
#include "WriteAliasCheck.h"
#include "PMModelVerifier.h"
#include "InstsSet.h"
#include "CommonSCCOps.h"

namespace llvm {

void initializeInstrumentationPassPass(PassRegistry &);

class InstrumentationPass : public FunctionPass {
// Function for instrumntation
	Function *FenceEncountered;
	Function *RecordStrictWrites;
	Function *RecordNonStrictWrites;
	Function *RecordFlushes;

// Map for mapping instruction IDs and their line numbers
	std::map<uint32_t, uint64_t> InstIdToLineNoMap;

public:
	static char ID;

	InstrumentationPass() : FunctionPass(ID) {
		//initializeModelVerififierWrapperPassPass(
			//					*PassRegistry::getPassRegistry());
		//initializeInstrumentationPassPass(*PassRegistry::getPassRegistry());
		initializeGenCondBlockSetLoopInfoWrapperPassPass(
										*PassRegistry::getPassRegistry());
		initializeModelVerifierWrapperPassPass(*PassRegistry::getPassRegistry());
	}

	bool runOnFunction(Function &F) override;

	void getAnalysisUsage(AnalysisUsage &AU) const {
		AU.addRequired<DominatorTreeWrapperPass>();
		AU.addRequired<GenCondBlockSetLoopInfoWrapperPass>();
		//AU.addRequired<CFLSteensAAWrapperPass>();
		//AU.addRequired<CFLAndersAAWrapperPass>();
		//AU.addRequired<SCEVAAWrapperPass>();
		//AU.addRequired<GlobalsAAWrapperPass>();
		//AU.addRequired<ObjCARCAAWrapperPass>();
		//AU.addRequired<TypeBasedAAWrapperPass>();
		//AU.addRequired<ScopedNoAliasAAWrapperPass>();
		AU.addRequired<BasicAAWrapperPass>();
		AU.addRequired<AAResultsWrapperPass>();
		//AU.addRequired<ModelVerifierWrapperPass>();
		//AU.setPreservesCFG();
		//AU.setPreservesAll();
	}

	bool doInitialization(Module &);

	bool doFinalization(Module &);
};


} // end of namespace llvm

#endif // PM_INSTRUMENTER_H_
