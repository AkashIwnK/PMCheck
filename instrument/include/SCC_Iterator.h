
#ifndef PM_SCC_ITERATOR_H_
#define PM_SCC_ITERATOR_H_

#define _GNU_SOURCE
#include <vector>
#include <iostream>
#include <cassert>
#include <cstddef>
#include <iterator>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/iterator.h"

namespace llvm {

// We implement the SCC iterator same way LLVM does but we add more funtionality to it
template <class GraphT, class GT = GraphTraits<GraphT>>
class SCC_Iterator : public iterator_facade_base<
					  SCC_Iterator<GraphT, GT>, std::forward_iterator_tag,
					  const std::vector<typename GT::NodeRef>, ptrdiff_t> {
	using NodeRef = typename GT::NodeRef;
	using ChildItTy = typename GT::ChildIteratorType;
	using SccTy = std::vector<NodeRef>;
	using reference = typename SCC_Iterator::reference;

// Element of VisitStack during DFS.
	struct StackElement {
		NodeRef Node;         //< The current node pointer.
		ChildItTy NextChild;  //< The next child, modified inplace during DFS.
		unsigned MinVisited;  //< Minimum uplink value of all children of Node.

		StackElement(NodeRef Node, const ChildItTy &Child, unsigned Min)
		: Node(Node), NextChild(Child), MinVisited(Min) {}

		bool operator==(const StackElement &Other) const {
		return Node == Other.Node &&
		NextChild == Other.NextChild &&
		MinVisited == Other.MinVisited;
	}
};

// The visit counters used to detect when a complete SCC is on the stack.
// visitNum is the global counter.
// nodeVisitNumbers are per-node visit numbers, also used as DFS flags.
	unsigned visitNum;
	DenseMap<NodeRef, unsigned> nodeVisitNumbers;

// Stack holding nodes of the SCC.
	std::vector<NodeRef> SCCNodeStack;

// The current SCC, retrieved using operator*().
	SccTy CurrentSCC;

// DFS stack, Used to maintain the ordering.  The top contains the current
// node, the next child to visit, and the minimum uplink value of all child
	std::vector<StackElement> VisitStack;

// Get exits
	std::vector<NodeRef> ;

// A single "visit" within the non-recursive DFS traversal.
	void DFSVisitOne(NodeRef N);

// The stack-based DFS traversal; defined below.
	void DFSVisitChildren();

// Compute the next SCC using the DFS traversal.
	void GetNextSCC();

	SCC_Iterator(NodeRef entryN) : visitNum(0) {
		DFSVisitOne(entryN);
		GetNextSCC();
	}

 public:
// End is when the DFS stack is empty.
	SCC_Iterator() = default;

	static SCC_Iterator begin(const GraphT &G) {
		return SCC_Iterator(GT::getEntryNode(G));
	}

	static SCC_Iterator end(const GraphT &) {
		return SCC_Iterator();
	}

// Direct loop termination test which is more efficient than
// comparison with \c end().
	bool isAtEnd() const {
		assert(!CurrentSCC.empty() || VisitStack.empty());
		return CurrentSCC.empty();
	}

	bool operator==(const SCC_Iterator &x) const {
		return VisitStack == x.VisitStack && CurrentSCC == x.CurrentSCC;
	}

	SCC_Iterator &operator++() {
		GetNextSCC();
		return *this;
	}

	reference operator*() const {
		assert(!CurrentSCC.empty() && "Dereferencing END SCC iterator!");
		return CurrentSCC;
	}

	SCC_Iterator operator=(const SCC_Iterator &x) {
		VisitStack = x.VisitStack;
		CurrentSCC = x.CurrentSCC;
		SCCNodeStack = x.SCCNodeStack;
		visitNum = x.visitNum;
		nodeVisitNumbers = x.nodeVisitNumbers;
		return *this;
	}

	void printSCC() const {
		errs() << "PRINTING SCC:\n";
		for(auto &Node : CurrentSCC) {
			Node->printAsOperand(errs(), false);
			errs() << "\n";
		}
		errs() << "+++++++++++++\n";
	}

// This informs the \c SCC_Iterator that the specified \c Old node
// has been deleted, and \c New is to be used in its place.
	void ReplaceNode(NodeRef Old, NodeRef New) {
		assert(nodeVisitNumbers.count(Old) && "Old not in SCC_Iterator?");
		nodeVisitNumbers[New] = nodeVisitNumbers[Old];
		nodeVisitNumbers.erase(Old);
	}

	bool isInSCC(NodeRef N) const {
		for(auto &Node : CurrentSCC) {
			if(N == Node)
				return true;
		}
		return false;
	}

	SccTy getSCCExits() const {
		SccTy Exits;
		for(auto &Node : CurrentSCC) {
			for(auto ChildNode : children<NodeRef>(Node)) {
				if(!isInSCC(ChildNode))
					Exits.push_back(ChildNode);
			}
		}
		assert(Exits.size() && "Number of SCC exits cannot be zero.");
		return Exits;
	}

	bool hasMultipleExits() const {
		return getSCCExits().size() != 1;
	}

	SccTy getSCCPredecessors() const {
		SccTy Preds;
		for(auto &Node : CurrentSCC) {
			for(auto PredNode : children<Inverse<NodeRef>>(Node)) {
				if(!isInSCC(PredNode)) {
					errs() << "================\n";
					PredNode->printAsOperand(errs(), false);
					errs() << "\n================\n";
					Preds.push_back(PredNode);
				}
			}
		}
		return Preds;
	}

	bool hasMultiplePredecessors() const {
		SccTy Preds = getSCCPredecessors();
		return (Preds.size() && Preds.size() != 1);
	}

	NodeRef getSCCPredecessor() const {
		SccTy Preds = getSCCPredecessors();
		if(Preds.size() == 1)
			return Preds[0];
		return nullptr;
	}

	NodeRef getSCCExit() const {
		SccTy Exits = getSCCExits();
		if(Exits.size() == 1)
			return Exits[0];
		return nullptr;
	}

// Test if the current SCC has a loop.
// If the SCC has more than one node, this is trivially true.  If not, it may
// still contain a loop if the node has an edge back to itself.
	bool hasLoop() const {
		assert(!CurrentSCC.empty() && "Dereferencing END SCC iterator!");
		if(CurrentSCC.size() > 1)
			return true;

		NodeRef N = CurrentSCC.front();
		for(ChildItTy CI = GT::child_begin(N), CE = GT::child_end(N); CI != CE; ++CI) {
			if(*CI == N)
				return true;
		}
		return false;
	}
};


template <class GraphT, class GT>
void SCC_Iterator<GraphT, GT>::DFSVisitOne(NodeRef N) {
	++visitNum;
	nodeVisitNumbers[N] = visitNum;
	SCCNodeStack.push_back(N);
	VisitStack.push_back(StackElement(N, GT::child_begin(N), visitNum));
}

template <class GraphT, class GT>
void SCC_Iterator<GraphT, GT>::DFSVisitChildren() {
	assert(!VisitStack.empty());
	while (VisitStack.back().NextChild != GT::child_end(VisitStack.back().Node)) {
	// TOS has at least one more child so continue DFS
		NodeRef childN = *VisitStack.back().NextChild++;
		typename DenseMap<NodeRef, unsigned>::iterator Visited =
									nodeVisitNumbers.find(childN);
		if (Visited == nodeVisitNumbers.end()) {
		// this node has never been seen.
			DFSVisitOne(childN);
			continue;
		}

		unsigned childNum = Visited->second;
		if (VisitStack.back().MinVisited > childNum)
			VisitStack.back().MinVisited = childNum;
	}
}

template <class GraphT, class GT> void SCC_Iterator<GraphT, GT>::GetNextSCC() {
	CurrentSCC.clear(); // Prepare to compute the next SCC
	while (!VisitStack.empty()) {
		DFSVisitChildren();

	// Pop the leaf on top of the VisitStack.
		NodeRef visitingN = VisitStack.back().Node;
		unsigned minVisitNum = VisitStack.back().MinVisited;
		assert(VisitStack.back().NextChild == GT::child_end(visitingN));
		VisitStack.pop_back();

	// Propagate MinVisitNum to parent so we can detect the SCC starting node.
		if(!VisitStack.empty() && VisitStack.back().MinVisited > minVisitNum)
			VisitStack.back().MinVisited = minVisitNum;

		if (minVisitNum != nodeVisitNumbers[visitingN])
			continue;

	// A full SCC is on the SCCNodeStack!  It includes all nodes below
	// visitingN on the stack.  Copy those nodes to CurrentSCC,
	// reset their minVisit values, and return (this suspends
	// the DFS traversal till the next ++).
		do {
			CurrentSCC.push_back(SCCNodeStack.back());
			SCCNodeStack.pop_back();
			nodeVisitNumbers[CurrentSCC.back()] = ~0U;
		} while(CurrentSCC.back() != visitingN);

		return;
	}
}

// Construct the begin iterator for a deduced graph type T.
template <class T> SCC_Iterator<T> scc_begin(const T &G) {
	return SCC_Iterator<T>::begin(G);
}

// Construct the end iterator for a deduced graph type T.
template <class T> SCC_Iterator<T> scc_end(const T &G) {
	return SCC_Iterator<T>::end(G);
}

}

#endif // PM_SCC_ITERATOR_H_
