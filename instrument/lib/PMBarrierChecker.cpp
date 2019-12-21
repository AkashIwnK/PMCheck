
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopInfoImpl.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/Casting.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/DebugInfoMetadata.h"

#include "CondBlockBase.h"
#include "CondBlockBaseImpl.h"
#include "GenCondInfo.h"
#include "GenCondInfoImpl.h"
#include "Interfaces.h"
#include "SCC_Iterator.h"
#include "PMBarrierChecker.h"
#include "FlowAwarePostOrder.h"
#include "WriteAliasCheck.h"

#include <iostream>
#include <vector>
#include <algorithm>

using namespace llvm;

// In some cases the fences can be hoisted up if we can statically prove that
// a loop executes at least once.


// Here we use loops to work out whether fences in the loops need to
// be sunk (when the epoch persistency model is being used).
// Fences in loops can easily degrade performance.
// So the fences that we recommend to be sunk out of the loop are
// the ones that do not dominate any stores in the loop.
//
static void LookForFencesInLoops(GenCondBlockSetLoopInfo &GI,
		DominatorTree &DT, PMInterfaces<> &PMI, AAResults &AA,
		SmallVector<Value *, 16> StackAndGlobalVarVect) {
	errs() << "\n\n\n\n";
	auto &FI = PMI.getFlushInterface();
	auto &MI = PMI.getMsyncInterface();
	auto &DI = PMI.getDrainInterface();
	auto &PMMI = PMI.getPmemInterface();
	auto &PI = PMI.getPersistInterface();
	auto &MPI = PMI.getMapInterface();

	// Lambda function to get the line number. We use debug information to do so.
	auto GetLineNumber = [](Instruction &I) {
		if(MDNode *N = I.getMetadata("dbg")) {
			if(DILocation *Loc = dyn_cast<DILocation>(N))
				return ConstantInt::get(Type::getInt32Ty(I.getContext()), Loc->getLine());
		}
		return (ConstantInt *)nullptr;
	};

	SmallVector<Instruction *, 4> FenceInstsVect;
	DenseMap<GenLoop *, bool> LoopsToLatchStatusMap;
	Instruction *CheckPoint = nullptr;
	bool SetCheckPoint = true;
	for(auto *BB : FlowAwarePostOrder(DT.getRootNode(), GI)) {
		GenLoop *L = GI.getLoopFor(BB);
		if(!L)
			continue;

		if(!LoopsToLatchStatusMap.empty()
				&& !LoopsToLatchStatusMap[L] && L->isLoopLatch(BB)) {
			// Set a "checkpoint" here
			if(SetCheckPoint) {
				CheckPoint = BB->getTerminator();
				errs() << "CHECK POINT: ";
				CheckPoint->print(errs());
				errs() << "\n";
				SetCheckPoint = false;
			}
			LoopsToLatchStatusMap[L] = true;
		}
		if(LoopsToLatchStatusMap[L]) {
			errs() << "LOOKING AT  THE BB: ";
			BB->printAsOperand(errs(), false);
			errs() << "\n";
			bool FenceToBeFound = false;
			bool FenceFound = false;
			bool WriteFound = false;
			for(auto &I : *BB) {
				if(StoreInst *SI = dyn_cast<StoreInst>(&I)) {
					if(WriteAliases(SI, StackAndGlobalVarVect, AA))
						continue;

					errs() << "STORE INSTRUCTION FOUND\n";
					// If a fence has been discovered, then we skip this block
					WriteFound = true;
					errs() << "VECT SIZE: " << FenceInstsVect.size() << "\n";
					if(unsigned Size = FenceInstsVect.size()) {
						if(FenceFound) {
							// Pop the flush instruction
							FenceInstsVect.pop_back();
						} else {
							errs() << "FENCE FOUND\n";
							break;
						}
					}
					errs() << "FENCE TO BE FOUND\n";
					// We try to find fence in this block. If we do not find it,
					// we can skip the loop altogether.
					FenceToBeFound = true;
					continue;
				}
				if(CallInst *CI = dyn_cast<CallInst>(&I)) {
					// Ignore the instrinsics that are not writing to memory
					if(dyn_cast<IntrinsicInst>(CI)
							&& !dyn_cast<AnyMemIntrinsic>(CI)) {
						continue;
					}

					// Skip the following functions and calls that do not change memory
					if(MI.isValidInterfaceCall(CI)
							|| DI.isValidInterfaceCall(CI)
							|| PI.isValidInterfaceCall(CI)
							|| MPI.isValidInterfaceCall(CI)
							|| CI->getCalledFunction()->onlyReadsMemory()) {
						continue;
					}

					// If the call is a recognizable memory operation that is
					// capable of writing to stack and globals, we need to
					// perform alias analysis here.
					if(!PMMI.isValidInterfaceCall(CI)
							&& WriteAliases(CI, StackAndGlobalVarVect, AA)) {
						continue;
					}

					if(FI.isValidInterfaceCall(CI)) {
						errs() << "FENCE INSTRUCTION FOUND\n";
						// Fence found!	If the fence is in a condblock set in the
						// current loop, then there is nothing we can do about it.
						if(auto *CondBlockSet = GI.getCondBlockSetFor(BB)) {
							errs() << "FENCE IN CONDBLOCK SET\n";
							if(CondBlockSet->contains(L)) {
								errs() << "CONDBLOCK SET CONTAINS LOOP\n";
								errs() << "FENCE SET\n";
								FenceInstsVect.push_back(CI);
								//FenceInst = CI;
								FenceFound = true;
								if(FenceToBeFound)
									FenceToBeFound = false;
							} else {
								errs() << "CONDBLOCK SET DOES NOT CONTAIN THE LOOP\n";
								// This means that the fence is in a condblock set
								// in a loop. So we move on.
								//LatchFound = false;
								break;
							}
						} else {
							// Fence found. Have we discovered a store already?
							errs() << "FENCE NOT IN CONDBLOCK SET\n";
							errs() << "FENCE SET\n";
							//FenceInst = CI;
							FenceInstsVect.push_back(CI);
							FenceFound = true;
							if(FenceToBeFound)
								FenceToBeFound = false;
						}
						continue;
					}

					// If a fence has been discovered, then we skip this block.
					// We have to make sure that the stores and fences are in the
					// same loop.
					WriteFound = true;
					errs() << "VECT SIZE: " << FenceInstsVect.size() << "\n";
					if(unsigned Size = FenceInstsVect.size()) {
						if(FenceFound) {
							// Pop the flush instruction
							FenceInstsVect.pop_back();
						} else {
							errs() << "FENCE FOUND\n";
							break;
						}
					}

					// We try to find fence in this block. If we do not find it,
					// we can skip the loop altogether.
					errs() << "FENCE TO BE FOUND\n";
					FenceToBeFound = true;
					continue;
				}
			}
			errs() << "BASIC BLOCK DONE\n";
			if(FenceToBeFound) {
				// Fence was not found. Skip the loop.
				LoopsToLatchStatusMap[L] = false;
				SetCheckPoint = true;
				errs() << "-------FENCE TO BE FOUND\n";
			} else {
				errs() << "-------FENCE NOT TO BE FOUND\n";
				if(WriteFound
						|| (BB == L->getHeader() && LoopsToLatchStatusMap[L])) {
					LoopsToLatchStatusMap[L] = false;
					if(!FenceInstsVect.empty()) {
						Instruction *FenceInst = FenceInstsVect.back();
						FenceInstsVect.pop_back();
						SetCheckPoint = true;
						errs() << "---------- FENCE\n";
						// Fence has been found with no store succeeding it.
						// We need to get the line number as well. We do that
						// with the help of debug info.
						if(ConstantInt *CI = GetLineNumber(*FenceInst)) {
							errs() << "Fence at line " << CI->getSExtValue()
								<< " can be sunk out of the loop";
							if(CI = GetLineNumber(*CheckPoint))
								errs() << " at line " << CI->getSExtValue();
							errs() << "\n";
						} else {
							errs() << "Debug info unavailable\n";
						}
					}
				}
			}
		}
	}
	errs() << "----------------------------------------------DONE\n";
}


// This can be run independently through opt utility
char FenceCheckerPass::ID = 0;
static RegisterPass<FenceCheckerPass> PassObj("FenceCheck",
		"Perform Check on Fences in Loops");

// Initialze the the legacy pass
char FenceCheckerLegacyPass::ID = 0;

INITIALIZE_PASS_BEGIN(FenceCheckerLegacyPass, "FenceCheckerWrapper",
		"Perform Check on Fences in Loops", true, true)
	INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GenCondBlockSetLoopInfoWrapperPass)
	INITIALIZE_PASS_END(FenceCheckerLegacyPass, "FenceCheckerWrapper",
			"Perform Check on Fences in Loops", true, true)


	bool FenceCheckerPass::runOnFunction(Function &F) {
		if(F.isDeclaration())
			return false;

		DominatorTree &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
		errs() << "GOT Dominator tree\n";
		auto &GI =
			getAnalysis<GenCondBlockSetLoopInfoWrapperPass>().getGenCondInfoWrapperPassInfo();
		auto &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();

		// Get all the globals and allocas in the function
		SmallVector<Value *, 16> StackAndGlobalVarVect;
		for(Module::global_iterator It = F.getParent()->global_begin();
				It != F.getParent()->global_end(); It++) {
			if(GlobalVariable *GV = dyn_cast<GlobalVariable>(&*It))
				StackAndGlobalVarVect.push_back(GV);
		}
		for(auto &BB : F) {
			for(auto &I : BB) {
				if(AllocaInst *AI = dyn_cast<AllocaInst>(&I))
					StackAndGlobalVarVect.push_back(AI);
			}
		}

		LookForFencesInLoops(GI, DT, PMI, AA, StackAndGlobalVarVect);

		return false;
	}

bool FenceCheckerLegacyPass::runOnFunction(Function &F) {
	if(!F.size())
		return false;
	errs() << "ENTER\n";
	errs() << "--FUNCTION NAME: " << F.getName() << "\n";
	errs() << "WORKING\n";
	DominatorTree &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
	errs() << "--WORKING\n";
	//initializeGenCondBlockSetLoopInfoWrapperPassPass(
	//						*PassRegistry::getPassRegistry());
	errs() << "WORKING\n";
	GenCondBlockSetLoopInfo &GI =
		getAnalysis<GenCondBlockSetLoopInfoWrapperPass>().getGenCondInfoWrapperPassInfo();
	auto &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();

	// Get all the globals and allocas in the function
	SmallVector<Value *, 16> StackAndGlobalVarVect;
	for(Module::global_iterator It = F.getParent()->global_begin();
			It != F.getParent()->global_end(); It++) {
		if(GlobalVariable *GV = dyn_cast<GlobalVariable>(&*It))
			StackAndGlobalVarVect.push_back(GV);
	}
	for(auto &BB : F) {
		for(auto &I : BB) {
			if(AllocaInst *AI = dyn_cast<AllocaInst>(&I))
				StackAndGlobalVarVect.push_back(AI);
		}
	}

	LookForFencesInLoops(GI, DT, PMI, AA, StackAndGlobalVarVect);
	errs() << "FUNCTION NAME: " << F.getName() << "\n";
	return false;
}
