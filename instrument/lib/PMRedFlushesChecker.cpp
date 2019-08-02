//============= Performance Checker for PMDK using applications ================//
//
// Looks for semantics that may detrimant performance of a system using
// persistant memory or not. We also check whether correct instructions
// are used or not.  Use of incorrect type of instructions can cause slowdowns.
//
//===============================================================================//

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
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Support/Casting.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/ADT/STLExtras.h"

#include "CondBlockBase.h"
#include "CondBlockBaseImpl.h"
#include "GenCondInfo.h"
#include "GenCondInfoImpl.h"
#include "Interfaces.h"
#include "PMRedFlushesChecker.h"
#include "SCC_Iterator.h"
#include "FlowAwarePostOrder.h"
#include "WriteAliasCheck.h"
#include "CommonSCCOps.h"
#include "InstsSet.h"

#include <iostream>
#include <vector>
#include <algorithm>

using namespace llvm;

static cl::opt<bool> PrintRedFlushes("print-flushes", cl::Hidden,
			 	 	 	 	 	 	 cl::desc("Print Redundant Flushes"), cl::init(true));

static cl::opt<bool> NoFlushesAliasCheck("no-flushes-alias-check", cl::Hidden,
			 	 	 	 	 	 	 cl::desc("No Flushes Alias check"), cl::init(false));

static cl::opt<bool> FlowInsensitiveAlias("flow-insensitive-flushes-alias-check", cl::Hidden,
			 	 	cl::desc("Perform Flow-Insensitive Flushes Alias check"), cl::init(false));


// This relies heavily on the fact that the serial flush instructions are actually pre-ordered.
// Note that we separate the flushes in loops from flushes outside those loops if we cannot
// guarantee that the loop executes at least once.
static void SeparateAcrossLoopsAndCondBlockSets(SCCToInstsPairVectTy &SCCToFlushesPairVect,
												GenCondBlockSetLoopInfo &GI, PMInterfaces<> &PMI,
												AAResults &AA,
												SmallVector<Value *, 16> &StackAndGlobalVarVect) {
// Get all relevant interfaces
	auto &FI = PMI.getFlushInterface();
	auto &PI = PMI.getPersistInterface();
	auto &MI = PMI.getMsyncInterface();
	auto &DI = PMI.getDrainInterface();
	auto &PMMI = PMI.getPmemInterface();
	auto &MPI = PMI.getMapInterface();

// Map to record whether a loop writes to memory or not
	DenseMap<GenLoop *, bool> LoopToMemWriteMap;

// Set to record wehther a loop contains condblock sets with flushes
	DenseSet<GenLoop *> LoopToCondBlockSetStatusSet;

	auto LoopWritesToPMem = [&](GenLoop *GL) {
		DenseMap<GenLoop *, bool>::iterator It = LoopToMemWriteMap.find(GL);
		if(It == LoopToMemWriteMap.end()) {
			for(BasicBlock *BB : GL->getBlocksVector()) {
				for(Instruction &I : *BB) {
					if(StoreInst *SI = dyn_cast<StoreInst>(&I)) {
						if(WriteAliases(SI, StackAndGlobalVarVect, AA))
							continue;
						LoopToMemWriteMap[GL] = true;
						return true;
					}
					if(CallInst *CI = dyn_cast<CallInst>(&I)) {
					// Ignore the instrinsics that are not writing to memory
						if(dyn_cast<IntrinsicInst>(CI)
						&& !dyn_cast<AnyMemIntrinsic>(CI)) {
							continue;
						}

					// Skip the following functions and calls that do not change memory
						if(FI.isValidInterfaceCall(CI)
						|| MI.isValidInterfaceCall(CI)
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

						LoopToMemWriteMap[GL] = true;
						return true;
					}
				}
			}
			LoopToMemWriteMap[GL] = false;
			return false;
		}
		return (*It).getSecond();
	};

// Lambda function to separate serial flush sets
	auto SeparateFlushSets = [](SCCToInstsPairTy &SCCToFlushesPair,
								SerialInstsSet<>::iterator &SFI,
								SCCToInstsPairVectTy &PairVect) {
	// We remove this flush instruction from the set and
	// all the subsequent flushes from this set and append
	// it to the vector.
		auto &SF = std::get<1>(SCCToFlushesPair);
		auto &SCCIterator = std::get<0>(SCCToFlushesPair);
		SerialInstsSet<> NewSF;
		for(SerialInstsSet<>::iterator I = SFI; I != SF.end(); ++I)
			NewSF.push_back(*I);
		SF.erase(SFI, SF.end());
		PairVect.push_back(std::make_pair(SCCIterator, NewSF));
	};

// Now we look for individual loops and condblock sets in the SCCs and separate
// flushes sets out across loop and condblock set boundaries.
	auto SeparateFlushSetsAcrossLoopsAndCondBlockSets =
										[&](SCCToInstsPairTy &SCCToFlushesPair,
											SCCToInstsPairVectTy &PairVect) {
	// If an SCC has no loop, then there is nothing more to do
		auto &SCCIterator = std::get<0>(SCCToFlushesPair);
		if(!SCCIterator.hasLoop())
			return;

	// If there is only one flush instruction, just move on
		auto &SF = std::get<1>(SCCToFlushesPair);
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
				SeparateFlushSets(SCCToFlushesPair, SFI, PairVect);

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
					if(LoopWritesToPMem(L)) {
						SeparateFlushSets(SCCToFlushesPair, SFI, PairVect);
						return;
					}
					L = GL;
					SFI++;
					continue;
				}
				if(!L || (GL && L->contains(GL))) {
					errs() << "NEST 2\n";
					if(LoopWritesToPMem(GL)) {
						SeparateFlushSets(SCCToFlushesPair, SFI, PairVect);
						return;
					}
					L = GL;
					SFI++;
					continue;
				}
				if((GL && LoopWritesToPMem(GL)) || (L && LoopWritesToPMem(L))) {
					errs() << "SEPARATE LOOPS\n";
					SeparateFlushSets(SCCToFlushesPair, SFI, PairVect);
					return;
				}
				L = GL;
			}
			SFI++;
		}
	};

	auto SeparateFlushSetsAcrossCondBlockSetsInLoops =
													[&](SCCToInstsPairTy &SCCToFlushesPair,
														SCCToInstsPairVectTy &PairVect) {
	// If an SCC has no loop, then there is nothing more to do
		auto &SCCIterator = std::get<0>(SCCToFlushesPair);
		if(!SCCIterator.hasLoop())
			return;

	// If there is only one flush instruction, just move on
		auto &SF = std::get<1>(SCCToFlushesPair);
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
						SeparateFlushSets(SCCToFlushesPair, SFI, PairVect);
						return;
					}
					L = GL;
					SFI++;
					continue;
				}
				if(!L || (GL && L->contains(GL))) {
					errs() << "NEST 2\n";
					if(LoopToCondBlockSetStatusSet.find(GL) != LoopToCondBlockSetStatusSet.end()) {
						SeparateFlushSets(SCCToFlushesPair, SFI, PairVect);
						return;
					}
					L = GL;
					SFI++;
					continue;
				}
				if((GL && LoopToCondBlockSetStatusSet.find(GL) != LoopToCondBlockSetStatusSet.end())
				|| (L && LoopToCondBlockSetStatusSet.find(L) != LoopToCondBlockSetStatusSet.end())) {
					errs() << "SEPARATE LOOPS\n";
					SeparateFlushSets(SCCToFlushesPair, SFI, PairVect);
					return;
				}
				L = GL;
			}
			SFI++;
		}
	};

// Iterate over the serial flushes sets
	for(unsigned Index = 0; Index != SCCToFlushesPairVect.size(); ++Index) {
		auto &SCCToFlushesPair = SCCToFlushesPairVect[Index];
		errs() << "ITERATING\n";
		SeparateFlushSetsAcrossLoopsAndCondBlockSets(SCCToFlushesPair, SCCToFlushesPairVect);
		errs() << "--SIZE: " << SCCToFlushesPairVect.size() << "\n";
	}

	errs() << "HALF LOOPS dealt with\n";
	for(auto &SCCToFlushesPair : SCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "++++++++++++++++++++++++++++++++\n\n";

// Iterate over the serial flushes sets again
	for(unsigned Index = 0; Index != SCCToFlushesPairVect.size(); ++Index) {
		auto &SCCToFlushesPair = SCCToFlushesPairVect[Index];
		errs() << "ITERATING\n";
		SeparateFlushSetsAcrossCondBlockSetsInLoops(SCCToFlushesPair, SCCToFlushesPairVect);
		errs() << "--SIZE: " << SCCToFlushesPairVect.size() << "\n";
	}
}

static void
IterateBlockToGroupFlushes(BasicBlock *BB, bool &InterveningWrites, bool &WriteStop,
												   SerialInstsSet<> &SF, PMInterfaces<> &PMI,
												   SCC_Iterator<Function *> &SCCIterator,
												   SCCToInstsPairVectTy &SCCToFlushesPairVect,
												   SmallVector<BasicBlock *, 4> &BBWithFirstSerialInsts,
												   DenseMap<BasicBlock *, SCC_Iterator<Function *>> &BlockToSCCMap,
												   SmallVector<Value *, 16> &StackAndGlobalVarVect,
												   std::vector<Instruction *> &WriteVect, AAResults &AA) {
// Get all relevant interfaces
	auto &FI = PMI.getFlushInterface();
	auto &MI = PMI.getMsyncInterface();
	auto &MPI = PMI.getMapInterface();
	auto &DI = PMI.getDrainInterface();
	auto &PI = PMI.getPersistInterface();
	auto &PMMI = PMI.getPmemInterface();

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

			if(SF.size()) {
				errs() << "GROUP FLUSHES\n";
				errs() << "FLUSHES COMMITED\n";
				auto Pair = std::make_pair(SCCIterator, SF);
				SCCToFlushesPairVect.push_back(Pair);
				SF.clear();
				WriteStop = true;
				if(!InterveningWrites && !SCCIterator.hasLoop())
					BBWithFirstSerialInsts.push_back(BB);
			}
			WriteVect.push_back(SI);
			InterveningWrites = true;
			continue;
		}
		if(CallInst *CI = dyn_cast<CallInst>(Inst)) {
			Inst->print(errs());
			std::cout << "\n";
			errs() << CI->getCalledFunction()->getName() << "\n";

		// Ignore the instrinsics that are not writing to memory
			if(dyn_cast<IntrinsicInst>(CI)
			&& !dyn_cast<AnyMemIntrinsic>(CI)) {
				continue;
			}

		// Deal with the flushes
			if(FI.isValidInterfaceCall(CI)
			|| PI.isValidInterfaceCall(CI)) {
				SF.push_back(CI);
				WriteStop = false;
				errs() << "FLUSH FOUND\n";
				continue;
			}

		// Skip the following functions and calls that do not change memory
			if(MI.isValidInterfaceCall(CI)
			|| DI.isValidInterfaceCall(CI)
			|| MPI.isValidInterfaceCall(CI)
			|| CI->getCalledFunction()->onlyReadsMemory()) {
				continue;
			}

		// If the call is a recognizable memory operation that is
		// capable of writing to stack and globals, we need to
		// perform alias analysis here.
			bool MostLikelyPMWrite = PMMI.isValidInterfaceCall(CI);
			if(!MostLikelyPMWrite && WriteAliases(CI, StackAndGlobalVarVect, AA)) {
				errs() << "WRITE ALIAS CONTINUE\n";
				continue;
			}

			errs() << "WRITE ALIAS NOT CONTINUE\n";
			if(SF.size()) {
				errs() << "GROUP FLUSHES\n";
				auto Pair = std::make_pair(SCCIterator, SF);
				SCCToFlushesPairVect.push_back(Pair);
				SF.clear();
				errs() << "FLUSHES COMMITED\n";
				WriteStop = true;
				if(!InterveningWrites && !SCCIterator.hasLoop())
					BBWithFirstSerialInsts.push_back(BB);
			}

		// If we are certain that this function call writes to memory, add it to record
			if(MostLikelyPMWrite || dyn_cast<AnyMemIntrinsic>(CI))
				WriteVect.push_back(CI);

			InterveningWrites = true;
			continue;
		}
	}
}

static void GroupSerialInstsInSCC(Function *F,  GenCondBlockSetLoopInfo &GI,
																	DominatorTree &DT, AAResults &AA,
																	std::vector<Instruction *> &WriteVect, PMInterfaces<> &PMI,
																	SCCToInstsPairVectTy &SCCToFlushesPairVect,
																	SCCToInstsPairVectTy &WriteFreeSCCToFlushesPairVect,
																	SmallVector<BasicBlock *, 4> &BBWithFirstSerialInsts,
																	DenseMap<BasicBlock *, SCC_Iterator<Function *>> &BlockToSCCMap,
																	SmallVector<Value *, 16> &StackAndGlobalVarVect) {
// Get all the globals
	for(Module::global_iterator It = F->getParent()->global_begin();
													It != F->getParent()->global_end(); It++) {
		if(GlobalVariable *GV = dyn_cast<GlobalVariable>(&*It))
			StackAndGlobalVarVect.push_back(GV);
	}

// Iterate over the function quickly to let all allocas
	for(auto &BB : *F) {
		for(auto &I : BB) {
			if(AllocaInst *AI = dyn_cast<AllocaInst>(&I))
				StackAndGlobalVarVect.push_back(AI);
		}
	}

// Iterate over all the SCCs
	errs() << "SCC ITERATOR ALL READY\n";
	for(SCC_Iterator<Function *> SCCIterator = SCC_Iterator<Function *>::begin(F);
													!SCCIterator.isAtEnd(); ++SCCIterator) {
		SerialInstsSet<> SF;
		bool InterveningWrites = false;
		bool WriteStop = false;
		errs() << "----------SCC-----------\n";

	// Iterate  over the blocks in preorder
		SCCIterator.printSCC();
		if((*SCCIterator).size()  == 1) {
		// Optimize for a common case
			BasicBlock *BB = DT.getNode((*SCCIterator)[0])->getBlock();
			IterateBlockToGroupFlushes(BB, InterveningWrites, WriteStop, SF,
									   PMI, SCCIterator, SCCToFlushesPairVect,
									   BBWithFirstSerialInsts, BlockToSCCMap,
									   StackAndGlobalVarVect, WriteVect, AA);
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

				IterateBlockToGroupFlushes(BB, InterveningWrites, WriteStop, SF,
																   PMI, SCCIterator, SCCToFlushesPairVect,
																   BBWithFirstSerialInsts, BlockToSCCMap,
										   				 		 StackAndGlobalVarVect, WriteVect, AA);
			}
		}

		errs() << "CHECK INTERVENING WRITES" << WriteStop << "\n";
		if(!WriteStop) {
		// Case of SCC which does not write to memory or if it does, the
		// flushes post-dominate all writes in a block or group of blocks.
			errs() << "NO INTERVENTING WRITES" << SF.size() << "\n";
			if(SF.size()) {
				errs() << "--GROUP FLUSHES\n";
				auto Pair = std::make_pair(SCCIterator, SF);
				if(InterveningWrites && SCCIterator.hasLoop())
					SCCToFlushesPairVect.push_back(Pair);
				else
					WriteFreeSCCToFlushesPairVect.push_back(Pair);
				SF.clear();
			}
		}
	}
}

static AliasResult FlushAliasCheck(CallInst *FlushA, CallInst *FlushB,
																	 AAResults &AA, PMInterfaces<> &PMI,
																   RedFlushesRecord &FR) {
// Get relevant interfaces
	auto &FI = PMI.getFlushInterface();
	auto &PI = PMI.getPersistInterface();

// Check if the size of the object being flushed is compile-time constant.
// If not, we skip it.
	Value *PtrA;
	Value *PtrB;
	uint64_t SizeA;
	uint64_t SizeB;
	if(FI.isValidInterfaceCall(FlushA)) {
		auto *CI = dyn_cast<ConstantInt>(FI.getPMemLenOperand(FlushA));
		if(!CI)
			return AliasResult::MayAlias;
		SizeA = CI->getZExtValue();
		PtrA = FI.getPMemAddrOperand(FlushA);
	} else {
	// Implied
		assert(PI.isValidInterfaceCall(FlushA) && "Flush for alias analysis is invalid.");
		auto *CI = dyn_cast<ConstantInt>(PI.getPMemLenOperand(FlushA));
		if(!CI)
			return AliasResult::MayAlias;
		SizeA = CI->getZExtValue();
		PtrA = PI.getPMemAddrOperand(FlushA);
	}
	if(FI.isValidInterfaceCall(FlushB)) {
		auto *CI = dyn_cast<ConstantInt>(FI.getPMemLenOperand(FlushB));
		if(!CI)
			return AliasResult::MayAlias;
		SizeB = CI->getZExtValue();
		PtrB = FI.getPMemAddrOperand(FlushB);
	} else {
	// Implied
		assert(PI.isValidInterfaceCall(FlushB) && "Flush for alias analysis is invalid.");
		auto *CI = dyn_cast<ConstantInt>(PI.getPMemLenOperand(FlushB));
		if(!CI)
			return AliasResult::MayAlias;
		SizeB = CI->getZExtValue();
		PtrB = PI.getPMemAddrOperand(FlushB);
	}

	const MemoryLocation &LocA = MemoryLocation(PtrA, LocationSize(SizeA * 8));
	const MemoryLocation &LocB = MemoryLocation(PtrB, LocationSize(SizeB * 8));
	auto Res = AA.alias(LocA, LocB);
	if(Res == AliasResult::PartialAlias || Res == AliasResult::MustAlias) {
	// Add to flush record
		FR.addPair(FlushA, FlushB);
	}
	return Res;
}

static void FlushesAliasCheck(SCCToInstsPairVectTy &SCCToFlushesPairVect,
							   							AAResults &AA, PMInterfaces<> &PMI, RedFlushesRecord &FR) {
	for(auto &Pair : SCCToFlushesPairVect) {
		auto &SF = std::get<1>(Pair);
		if(SF.size() == 1)
			continue;

		DenseMap<CallInst *, unsigned> FlushToNumNoAliasMap;
		SmallVector<unsigned , 4> RemoveFlushesVect;
		for(unsigned Index = 0; Index != SF.size(); ++Index) {
			auto *FlushA = dyn_cast<CallInst>(SF[Index]);
			unsigned NoAlias = 0;
			for(unsigned It_Index = Index + 1; It_Index != SF.size(); ++It_Index) {
				auto *FlushB = dyn_cast<CallInst>(SF[It_Index]);
				auto Res = FlushAliasCheck(FlushA, FlushB, AA, PMI,FR);
				if(Res == AliasResult::NoAlias) {
					NoAlias++;
					FlushToNumNoAliasMap[FlushB]++;
				}
			}
			unsigned Sum = NoAlias;
			auto Iterator = FlushToNumNoAliasMap.find(FlushA);
			if(Iterator != FlushToNumNoAliasMap.end())
				Sum += (*Iterator).second;
			if(Sum == SF.size() - 1)
				RemoveFlushesVect.push_back(Index);
		}

	// Remove the flushes at do not alias with others at all
		unsigned NumRemovedFlushes = 0;
		for(unsigned Index : RemoveFlushesVect)
			SF.erase(SF.begin() + Index - NumRemovedFlushes++);
	}
}

static void PopulateSerialInstsInfo(Function *F, GenCondBlockSetLoopInfo &GI,
																	  DominatorTree &DT, AAResults &AA,
																	  std::vector<Instruction *> &WriteVect,
																	  PMInterfaces<> &PMI, PerfCheckerInfo<> &PCI) {
// Vectors for serial flushes
	SCCToInstsPairVectTy SCCToFlushesPairVect;
	SCCToInstsPairVectTy WriteFreeSCCToFlushesPairVect;
	SmallVector<BasicBlock *, 4> BBWithFirstSerialInsts;
	DenseMap<BasicBlock *, SCC_Iterator<Function *>> BlockToSCCMap;
	SmallVector<Value *, 16> StackAndGlobalVarVect;

// Now we need to iterate over the strongly connected components in
// order to clump up the consecutive flush instructions together.
	GroupSerialInstsInSCC(F, GI, DT, AA, WriteVect, PMI, SCCToFlushesPairVect,
							WriteFreeSCCToFlushesPairVect,
							BBWithFirstSerialInsts, BlockToSCCMap,
							StackAndGlobalVarVect);

	errs() << "SCCs dealt with\n";
	for(auto &SCCToFlushesPair : SCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "++++++++++++++++++++++++++++++++\n\n";
	for(auto &SCCToFlushesPair : WriteFreeSCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}

// Merge the serial flushes across SCCs that do not have stores
	MergeAcrossSCCs(SCCToFlushesPairVect, WriteFreeSCCToFlushesPairVect,
			  	    BBWithFirstSerialInsts, BlockToSCCMap);

// No need for this anymore
	BBWithFirstSerialInsts.clear();

	errs() << "SCC Merged\n";
	for(auto &SCCToFlushesPair : SCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "++++++++++++++++++++++++++++++++\n\n";
	for(auto &SCCToFlushesPair : WriteFreeSCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}

	if(FlowInsensitiveAlias) {
		RedFlushesRecord FR;
		FlushesAliasCheck(SCCToFlushesPairVect, AA, PMI, FR);
		FlushesAliasCheck(WriteFreeSCCToFlushesPairVect, AA, PMI, FR);
		if(PrintRedFlushes) {
			errs() << "FLOW INSENSITIVE FLUSH ALIAS CHECK\n";
			FR.printRecord();
		}
		FR.clear();
	}

// Separate the serial flushes along loop and condblock set boundaries
	SeparateAcrossLoopsAndCondBlockSets(SCCToFlushesPairVect, GI, PMI,
										AA, StackAndGlobalVarVect);

	errs() << "LOOPS dealt with\n";
	for(auto &SCCToFlushesPair : SCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "++++++++++++++++++++++++++++++++\n\n";
	for(auto &SCCToFlushesPair : WriteFreeSCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}

// Separate the serial flushes along SCCs, loop and condblock set boundaries
	SeparateAcrossSCCsAndCondBlockSets(WriteFreeSCCToFlushesPairVect, BlockToSCCMap, GI);

	errs() << "SCCs dealt with\n";
	for(auto &SCCToFlushesPair : SCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}
	errs() << "++++++++++++++++++++++++++++++++\n\n";
	for(auto &SCCToFlushesPair : WriteFreeSCCToFlushesPairVect) {
		std::get<1>(SCCToFlushesPair).printSerialInsts();
		errs() << "-----------------------------\n\n";
	}

// Merge collections
	SCCToFlushesPairVect.append(WriteFreeSCCToFlushesPairVect.begin(),
								WriteFreeSCCToFlushesPairVect.end());

// Now, we perform alias analysis on the sets that we have
	if(!NoFlushesAliasCheck) {
		RedFlushesRecord FR;
		FlushesAliasCheck(SCCToFlushesPairVect, AA, PMI, FR);
		if(PrintRedFlushes) {
			errs() << "FLOW SENSITIVE FLUSH ALIAS CHECK\n";
			FR.printRecord();
		}
	}

// Record all the serial flushes sets
	for(auto &Pair : SCCToFlushesPairVect)
		PCI.addSerialInstsSet(F, std::get<1>(Pair));
}

// Initialze the the wrapper pass
char RedFlushesCheckerWrapperPass::ID = 0;

//
static RegisterPass<RedFlushesCheckerWrapperPass> PassObj("FlushCheck",
														  "Perform Check on Flushes");
/*
INITIALIZE_PASS_BEGIN(RedFlushesCheckerWrapperPass, "redundant-flushes-check",
                	  "Perform Check on Flushes", true, true)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GenCondBlockSetLoopInfoWrapperPass)
INITIALIZE_PASS_END(RedFlushesCheckerWrapperPass, "redundant-flushes-check",
               	   	"Perform Check on Flushes", true, true)
*/

bool RedFlushesCheckerWrapperPass::runOnFunction(Function &F) {
	if(!F.size())
		return false;

	errs() << "WORKING\n";
	auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
	auto &GI =
		getAnalysis<GenCondBlockSetLoopInfoWrapperPass>().getGenCondInfoWrapperPassInfo();

	auto &AA = getAnalysis<AAResultsWrapperPass>().getAAResults();

	PopulateSerialInstsInfo(&F, GI, DT, AA, WriteVect, PMI,  PCI);
	PCI.printFuncToSerialInstsSetMap();

	return false;
}
