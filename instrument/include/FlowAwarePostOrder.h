//======================= Post Order Implementation  ==========================//
//
// Implementation for a post order arrangement with awareness of control flow.
// This places restrictions on the usual post order traversal of a graph.
//
//===============================================================================//


#ifndef FLOW_AWARE_POST_ORDER_IT_H__
#define FLOW_AWARE_POST_ORDER_IT_H__

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

//namespace llvm {

//template<class BlockT, class GraphNodeT,  class GenInfoT>
//std::vector<BlockT *>
//FlowAwarePostOrderVect(const GraphNodeT &G, const GenInfoT &GI);

// A wrapper function for templated function
std::vector<BasicBlock *> FlowAwarePostOrder(const DomTreeNodeBase<BasicBlock> *G,
											 											 const GenCondBlockSetLoopInfo &GI);

// A wrapper function for pre-order
std::vector<BasicBlock *> FlowAwarePreOrder(const DomTreeNodeBase<BasicBlock> *G,
		 	 	 	 	 	 	 	 	 												const GenCondBlockSetLoopInfo &GI);

//}  // end of namespace llvm

#endif  // FLOW_AWARE_POST_ORDER_IT_H__
