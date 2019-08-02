//======================= Post Order Implementation  ==========================//
//
// Implementation for a post order arrangement with awareness of control flow.
// This places restrictions on the usual post order traversal of a graph.
//
//===============================================================================//

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/ADT/STLExtras.h"

#include "CondBlockBase.h"
#include "CondBlockBaseImpl.h"
#include "GenCondInfo.h"
#include "GenCondInfoImpl.h"

using namespace llvm;

template<class BlockT, class GraphNodeT,  class GenInfoT>
std::vector<BlockT *>
FlowAwarePostOrderVect(const GraphNodeT &G, const GenInfoT &GI) {
// Vector to order the basic blocks in post-order form
	std::vector<BlockT *> PostOrderedBBs;

// Map every condblock to the tail of the condblock set it is contained in
	DenseMap<BlockT *, SmallVector<BlockT *, 16>> TailToBlocksMap;

// Keep track of the head and tail top-level of condblock set
	BlockT *OuterCBSHeader = nullptr;
	BlockT *OuterCBSTail = nullptr;

// Function to add a condblock set and all its sub-condblock sets
// into the vector with post-ordered blocks.
	auto AddPostOrderedBlocks = [&](BlockT *StartTail) {
	// Add the start tail
		errs() << "TAIL BLOCK ADDED: ";
		StartTail->printAsOperand(errs(), false);
		errs() << "\n";
		PostOrderedBBs.push_back(StartTail);

	// Put all the blocks in the post order blocks vector recursively
		typedef std::pair<BlockT *, unsigned> TailToIndexPairTy;
		SmallVector<TailToIndexPairTy, 8> TailsToIndexPairStack;
		TailsToIndexPairStack.push_back(std::make_pair(StartTail, 0));
		while(!TailsToIndexPairStack.empty()) {
			BlockT *Tail = std::get<0>(TailsToIndexPairStack.back());
			unsigned Index = std::get<1>(TailsToIndexPairStack.back());
			TailsToIndexPairStack.pop_back();
			errs() << "TAIL: ";
			Tail->printAsOperand(errs(), false);
			errs() << "\n";
			for(; Index != TailToBlocksMap[Tail].size(); Index++) {
				BlockT *Block = TailToBlocksMap[Tail][Index];
				PostOrderedBBs.push_back(Block);
				errs() << "BLOCK ADDED: ";
				Block->printAsOperand(errs(), false);
				errs() << "\n";
				if(!GI.isCondBlockSetTail(Block)) {
					errs() << "COND BLOCK FOUND\n";
					continue;
				}

			// This block is a tail. So check map again.
				errs() << "TAIL FOUND\n";
				TailsToIndexPairStack.push_back(std::make_pair(Tail, Index + 1));
				TailsToIndexPairStack.push_back(std::make_pair(Block, 0));
				break;
			}
			errs() << "LOOP BOTTOM\n";
		}
	};

// Iterate graph in post-order
	for(auto &Node : post_order(G)) {
		BasicBlock *BB = Node->getBlock();
		BB->printAsOperand(errs(), false);
		errs() << "\n";

	// Reached the top of what we were supposed to iterate over
		if(BB == G->getBlock()) {
			errs() << "ROOT REACHED\n";
		// If this block is a condblock set header, then
			if(GI.isCondBlockSetHeader(BB)) {
			// Get the tail corresponding to this condblock set
				auto *CBS = GI.getCondBlockSetFor(BB->getTerminator()->getSuccessor(0));

			// Consider the case in which we deal with a condblock set with triangle case
				if(!CBS || CBS->getHeader() != BB)
					CBS = GI.getCondBlockSetFor(BB->getTerminator()->getSuccessor(1));
				errs() << "BB IS CONDBLOCK SET HEADER\n";
				BB->getTerminator()->getSuccessor(0)->printAsOperand(errs(), false);
				errs() << "\n";
				if(BlockT *Tail = const_cast<BlockT *>(CBS->getTail()))
					AddPostOrderedBlocks(Tail);
			}

		// Add the current block
			errs() << "--BLOCK ADDED: ";
			BB->printAsOperand(errs(), false);
			errs() << "\n";
			PostOrderedBBs.push_back(BB);
			continue;
		}

		if(auto *CBS = GI.getCondBlockSetFor(BB)) {
			if(BlockT *Tail = const_cast<BlockT *>(CBS->getTail())) {
				TailToBlocksMap[Tail].push_back(BB);
				errs() << "BLOCK QUEUED\n";
				continue;
			} else {
			// This is a condblock set without a tail. Check to see if the
			// condblock set has a parent condblock set that has a tail.
				auto *ParentCBS = CBS->getParentCondBlockSet();
				while(ParentCBS && !ParentCBS->getTail())
					ParentCBS = ParentCBS->getParentCondBlockSet();
				if(ParentCBS) {
					Tail = const_cast<BlockT *>(ParentCBS->getTail());
					TailToBlocksMap[Tail].push_back(BB);
					errs() << "BLOCK QUEUED\n";
					continue;
				}
			}
		} else {
		// If the current block is a tail, track the outermost tail
		// and the header of the condblock set.
			if(GI.isCondBlockSetTail(BB)) {
			// This works because for a top-level condblock set, head always
			// dominates its tail.
				OuterCBSHeader = GI.getHeaderForTopLevelTail(BB);
				OuterCBSTail = BB;
				errs() << "TAIL DETECTED\n";
				continue;
			}

		// Put all the condblocks once the outermost head of condblock sets
		// is discovered.
			if(OuterCBSHeader == BB) {
			// Add all the condblock sets to post-order list now
				AddPostOrderedBlocks(OuterCBSTail);

			// Reset pointers for other condblock sets
				OuterCBSHeader = OuterCBSTail = nullptr;
			}
		}

		errs() << "--BLOCK ADDED: ";
		BB->printAsOperand(errs(), false);
		errs() << "\n";
		PostOrderedBBs.push_back(BB);
	}

	errs() << "PRINTING POST ORDER: ";
	for(auto *BB : PostOrderedBBs) {
		BB->printAsOperand(errs(), false);
		errs() << "\n";
	}
	errs() << "=====================\n";

	return PostOrderedBBs;
}

// A wrapper function for templated function
std::vector<BasicBlock *> FlowAwarePostOrder(const DomTreeNodeBase<BasicBlock> *G,
											 const GenCondBlockSetLoopInfo &GI) {
	return FlowAwarePostOrderVect<BasicBlock, DomTreeNodeBase<BasicBlock> *,
							  GenCondBlockSetLoopInfo>(
								const_cast<DomTreeNodeBase<BasicBlock> *>(G), GI);
}

// A wrapper function for pre-order
std::vector<BasicBlock *> FlowAwarePreOrder(const DomTreeNodeBase<BasicBlock> *G,
		 	 	 	 	 	 	 	 	 	const GenCondBlockSetLoopInfo &GI) {
	std::vector<BasicBlock *> PO =
			FlowAwarePostOrderVect<BasicBlock, DomTreeNodeBase<BasicBlock> *,
			GenCondBlockSetLoopInfo>(const_cast<DomTreeNodeBase<BasicBlock> *>(G), GI);
	std::reverse(PO.begin(), PO.end());
	return PO;
}
