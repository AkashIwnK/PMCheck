//=================== COMMON SCC OPERATIONS =====================//
//
//--------------------------------------------------------------//
//
// This contains some common operations that passes perform on
// strongly connected components.
//
//==============================================================//


#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"

#include "CondBlockBase.h"
#include "CondBlockBaseImpl.h"
#include "GenCondInfo.h"
#include "GenCondInfoImpl.h"
#include "InstsSet.h"

#include "CommonSCCOps.h"
#include "SCC_Iterator.h"

using namespace llvm;

// This merges the SCCs that do not contain certain "stop" instructions.
// We define "stop" instructions as instructions at which we stop
// looking for certain persist operations and commit the accumulated sets
// of persistent operations.
void MergeAcrossSCCs(SCCToInstsPairVectTy &SCCToInstsPairVect,
					 SCCToInstsPairVectTy &StopFreeSCCToInstsPairVect,
					 SmallVector<BasicBlock *, 4> &BBWithFirstSerialInsts,
					 DenseMap<BasicBlock *, SCC_Iterator<Function *>> &BlockToSCCMap) {
// Merges the serial persist instructions sets from two SCCs
	auto MergeSCCPairs = [&](unsigned MergeIndex, unsigned RemoveIndex) {
		SerialInstsSet<> &RemoveSF =
					std::get<1>( StopFreeSCCToInstsPairVect[RemoveIndex]);
		SerialInstsSet<> &MergeSF =
					std::get<1>(StopFreeSCCToInstsPairVect[MergeIndex]);

	// Merge serial persist instructions sets through prepend copy
		MergeSF.insert(MergeSF.begin(), RemoveSF.begin(), RemoveSF.end());
	};

	for(unsigned Index = 0 ; Index != StopFreeSCCToInstsPairVect.size(); Index++) {
		errs() << "*****CURRENT INDEX = " << Index << "\n";
		errs() << "*****SIZE= " << StopFreeSCCToInstsPairVect.size() << "\n";
		auto &SCCToInstsPair = StopFreeSCCToInstsPairVect[Index];
		SCC_Iterator<Function *> SCCIterator = std::get<0>(SCCToInstsPair);
		unsigned MergeIndex = Index;
		unsigned RemovalIndex;
		bool SCCsMerged = false;
		for(auto *BB : *SCCIterator) {
			errs() << "ITERATOR BLOCK: ";
			BB->printAsOperand(errs(), false);
			errs() << "\n";
		}

	// If the SCC has a single predecessor SCC with single exit, then we can serial
	// persist instructions sets can be merged.
		if(BasicBlock *BB = SCCIterator.getSCCPredecessor()) {
		// Merge
			errs() << "PREDESSECOR BB:";
			BB->printAsOperand(errs(), false);
			errs() << "\n";
			SCC_Iterator<Function *> PredSCC = BlockToSCCMap[BB];
			unsigned It_Index = 0;
			for(; It_Index != StopFreeSCCToInstsPairVect.size(); It_Index++) {
				auto &SCCToInstsPair = StopFreeSCCToInstsPairVect[It_Index];
				if(PredSCC == std::get<0>(SCCToInstsPair)
				&& It_Index != Index) {
					break;
				}
			}
			if(It_Index != StopFreeSCCToInstsPairVect.size()) {
				errs() << "CURRENT INDEX: " << Index << "\n";
				RemovalIndex = It_Index;
				if(It_Index < Index) {
					errs() << "MORE INDEX\n";
					RemovalIndex = Index;
					MergeIndex = It_Index;
				}
				MergeSCCPairs(MergeIndex, RemovalIndex);
				SCCsMerged = true;
				errs() << "MERGE   INDEX: " << MergeIndex << "\n";
				errs() << "REMOVAL INDEX: " << RemovalIndex << "\n";
			}
		}

	// We can also merge the SCCs with no writes with SCCs with no loops
	// (single block exits to the current SCCs).
		if(BasicBlock *BB = SCCIterator.getSCCExit()) {
			if(find(BBWithFirstSerialInsts, BB) != BBWithFirstSerialInsts.end()) {
				SCC_Iterator<Function *> ExitSCC = BlockToSCCMap[BB];
				unsigned RemovalIndex = 0;
				for(; RemovalIndex != SCCToInstsPairVect.size(); RemovalIndex++) {
					if(ExitSCC == std::get<0>(SCCToInstsPairVect[RemovalIndex])) {
						auto &SCCToInstsRemovePair = SCCToInstsPairVect[RemovalIndex];
						SerialInstsSet<> &RemoveSF = std::get<1>(SCCToInstsRemovePair);
						auto &SCCToInstsMergePair = StopFreeSCCToInstsPairVect[MergeIndex];
						SerialInstsSet<> &MergeSF = std::get<1>(SCCToInstsMergePair);

					// Merge serial persist instructions sets and  remove the remove index element
						for(auto &I : RemoveSF)
							MergeSF.push_back(I);
						SCCToInstsPairVect.erase(SCCToInstsPairVect.begin() + RemovalIndex);
						break;
					}
				}
			}
		}

	// Finish up the merging process
		if(SCCsMerged) {
			errs() << "====SCCs MERGED\n";
			errs() << "====MERGE   INDEX: " << MergeIndex << "\n";
			errs() << "====REMOVAL INDEX: " << RemovalIndex << "\n";
			errs() << "====SIZE: " << StopFreeSCCToInstsPairVect.size() << "\n";

		// Update the scc iterator for the merged SCCs
			auto &SCCToInstsRemovePair = StopFreeSCCToInstsPairVect[RemovalIndex];
			auto &SCCToInstsMergePair = StopFreeSCCToInstsPairVect[MergeIndex];
			auto &RemoveSCCIterator = std::get<0>(SCCToInstsRemovePair);
			SerialInstsSet<> &MergeSF = std::get<1>(SCCToInstsMergePair);

		// What SCC iterator we assign to this combination depends on  we have
		// already covered the removed index or not. If not, we use that instead we
		// leave the current index alone.
			if(RemovalIndex > MergeIndex) {
				StopFreeSCCToInstsPairVect[MergeIndex] =
									std::make_pair(RemoveSCCIterator, MergeSF);
			}

			for(auto *BB : *RemoveSCCIterator) {
				errs() << "REMOVE ITERATOR BLOCK: ";
				BB->printAsOperand(errs(), false);
				errs() << "\n";
			}

		// Remove the element if the SCCs have been merged
			errs() << "";
			StopFreeSCCToInstsPairVect.erase(
					StopFreeSCCToInstsPairVect.begin() + RemovalIndex);

			errs() << "----SIZE: " << StopFreeSCCToInstsPairVect.size() << "\n";

		// We decrement this index always because if RemovalIndex > MergeIndex, we use
		// a new SCC iterator of the predecessor of the current SCC that we would need
		// to deal with. And if RemovalIndex < MergeIndex, we must have covered the
		// predecessor and current SCC, however we also removed an already convered SCC
		// so the index of current SCC reduces by one. Moreoever, index will be incremented
		// again in the top of this loop.
			Index--;
		}
	}
}

// This function seperates the persist operations if they happen to be in condblock sets
// because we cannot statically analyze the perisist operations in condblock sets, however,
// they can be analyzed dynamically.
void SeparateAcrossSCCsAndCondBlockSets(
					SCCToInstsPairVectTy &StopFreeSCCToInstsPairVect,
					DenseMap<BasicBlock *, SCC_Iterator<Function *>> &BlockToSCCMap,
					GenCondBlockSetLoopInfo &GI) {
// Set to record wehther a loop contains condblock sets with persist instructions
	DenseSet<GenLoop *> LoopToCondBlockSetStatusSet;

// Set to record wehther a loop contains condblock sets with persist instructions
	SmallVector<SCC_Iterator<Function *>, 4> SCCToCondBlockSetStatusSet;

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
	auto SeparateInstructionSetsAcrossCondBlockSets =
										[&](SCCToInstsPairTy &SCCToInstsPair,
											SCCToInstsPairVectTy &PairVect) {
	// If there is only one instruction, just move on
		auto &SF = std::get<1>(SCCToInstsPair);
		if(SF.size() == 1)
			return;

		GenCondBlockSet *CBS = GI.getCondBlockSetFor(SF[0]->getParent());
		SerialInstsSet<>::iterator SFI = SF.begin();
		for(Instruction *FI : SF) {
			errs() << "PARENT: ";
			FI->getParent()->printAsOperand(errs(), false);
			errs() << " ";
			errs() << "FLUSH: ";
			FI->print(errs());
			errs() << "\n";
			GenCondBlockSet *GCBS = GI.getCondBlockSetFor(FI->getParent());

		// Deal with condblock sets
			if(GCBS != CBS) {
			// Separate the instruction sets
				errs() << "HERE\n";
				SeparateInstructionSets(SCCToInstsPair, SFI, PairVect);

			// Now we also record the loops these condblock sets happen
			// to be in.
				LoopToCondBlockSetStatusSet.insert(GI.getLoopFor(FI->getParent()));
				SCCToCondBlockSetStatusSet.push_back(BlockToSCCMap[FI->getParent()]);
				return;
			}
			SFI++;
		}
	};

	auto SeparateInstructionSetsAcrossLoopsAndSCCs = [&](SCCToInstsPairTy &SCCToInstsPair,
												   SCCToInstsPairVectTy &PairVect) {
	// If there is only one instruction, just move on
		auto &SF = std::get<1>(SCCToInstsPair);
		if(SF.size() == 1)
			return;

		GenLoop *L = GI.getLoopFor(SF[0]->getParent());
		SCC_Iterator<Function *> SCCIt = BlockToSCCMap[SF[0]->getParent()];
		SerialInstsSet<>::iterator SFI = SF.begin();
		for(Instruction *FI : SF) {
			errs() << "PARENT: ";
			FI->getParent()->printAsOperand(errs(), false);
			errs() << " ";
			errs() << "FLUSH: ";
			FI->print(errs());
			errs() << "\n";
			GenLoop *GL = GI.getLoopFor(FI->getParent());
			SCC_Iterator<Function *> SCCIterator = BlockToSCCMap[FI->getParent()];

		// Deal with the SCCs
			if(SCCIterator != SCCIt) {
				if(find(SCCToCondBlockSetStatusSet, SCCIterator) != SCCToCondBlockSetStatusSet.end()
				|| find(SCCToCondBlockSetStatusSet, SCCIt) != SCCToCondBlockSetStatusSet.end()) {
					SeparateInstructionSets(SCCToInstsPair, SFI, PairVect);
					return;
				}
				SCCIt = SCCIterator;
				SFI++;
				continue;
			}

		// Deal with loops
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
	for(unsigned Index = 0; Index != StopFreeSCCToInstsPairVect.size(); ++Index) {
		auto &SCCToInstsPair = StopFreeSCCToInstsPairVect[Index];
		errs() << "ITERATING\n";
		SeparateInstructionSetsAcrossCondBlockSets(SCCToInstsPair, StopFreeSCCToInstsPairVect);
		errs() << "--SIZE: " << StopFreeSCCToInstsPairVect.size() << "\n";
	}

// Iterate over the serial persist instructions sets again
	for(unsigned Index = 0; Index != StopFreeSCCToInstsPairVect.size(); ++Index) {
		auto &SCCToInstsPair = StopFreeSCCToInstsPairVect[Index];
		errs() << "ITERATING\n";
		SeparateInstructionSetsAcrossLoopsAndSCCs(SCCToInstsPair, StopFreeSCCToInstsPairVect);
		errs() << "--SIZE: " << StopFreeSCCToInstsPairVect.size() << "\n";
	}
}
