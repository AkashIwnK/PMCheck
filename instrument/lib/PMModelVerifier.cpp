//============ Performance Checker for PMDK using applications ================//
//
// Looks for semantics that may detrimant performance of a system using
// persistant memory or not. We also check  correct instructions
// are used or not.  Use of incorrect type of instructions can cause slowdowns.
//
//=============================================================================//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Pass.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/TargetLibraryInfo.h"

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
#include "LibFuncValidityCheck.h"

#include <iostream>
#include <vector>
#include <algorithm>

using namespace llvm;

// This relies heavily on the fact that the serial instructions are actually
// pre-ordered. Note that we separate the persist instructions in loops from persist
// instructions outside those loops if we cannot guarantee that the loop executes
// at least once.
static void SeparateAcrossLoopsAndCondBlockSets(
												SCCToInstsPairVectTy &SCCToInstsPairVect,
												GenCondBlockSetLoopInfo &GI, PMInterfaces<> &PMI,
												AAResults &AA, SmallVector<Value *, 16> &StackAndGlobalVarVect) {
// Get all relevant interfaces
	auto &PI = PMI.getPersistInterface();
	auto &DI = PMI.getDrainInterface();

// Map to record  a loop contains a fence or not
	DenseMap<GenLoop *, bool> LoopContainsFenceMap;

// Set to record wehther a loop contains condblock sets with persist instructions
	DenseSet<GenLoop *> LoopToCondBlockSetStatusSet;

	auto LoopContainsFence = [&](GenLoop *GL) {
		DenseMap<GenLoop *, bool>::iterator It = LoopContainsFenceMap.find(GL);
		if(It == LoopContainsFenceMap.end()) {
			for(BasicBlock *BB : GL->getBlocksVector()) {
				for(Instruction &I : *BB) {
					if(CallInst *CI = dyn_cast<CallInst>(&I)) {
					// Looking for fences
						if(DI.isValidInterfaceCall(CI)
						|| PI.isValidInterfaceCall(CI)) {
							LoopContainsFenceMap[GL] = true;
							return true;
						}
					}
				}
			}
			LoopContainsFenceMap[GL] = false;
			return false;
		}
		return (*It).getSecond();
	};

// Lambda function to separate serial instruction sets
	auto SeparateInstructionSets = [](SCCToInstsPairTy &SCCToInstsPair,
								SerialInstsSet<>::iterator &SFI,
								SCCToInstsPairVectTy &PairVect) {
	// We remove this instruction from the set and
	// all the subsequent persist instructions from this set and append
	// it to the vector.
		auto &SF = std::get<1>(SCCToInstsPair);
		auto &SCCIterator = std::get<0>(SCCToInstsPair);
		SerialInstsSet<> NewSF;
		for(SerialInstsSet<>::iterator I = SFI; I != SF.end(); ++I)
			NewSF.push_back(*I);
		SF.erase(SFI, SF.end());
		PairVect.push_back(std::make_pair(SCCIterator, NewSF));
	};

// Now we look for individual loops and condblock sets in the SCCs and separate
// persist instructions sets out across loop and condblock set boundaries.
	auto SeparateInstructionSetsAcrossLoopsAndCondBlockSets =
										[&](SCCToInstsPairTy &SCCToInstsPair,
											SCCToInstsPairVectTy &PairVect) {
	// If an SCC has no loop, then there is nothing more to do
		auto &SCCIterator = std::get<0>(SCCToInstsPair);
		if(!SCCIterator.hasLoop())
			return;

	// If there is only one instruction, just move on
		auto &SF = std::get<1>(SCCToInstsPair);
		if(SF.size() == 1)
			return;

		GenLoop *L = GI.getLoopFor(SF[0]->getParent());
		GenCondBlockSet *CBS = GI.getCondBlockSetFor(SF[0]->getParent());
		SerialInstsSet<>::iterator SFI = SF.begin();
		for(Instruction *FI : SF) {
			errs() << "PARENT: ";
			FI->getParent()->printAsOperand(errs(), false);
			errs() << " ";
			errs() << "FLUSH: ";
			FI->print(errs());
			errs() << "\n";
			GenLoop *GL = GI.getLoopFor(FI->getParent());
			GenCondBlockSet *GCBS = GI.getCondBlockSetFor(FI->getParent());

		// Deal with condblock sets
			if(GCBS != CBS) {
				errs() << "HERE\n";
				SeparateInstructionSets(SCCToInstsPair, SFI, PairVect);

			// Now we also record the loops these condblock sets happen
			// to be in.
				LoopToCondBlockSetStatusSet.insert(GL);
				return;
			}

		// Deal with loops
			if(GL != L) {
				errs() << "LOOPS ARE UNEQUAL\n";
				if(!GL || (L && GL->contains(L))) {
					errs() << "NEST 1\n";
					if(LoopContainsFence(L)) {
						SeparateInstructionSets(SCCToInstsPair, SFI, PairVect);
						return;
					}
					L = GL;
					SFI++;
					continue;
				}
				if(!L || (GL && L->contains(GL))) {
					errs() << "NEST 2\n";
					if(LoopContainsFence(GL)) {
						SeparateInstructionSets(SCCToInstsPair, SFI, PairVect);
						return;
					}
					L = GL;
					SFI++;
					continue;
				}
				if((GL && LoopContainsFence(GL)) || (L && LoopContainsFence(L))) {
					errs() << "SEPARATE LOOPS\n";
					SeparateInstructionSets(SCCToInstsPair, SFI, PairVect);
					return;
				}
				L = GL;
			}
			SFI++;
		}
	};

	auto SeparateInstructionSetsAcrossCondBlockSetsInLoops =
													[&](SCCToInstsPairTy &SCCToInstsPair,
														SCCToInstsPairVectTy &PairVect) {
	// If an SCC has no loop, then there is nothing more to do
		auto &SCCIterator = std::get<0>(SCCToInstsPair);
		if(!SCCIterator.hasLoop())
			return;

	// If there is only one instruction, just move on
		auto &SF = std::get<1>(SCCToInstsPair);
		if(SF.size() == 1)
			return;

		GenLoop *L = GI.getLoopFor(SF[0]->getParent());
		SerialInstsSet<>::iterator SFI = SF.begin();
		for(Instruction *FI : SF) {
			errs() << "PARENT: ";
			FI->getParent()->printAsOperand(errs(), false);
			errs() << " ";
			errs() << "FLUSH: ";
			FI->print(errs());
			errs() << "\n";
			GenLoop *GL = GI.getLoopFor(FI->getParent());
			if(GL != L) {
				if(!GL || (L && GL->contains(L))) {
					errs() << "NEST 1\n";
					if(LoopToCondBlockSetStatusSet.find(L) != LoopToCondBlockSetStatusSet.end()) {
						SeparateInstructionSets(SCCToInstsPair, SFI, PairVect);
						return;
					}
					L = GL;
					SFI++;
					continue;
				}
				if(!L || (GL && L->contains(GL))) {
					errs() << "NEST 2\n";
					if(LoopToCondBlockSetStatusSet.find(GL) != LoopToCondBlockSetStatusSet.end()) {
						SeparateInstructionSets(SCCToInstsPair, SFI, PairVect);
						return;
					}
					L = GL;
					SFI++;
					continue;
				}
				if((GL && LoopToCondBlockSetStatusSet.find(GL) != LoopToCondBlockSetStatusSet.end())
				|| (L && LoopToCondBlockSetStatusSet.find(L) != LoopToCondBlockSetStatusSet.end())) {
					errs() << "SEPARATE LOOPS\n";
					SeparateInstructionSets(SCCToInstsPair, SFI, PairVect);
					return;
				}
				L = GL;
			}
			SFI++;
		}
	};

// Iterate over the serial persist instructions sets
	for(unsigned Index = 0; Index != SCCToInstsPairVect.size(); ++Index) {
		auto &SCCToInstsPair = SCCToInstsPairVect[Index];
		errs() << "ITERATING\n";
		SeparateInstructionSetsAcrossLoopsAndCondBlockSets(SCCToInstsPair, SCCToInstsPairVect);
		errs() << "--SIZE: " << SCCToInstsPairVect.size() << "\n";
	}

	errs() << "HALF LOOPS dealt with\n";
	for(auto &SCCToInstsPair : SCCToInstsPairVect) {
		std::get<1>(SCCToInstsPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "++++++++++++++++++++++++++++++++\n\n";

// Iterate over the serial persist instructions sets again
	for(unsigned Index = 0; Index != SCCToInstsPairVect.size(); ++Index) {
		auto &SCCToInstsPair = SCCToInstsPairVect[Index];
		errs() << "ITERATING\n";
		SeparateInstructionSetsAcrossCondBlockSetsInLoops(SCCToInstsPair, SCCToInstsPairVect);
		errs() << "--SIZE: " << SCCToInstsPairVect.size() << "\n";
	}
}

static void
IterateBlockToGroupInsts(BasicBlock *BB, bool &InterveningFence, bool &FenceStop,
												 SerialInstsSet<> &SW, SerialInstsSet<> &SF,
												 PMInterfaces<> &PMI, SCC_Iterator<Function *> &SCCIterator,
												 SCCToInstsPairVectTy &SCCToWritesPairVect,
												 SCCToInstsPairVectTy &SCCToFlushesPairVect,
												 SmallVector<BasicBlock *, 4> &BBWithFirstSerialWrites,
												 SmallVector<BasicBlock *, 4> &BBWithFirstSerialFlushes,
												 std::vector<Instruction *> &FencesVect,
												 DenseMap<BasicBlock *, SCC_Iterator<Function *>> &BlockToSCCMap,
												 SmallVector<Value *, 16> &StackAndGlobalVarVect,
												 AAResults &AA, TargetLibraryInfo &TLI, bool StrictModel) {
// Get all relevant interfaces
	auto &FI = PMI.getFlushInterface();
	auto &MI = PMI.getMsyncInterface();
	auto &DI = PMI.getDrainInterface();
	auto &PI = PMI.getPersistInterface();
	auto &PMMI = PMI.getPmemInterface();
	auto &MPI = PMI.getMapInterface();
	auto &UI = PMI.getUnmapInterface();

	errs() << "DEALING BLOCK: ";
	BB->printAsOperand(errs(), false);
	errs() << "\n";
	BlockToSCCMap[BB] = SCCIterator;
	for(Instruction &I : *BB) {
		Instruction *Inst = &I;
		if(StoreInst *SI = dyn_cast<StoreInst>(Inst)) {
			Inst->print(errs());
			std::cout << "\n";
		// Make sure that the store instruction is not writing
		// to stack or global variable. Even partial alias means
		// that the write to stack or globals partially, so that counts.
			if(WriteAliases(SI, StackAndGlobalVarVect, AA))
				continue;

			SW.push_back(SI);
			FenceStop = false;
			continue;
		}
		if(CallInst *CI = dyn_cast<CallInst>(Inst)) {
			Inst->print(errs());
			std::cout << "\n";

		// Ignore most of the target library calls
			bool IsLibMemCall = false;
			if(auto *Callee = CI->getCalledFunction()) {
				errs() << CI->getCalledFunction()->getName() << "\n";

			// If the called function just reads memory, ignore it
				if(Callee->onlyReadsMemory())
						continue;

			// If it is a funtion that potentially terminates the application by
			// not returning, we need to ignore this function. Example "exit()"
				errs() << "TEST FOR EXIT\n";
				if(CalleeIsTerminatesProgram(Callee)) {
					errs() << "EXIT FOUND\n";
					continue;
				}

				errs() << "FUNCTION CALL NOT CALLING EXIT\n";

				errs() << "CHECKING IF IT IS A LIBRARY FUNCTION CALL\n";
				LibFunc TLIFn;
				if(TLI.getLibFunc(*Callee, TLIFn)) {
					errs() << "LIBRARY FUNCTION CALL\n";
					const DataLayout &DL = Callee->getParent()->getDataLayout();
					if(!IsValidLibMemoryOperation(*(Callee->getFunctionType()), TLIFn, DL)) {
					// The library call is not a valid library call writing to memory,
					// so skip it.
						errs() << "NOT VALID LIB MEMORY OPERATION\n";
						continue;
					}
					IsLibMemCall = true;
				} else {
					errs() << "NOT A LIBRARY FUNCTION CALL\n";
				}
			} else {
			// We do not recognize this function since it seems to be an indirect call.
			// Better to commit the persist operations. This call can be treated like
			// a pure fence in this situation since we can only be safely conservative.
				errs() << "CALLED FUNCTION IS NULL\n";
				if(SW.size()) {
					auto Pair = std::make_pair(SCCIterator, SW);
					SCCToWritesPairVect.push_back(Pair);
					SW.clear();
					if(!InterveningFence && !SCCIterator.hasLoop())
						BBWithFirstSerialWrites.push_back(BB);
				}
				if(SF.size()) {
					auto Pair = std::make_pair(SCCIterator, SF);
					SCCToFlushesPairVect.push_back(Pair);
					SF.clear();
					if(!InterveningFence && !SCCIterator.hasLoop())
						BBWithFirstSerialFlushes.push_back(BB);
				}
				FenceStop = true;
				InterveningFence = true;
				continue;
			}

		// Check for memory intrinsics
			bool IsMemIntrinsic = false;
			if(!IsLibMemCall && dyn_cast<IntrinsicInst>(CI)) {
			// Ignore the instrinsics that are not writing to memory
				if(!dyn_cast<AnyMemIntrinsic>(CI))
					continue;
				IsMemIntrinsic = true;
			}
			errs() << "NOT A NON-MEMORY INTRINSIC\n";

		// If the call is a recognizable memory operation that is capable of writing
		// to stack and globals, we need to perform alias analysis here.
			if((IsLibMemCall || IsMemIntrinsic)
			&& WriteAliases(CI, StackAndGlobalVarVect, AA)) {
				errs() << "WRITE ALIAS CONTINUE\n";
				continue;
			}

		// Check if it is a fairly huge memory operation when strict persistency
		// model is meant to be followed.
			if(!IsLibMemCall && !IsMemIntrinsic && StrictModel
			&& PMMI.isValidInterfaceCall(CI)) {
			// Make sure that the size is no more than 128 bytes because that is the
			// maximum number of bytes that can be written atomically.
				if(auto Length = dyn_cast<ConstantInt>(PMMI.getLengthOperand(CI))) {
					assert(!(Length->getZExtValue() > 128)
							&& "Write Not following strict persistency.");
				}
			}

		// Memory operations that may or definitely write to PM are added to set
			if(IsLibMemCall || IsMemIntrinsic || PMMI.isValidInterfaceCall(CI)) {
				SW.push_back(CI);
				FenceStop = false;
				continue;
			}

		// Pure flush
			if(FI.isValidInterfaceCall(CI)) {
				SF.push_back(CI);
				FenceStop = false;
				errs() << "FLUSH FOUND\n";
				continue;
			}

		// Pure fence
			if(DI.isValidInterfaceCall(CI)) {
				if(SW.size()) {
					auto Pair = std::make_pair(SCCIterator, SW);
					SCCToWritesPairVect.push_back(Pair);
					SW.clear();
					if(!InterveningFence && !SCCIterator.hasLoop())
						BBWithFirstSerialWrites.push_back(BB);
				}
				if(SF.size()) {
					auto Pair = std::make_pair(SCCIterator, SF);
					SCCToFlushesPairVect.push_back(Pair);
					SF.clear();
					if(!InterveningFence && !SCCIterator.hasLoop())
						BBWithFirstSerialFlushes.push_back(BB);
				}
				FencesVect.push_back(CI);
				FenceStop = true;
				InterveningFence = true;
				continue;
			}

		// Flush and fence
			if(PI.isValidInterfaceCall(CI)) {
				SF.push_back(CI);
				if(SW.size()) {
					auto Pair = std::make_pair(SCCIterator, SW);
					SCCToWritesPairVect.push_back(Pair);
					SW.clear();
					if(!InterveningFence && !SCCIterator.hasLoop())
						BBWithFirstSerialWrites.push_back(BB);
				}
				auto Pair = std::make_pair(SCCIterator, SF);
				SCCToFlushesPairVect.push_back(Pair);
				SF.clear();
				if(!InterveningFence && !SCCIterator.hasLoop())
					BBWithFirstSerialFlushes.push_back(BB);
				FencesVect.push_back(CI);
				FenceStop = true;
				InterveningFence = true;
				continue;
			}

		// Skip the following functions and calls that do not change memory
			if(MI.isValidInterfaceCall(CI)
			|| MPI.isValidInterfaceCall(CI)
			|| UI.isValidInterfaceCall(CI)) {
				continue;
			}

		// We do not recognize this function. Better to commit the persist operations.
		// This call can be treated like a pure fence in this situation since we can
		// only be safely conservative.
			if(SW.size()) {
				auto Pair = std::make_pair(SCCIterator, SW);
				SCCToWritesPairVect.push_back(Pair);
				SW.clear();
				if(!InterveningFence && !SCCIterator.hasLoop())
					BBWithFirstSerialWrites.push_back(BB);
			}
			if(SF.size()) {
				auto Pair = std::make_pair(SCCIterator, SF);
				SCCToFlushesPairVect.push_back(Pair);
				SF.clear();
				if(!InterveningFence && !SCCIterator.hasLoop())
					BBWithFirstSerialFlushes.push_back(BB);
			}
			FenceStop = true;
			InterveningFence = true;
			continue;
		}
	}
}

static void GroupSerialInstsInSCC(Function *F,  GenCondBlockSetLoopInfo &GI,
																  DominatorTree &DT, AAResults &AA,
																	TargetLibraryInfo &TLI, PMInterfaces<> &PMI,
																  SCCToInstsPairVectTy &SCCToWritesPairVect,
																  SCCToInstsPairVectTy &SCCToFlushesPairVect,
																  SCCToInstsPairVectTy &FenceFreeSCCToWritesPairVect,
																  SCCToInstsPairVectTy &FenceFreeSCCToFlushesPairVect,
																  SmallVector<BasicBlock *, 4> &BBWithFirstSerialWrites,
																  SmallVector<BasicBlock *, 4> &BBWithFirstSerialFlushes,
																  std::vector<Instruction *> &FencesVect,
																  DenseMap<BasicBlock *, SCC_Iterator<Function *>> &BlockToSCCMap,
																  SmallVector<Value *, 16> &StackAndGlobalVarVect,
																	bool StrictModel = false) {
	errs() << "FUNCTION NAME: " << F->getName() << "\n";
	errs() << "GROUPING SERIAL INSTRUCTIONS IN SCC\n";
// Iterate over all the SCCs
	errs() << "SCC ITERATOR ALL READY\n";
	for(SCC_Iterator<Function *> SCCIterator = SCC_Iterator<Function *>::begin(F);
										!SCCIterator.isAtEnd(); ++SCCIterator) {
		SerialInstsSet<> SW;
		SerialInstsSet<> SF;
		bool InterveningFence = false;
		bool FenceStop = false;
		errs() << "----------SCC-----------\n";

	// Iterate  over the blocks in preorder
		SCCIterator.printSCC();
		if((*SCCIterator).size()  == 1) {
		// Optimize for a common case
			BasicBlock *BB = DT.getNode((*SCCIterator)[0])->getBlock();
			IterateBlockToGroupInsts(BB, InterveningFence, FenceStop,
									 SW, SF, PMI, SCCIterator, SCCToWritesPairVect,
									 SCCToFlushesPairVect, BBWithFirstSerialWrites,
									 BBWithFirstSerialFlushes, FencesVect, BlockToSCCMap,
									 StackAndGlobalVarVect, AA, TLI, StrictModel);
		} else {
			const DomTreeNodeBase<BasicBlock> *DomRoot =
							 DT.getNode((*SCCIterator)[(*SCCIterator).size() - 1]);
			errs() << "SCC ROOT: ";
			DomRoot->getBlock()->printAsOperand(errs(), false);
			errs() << "\n";
			for(auto *BB : FlowAwarePreOrder(DomRoot, GI)) {
				errs() << "SCC CHECK\n";
				BB->printAsOperand(errs(), false);
				errs() << "\n";
				if(!SCCIterator.isInSCC(BB))
					continue;

				IterateBlockToGroupInsts(BB, InterveningFence, FenceStop,
										 SW, SF, PMI, SCCIterator, SCCToWritesPairVect,
										 SCCToFlushesPairVect, BBWithFirstSerialWrites,
										 BBWithFirstSerialFlushes, FencesVect, BlockToSCCMap,
										 StackAndGlobalVarVect, AA, TLI, StrictModel);
			}
		}

		errs() << "CHECK INTERVENING WRITES" << FenceStop << "\n";
		if(!FenceStop) {
		// Case of SCC which does not write to memory or if it does, the
		// persist instructions post-dominate all writes in a block or group of blocks.
			if(SW.size()) {
				errs() << "--GROUP FLUSHES\n";
				auto Pair = std::make_pair(SCCIterator, SW);
				if(InterveningFence && SCCIterator.hasLoop())
					SCCToWritesPairVect.push_back(Pair);
				else
					FenceFreeSCCToWritesPairVect.push_back(Pair);
				SW.clear();
			}
			if(SF.size()) {
				errs() << "--GROUP FLUSHES\n";
				auto Pair = std::make_pair(SCCIterator, SF);
				if(InterveningFence && SCCIterator.hasLoop())
					SCCToFlushesPairVect.push_back(Pair);
				else
					FenceFreeSCCToFlushesPairVect.push_back(Pair);
				SF.clear();
			}
		}
	}
}

static void GetGlobalsAndStackVarsAndTPR(Function *F, GenCondBlockSetLoopInfo &GI,
																				 AAResults &AA, TargetLibraryInfo &TLI,
																				 PMInterfaces<> &PMI,
																				 TempPersistencyRecord<> &TPR,
										 									 	 SmallVector<Value *, 16> &StackAndGlobalVarVect) {
	errs() << "GET GLOBALS AND STACK VARIABLES\n";
	errs() << "FUNCTION NAME: " << F->getName() << "\n";
// Get interfaces
	auto &FI = PMI.getFlushInterface();
	auto &MI = PMI.getMsyncInterface();
	auto &DI = PMI.getDrainInterface();
	auto &PI = PMI.getPersistInterface();
	auto &PMMI = PMI.getPmemInterface();
	auto &MPI = PMI.getMapInterface();
	auto &UI = PMI.getUnmapInterface();

// Get all the globals
	for(Module::global_iterator It = F->getParent()->global_begin();
						It != F->getParent()->global_end(); It++) {
		if(GlobalVariable *GV = dyn_cast<GlobalVariable>(&*It))
			StackAndGlobalVarVect.push_back(GV);
	}

// Iterate over the function quickly to let all allocas. At the sane time
// we try to get some simple "complete" sets of persistency operations that
// comply with some specific persistency model. We record all the compile-time
// errors and throw them after collecting all of them.
	SerialInstsSet<> SW;
	SerialInstsSet<> SF;
	SerialInstsSet<> SFC;
	bool StartFence = false;
	bool InterveningWriteOrFlush = true;
	for(auto &BB : *F) {
	// Skip block if it is in a loop
		if(GI.getLoopFor(&BB)
		|| GI.getCondBlockSetFor(&BB)) {
			errs() << "SKIPPING BLOCK: ";
			BB.printAsOperand(errs(), false);
			GI.getCondBlockSetFor(&BB)->printCondBlockSetInfo();
			errs() << "\n";
		// Drop the collected sets and move on
			SF.clear();
			SW.clear();
			SFC.clear();
			StartFence = false;
			InterveningWriteOrFlush = true;
			continue;
		}

		for(auto &I : BB) {
			Instruction *Inst = &I;

		// Get the alloca along the way
			if(AllocaInst *AI = dyn_cast<AllocaInst>(Inst)) {
				StackAndGlobalVarVect.push_back(AI);
				continue;
			}

			if(StoreInst *SI = dyn_cast<StoreInst>(Inst)) {
				Inst->print(errs());
				std::cout << "\n";
			// Make sure that the store instruction is not writing
			// to stack or global variable. Even partial alias means
			// that the write to stack or globals partially, so that counts.
				if(StartFence && !WriteAliases(SI, StackAndGlobalVarVect, AA)) {
					SW.push_back(SI);
					InterveningWriteOrFlush = true;
				}
				continue;
			}

			if(CallInst *CI = dyn_cast<CallInst>(Inst)) {
				Inst->print(errs());
				std::cout << "\n";

			// Ignore most of the target library calls
				bool IsLibMemCall = false;
				if(auto *Callee = CI->getCalledFunction()) {
					errs() << "CALLED FUNCTION: ";
					errs() << Callee->getName() << "\n";

				// If the called function just reads memory, ignore it
					if(Callee->onlyReadsMemory())
							continue;

				// If it is a funtion that potentially terminates the application by
				// not returning, we need to ignore this function. Example "exit()"
					errs() << "TEST FOR EXIT\n";
					bool IsLibMemCall = false;
					if(CalleeIsTerminatesProgram(Callee)) {
						errs() << "EXIT FOUND\n";
						continue;
					}

					errs() << "FUNCTION CALL NOT CALLING EXIT\n";
					LibFunc TLIFn;
					if(TLI.getLibFunc(*Callee, TLIFn)) {
						const DataLayout &DL = Callee->getParent()->getDataLayout();
						if(!IsValidLibMemoryOperation(*(Callee->getFunctionType()), TLIFn, DL)) {
						// The library call is not a valid library call writing to memory,
						// so skip it.
							errs() << "NOT VALID LIB MEMORY OPERATION\n";
							continue;
						}
						IsLibMemCall = true;
					}
				} else {
				// We have come across a call instruction that we do not recognize since
				// it is most likely an indirect function call. Drop the collected sets
				// and move on.
					errs() << "CALLED FUNCTION IS NULL\n";
					SF.clear();
					SW.clear();
					SFC.clear();
					StartFence = false;
					InterveningWriteOrFlush = true;
					continue;
				}

			// Check for memory intrinsics
				bool IsMemIntrinsic = false;
				if(!IsLibMemCall && dyn_cast<IntrinsicInst>(CI)) {
				// Ignore the instrinsics that are not writing to memory
					if(!dyn_cast<AnyMemIntrinsic>(CI))
						continue;
					IsMemIntrinsic = true;
				}

			// If the call is a recognizable memory operation that is capable of writing
			// to stack and globals, we need to perform alias analysis here.
				if((IsLibMemCall || IsMemIntrinsic)
				&& WriteAliases(CI, StackAndGlobalVarVect, AA)) {
					errs() << "WRITE ALIAS CONTINUE\n";
					continue;
				}

			// Pure fence
				if(!IsLibMemCall && !IsMemIntrinsic && DI.isValidInterfaceCall(CI)) {
				// Ignore the first fence
					if(StartFence) {
						TPR.addWritesAndFlushes(SW, SF);
						if(!SW.size() && !SF.size()) {
						// Redundant flush
							SFC.push_back(CI);
						} else if(!InterveningWriteOrFlush) {
						// Commit the vector of fences
							TPR.addRedFences(SFC);
						} else {
						// Get rid of the collected fences
							SFC.clear();
						}
						SW.clear();
						SF.clear();
					}
					StartFence = true;
					InterveningWriteOrFlush = false;
					continue;
				}

			// Flush and fence
				if(!IsLibMemCall && !IsMemIntrinsic && PI.isValidInterfaceCall(CI)) {
					if(StartFence) {
						SF.push_back(CI);
						TPR.addWritesAndFlushes(SW, SF);
						if(!InterveningWriteOrFlush) {
						// Commit the vector of fences
							TPR.addRedFences(SFC);
						} else {
						// Get rid of the collected fences
							SFC.clear();
						}
						SW.clear();
						SF.clear();
					}
					StartFence = true;
					InterveningWriteOrFlush = false;
					continue;
				}

			// No need to collect anything until the first fence in a function
			// is detected.
				if(!StartFence)
					continue;

			// Pure flush
				if(!IsLibMemCall && !IsMemIntrinsic && FI.isValidInterfaceCall(CI)) {
					SF.push_back(CI);
					errs() << "FLUSH FOUND\n";
					InterveningWriteOrFlush = true;
					continue;
				}

			// Memory operations that may or definitely write to PM are added to set
				if(IsLibMemCall || IsMemIntrinsic || PMMI.isValidInterfaceCall(CI)) {
					SW.push_back(CI);
					InterveningWriteOrFlush = true;
					continue;
				}

			// Skip the following functions and calls that do not change memory
				if(MI.isValidInterfaceCall(CI)
				|| MPI.isValidInterfaceCall(CI)
				|| UI.isValidInterfaceCall(CI)) {
					continue;
				}

			// We have come across a call instruction that we do not recognize.
			// Drop the collected sets and move on.
				SF.clear();
				SW.clear();
				SFC.clear();
				StartFence = false;
				InterveningWriteOrFlush = true;
				continue;
			}
		}
	}
}

static void PopulateSerialInstsInfo(Function *F, GenCondBlockSetLoopInfo &GI,
																  	DominatorTree &DT, AAResults &AA,
																		TargetLibraryInfo &TLI,
																		std::vector<Instruction *> &FencesVect,
																		PMInterfaces<> &PMI,
																		PerfCheckerInfo<> &WritePCI,
																		PerfCheckerInfo<> &FlushPCI) {
// Vectors for serial persist instructions
	SCCToInstsPairVectTy SCCToWritesPairVect;
	SCCToInstsPairVectTy FenceFreeSCCToWritesPairVect;
	SCCToInstsPairVectTy SCCToFlushesPairVect;
	SCCToInstsPairVectTy FenceFreeSCCToFlushesPairVect;
	SmallVector<BasicBlock *, 4> BBWithFirstSerialWrites;
	SmallVector<BasicBlock *, 4> BBWithFirstSerialFlushes;
	DenseMap<BasicBlock *, SCC_Iterator<Function *>> BlockToSCCMap;
	SmallVector<Value *, 16> StackAndGlobalVarVect;
	TempPersistencyRecord<> TPR;
	bool StrictModel = false;

// Get the globals and stack variables
	GetGlobalsAndStackVarsAndTPR(F, GI, AA, TLI, PMI, TPR, StackAndGlobalVarVect);

// Now we need to iterate over the strongly connected components in
// order to clump up the consecutive instructions together.
	GroupSerialInstsInSCC(F, GI, DT, AA, TLI, PMI, SCCToWritesPairVect,
						  SCCToFlushesPairVect, FenceFreeSCCToWritesPairVect,
						  FenceFreeSCCToFlushesPairVect, BBWithFirstSerialWrites,
						  BBWithFirstSerialFlushes, FencesVect,
						  BlockToSCCMap, StackAndGlobalVarVect, StrictModel);

	errs() << "GROUPED SERIAL INSTRUCTIONS\n";
	errs() << "WRITES: \n";
	for(auto &SCCToFlushesPair : SCCToWritesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "FLUSHES: \n";
	for(auto &SCCToFlushesPair : SCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "++++++++++++++++++++++++++++++++\n\n";
	errs() << "WRITES: \n";
	for(auto &SCCToFlushesPair : FenceFreeSCCToWritesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "FLUSHES: \n";
	for(auto &SCCToFlushesPair : FenceFreeSCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}

// Merge the serial persist instructions across SCCs that do not have stores
	MergeAcrossSCCs(SCCToWritesPairVect, FenceFreeSCCToWritesPairVect,
									BBWithFirstSerialWrites, BlockToSCCMap);
	MergeAcrossSCCs(SCCToFlushesPairVect, FenceFreeSCCToFlushesPairVect,
									BBWithFirstSerialFlushes, BlockToSCCMap);

// No need for these anymore
	BBWithFirstSerialFlushes.clear();
	BBWithFirstSerialWrites.clear();

	errs() << "MERGE ACROSS SCCs\n";
	errs() << "WRITES: \n";
	for(auto &SCCToFlushesPair : SCCToWritesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "FLUSHES: \n";
	for(auto &SCCToFlushesPair : SCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "++++++++++++++++++++++++++++++++\n\n";
	errs() << "WRITES: \n";
	for(auto &SCCToFlushesPair : FenceFreeSCCToWritesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "FLUSHES: \n";
	for(auto &SCCToFlushesPair : FenceFreeSCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}

// Separate the serial persist instructions along loop and condblock set boundaries
	SeparateAcrossLoopsAndCondBlockSets(SCCToWritesPairVect, GI, PMI,
										AA, StackAndGlobalVarVect);
	SeparateAcrossLoopsAndCondBlockSets(SCCToFlushesPairVect, GI, PMI,
										AA, StackAndGlobalVarVect);

	errs() << "SEPARATE ACROSS LOOPS AND CONDBLOCK SETS\n";
	errs() << "WRITES: \n";
	for(auto &SCCToFlushesPair : SCCToWritesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "FLUSHES: \n";
	for(auto &SCCToFlushesPair : SCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "++++++++++++++++++++++++++++++++\n\n";
	errs() << "WRITES: \n";
	for(auto &SCCToFlushesPair : FenceFreeSCCToWritesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "FLUSHES: \n";
	for(auto &SCCToFlushesPair : FenceFreeSCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}

// Separate the serial persist instructions along SCCs, loop and condblock set boundaries
	SeparateAcrossSCCsAndCondBlockSets(FenceFreeSCCToWritesPairVect, BlockToSCCMap, GI);
	SeparateAcrossSCCsAndCondBlockSets(FenceFreeSCCToFlushesPairVect, BlockToSCCMap, GI);

	errs() << "SEPARATE ACROSS SCCs AND CONDBLOCK SETS\n";
	errs() << "WRITES: \n";
	for(auto &SCCToFlushesPair : SCCToWritesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "FLUSHES: \n";
	for(auto &SCCToFlushesPair : SCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "++++++++++++++++++++++++++++++++\n\n";
	errs() << "WRITES: \n";
	for(auto &SCCToFlushesPair : FenceFreeSCCToWritesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "FLUSHES: \n";
	for(auto &SCCToFlushesPair : FenceFreeSCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}

// Merge collections
	SCCToWritesPairVect.append(FenceFreeSCCToWritesPairVect.begin(),
							   FenceFreeSCCToWritesPairVect.end());
	SCCToFlushesPairVect.append(FenceFreeSCCToFlushesPairVect.begin(),
								FenceFreeSCCToFlushesPairVect.end());

// Now we iterate over the temporary records to look for problems and
// print all of them.
	TPR.printRecord();

// Record all the serial persist instructions sets
	for(auto &Pair : SCCToWritesPairVect)
		WritePCI.addSerialInstsSet(F, std::get<1>(Pair));
	for(auto &Pair : SCCToFlushesPairVect)
		FlushPCI.addSerialInstsSet(F, std::get<1>(Pair));
}


// Initialze the the wrapper pass
char ModelVerifierWrapperPass::ID = 0;
char ModelVerifierPass::ID = 0;

// Register the pass for the opt tool
static RegisterPass<ModelVerifierPass> PassObj("ModelCheck",
																								 "Perform Check on Insts");

INITIALIZE_PASS_BEGIN(ModelVerifierWrapperPass, "redundant-persist instructions-check",
                	  														"Perform Check on Insts", true, true)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GenCondBlockSetLoopInfoWrapperPass)
INITIALIZE_PASS_END(ModelVerifierWrapperPass, "redundant-persist instructions-check",
               	   														"Perform Check on Insts", true, true)

bool ModelVerifierWrapperPass::runOnFunction(Function &F) {
	if(!F.size())
		return false;

	errs() << "--WORKING\n";
	auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
	auto &GI =
		getAnalysis<GenCondBlockSetLoopInfoWrapperPass>().getGenCondInfoWrapperPassInfo();

	auto &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
	auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();

	PopulateSerialInstsInfo(&F, GI, DT, AA, TLI, FencesVect, PMI,  WritePCI, FlushPCI);
	errs() << "PRINTING WRITES:\n";
	WritePCI.printFuncToSerialInstsSetMap();
	errs() << "PRINTING FLUSHES:\n";
	FlushPCI.printFuncToSerialInstsSetMap();
	errs() << "PRINTING FENCES:\n";
	for(auto *Fence : FencesVect) {
		Fence->print(errs());
		errs() << "\n";
	}

	return false;
}

bool ModelVerifierPass::runOnFunction(Function &F) {
	if(!F.size())
		return false;

	errs() << "MODEL WORKING\n";
	auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
	auto &GI =
		getAnalysis<GenCondBlockSetLoopInfoWrapperPass>().getGenCondInfoWrapperPassInfo();

	auto &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();
	auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();

	PopulateSerialInstsInfo(&F, GI, DT, AA, TLI, FencesVect, PMI,  WritePCI, FlushPCI);
	errs() << "PRINTING WRITES:\n";
	WritePCI.printFuncToSerialInstsSetMap();
	errs() << "PRINTING FLUSHES:\n";
	FlushPCI.printFuncToSerialInstsSetMap();
	errs() << "PRINTING FENCES:\n";
	for(auto *Fence : FencesVect) {
		Fence->print(errs());
		errs() << "\n";
	}

	return false;
}
