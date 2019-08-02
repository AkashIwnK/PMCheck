//======================= Interval Tree Implementation ========================//
//
// Implementation of an AVL Interval Tree for PMCheck runtime.
//
//=============================================================================//

#ifndef INTERVAL_TREE_H_
#define INTERVAL_TREE_H_

#include <iostream>
#include <vector>
#include <set>
#include <utility>
#include <tuple>

struct IntervalNodeConcept {
	uint64_t Start;
	uint64_t End;
	uint64_t Middle;

	IntervalNodeConcept(uint64_t Start, uint64_t End) {
		this->Start = Start;
		this->End = End;
	}

	void print() const {
		std::cout << "INTERVAL: ";
		std::cout << Start << " TO " << End << "\n";
	}

	virtual void updateMiddle() = 0;
};

class ITResult {
public:
	enum OverlapResult {
		NoOverlap,
		PartialOverlap,
		CompleteOverlap,
		CompletelyPerfectOverlap,

	// This is used for search operations where a given interval
	// is found partially in some nodes but it does entirely lie
	// in an interval tree.
		PartialCompleteOverlap
	};

	struct NodeRange {
		uint64_t Start;
		uint64_t End;

		NodeRange() : Start((uint64_t)-1), End((uint64_t)-1) {}

		NodeRange(uint64_t Start, uint64_t End) {
			this->Start = Start;
			this->End = End;
		}

		bool isUnset() const {
			if(Start == (uint64_t)-1 && End == (uint64_t)-1)
				return true;
			return false;
		}

		void print() const {
			std::cout << "NODE RANGE: " << Start << " TO " << End << "\n";
		}
	};

private:
	typedef std::tuple<IntervalNodeConcept *, OverlapResult, NodeRange> NodeOverlapInfoTy;

// These results reflect the most recent changes make to the interval tree
	OverlapResult OR;
	std::vector<NodeOverlapInfoTy> NodeOverlapInfoVect;

// This result reflects how the corresponding nodes previously were right before
// some operation was performed on the interval tree. This is used especially when
// there is some partial overlap and they are updated.
	std::vector<NodeRange> PreviousNodeRangesVect;

// ITResult constructors
	ITResult(OverlapResult Result, IntervalNodeConcept *Ptr) {
		OR = Result;
		NodeOverlapInfoVect.push_back(std::make_tuple(Ptr, Result, NodeRange()));
	}

	ITResult(OverlapResult Result, IntervalNodeConcept *Ptr,
			 NodeRange State) {
		OR = Result;
		NodeOverlapInfoVect.push_back(std::make_tuple(Ptr, Result, NodeRange()));
		PreviousNodeRangesVect.push_back(State);
	}

	ITResult(OverlapResult Result,
			 std::vector<NodeOverlapInfoTy> PtrVect) {
		OR = Result;
		NodeOverlapInfoVect = PtrVect;
	}

	ITResult(OverlapResult Result,
			 std::vector<NodeOverlapInfoTy> PtrVect,
			 std::vector<NodeRange> NodesStatusVect) {
		OR = Result;
		NodeOverlapInfoVect = PtrVect;
		PreviousNodeRangesVect = NodesStatusVect;
	}

// Interval tree can access ITResult constructors
	template<bool OptimizeSearch> friend class IntervalTree;

public:
	OverlapResult getOverlapResult() const {
		return OR;
	}

	const std::vector<NodeOverlapInfoTy> &getNodesAndOverlapResults() const {
		return NodeOverlapInfoVect;
	}

	IntervalNodeConcept *getNode(unsigned Index) const {
		if(Index < NodeOverlapInfoVect.size())
			return std::get<0>(NodeOverlapInfoVect[Index]);
		return nullptr;
	}

	const std::vector<NodeRange> &getPreviousNodeRanges() const {
		return PreviousNodeRangesVect;
	}

	unsigned getNumOverlapNodes() const {
		return NodeOverlapInfoVect.size();
	}

	NodeRange getPreviousNodeRange(unsigned Index) const {
		if(Index < PreviousNodeRangesVect.size())
			return PreviousNodeRangesVect[Index];
		return NodeRange();
	}

	unsigned getPreviousNodeRangeSize() const {
		return PreviousNodeRangesVect.size();
	}
};

template<bool OptimizeSearch>
class IntervalTree {
	struct IntervalNode : public IntervalNodeConcept {
		IntervalNode *Parent;
		IntervalNode *Left;
		IntervalNode *Right;

		void updateMiddle() override {
			Middle = IntervalTree::getMiddle(IntervalNodeConcept::Start, IntervalNodeConcept::End);
		}

		IntervalNode(uint64_t Start, uint64_t Len) :
						IntervalNodeConcept(Start, Len) , Parent(nullptr),
						Left(nullptr), Right(nullptr) {
			updateMiddle();
		}

		void reset() {
			Parent = Left = Right = nullptr;
		}

		void print() const {
			std::cout << "\n----------------------\n";
			std::cout << "PRINTING NODE\n";
			IntervalNodeConcept::print();
			if(Parent) {
				std::cout << "PARENT ";
				((IntervalNodeConcept *)Parent)->print();
			} else {
				std::cout << "NO PARENT NODE\n";
			}
			if(Left) {
				std::cout << "LEFT ";
				((IntervalNodeConcept *)Left)->print();
			} else {
				std::cout << "NO LEFT NODE\n";
			}
			if(Right) {
				std::cout << "RIGHT ";
				((IntervalNodeConcept *)Right)->print();
			} else {
				std::cout << "NO RIGHT NODE\n";
			}
			std::cout << "------------------------\n";
		}
	};

	static uint64_t getMiddle(uint64_t Start, uint64_t End) {
	// Use arithmetic mean
		return (Start + End) >> 1;
	}

// Root of the interval tree
	IntervalNode *Root;

// Vector of all nodes in the interval tree. This makes accessing
// all of them easy for operations like destructing the tree, etc.
	std::set<IntervalNode *> NodesSet;

	IntervalNode *insertNode(IntervalNode *Node);

	void removeNode(IntervalNode *Node);

	IntervalNode *internalSearch(uint64_t Start, uint64_t End) const;

	ITResult detailedInternalSearch(uint64_t Start, uint64_t End) const;

	ITResult detailedInternalRemove(uint64_t Start, uint64_t End,
									bool AllowPartialRemoval = true);

	signed nodeHeight(IntervalNode *Node);

	signed heightDiff(IntervalNode *Node);

	IntervalNode *rightRightRotate(IntervalNode *Node);

	IntervalNode *leftLeftRotate(IntervalNode *Node);

	IntervalNode *leftRightRotate(IntervalNode *Node);

	IntervalNode *rightLeftRotate(IntervalNode *Node);

	IntervalNode *balanceTree(IntervalNode *Node);

public:
	IntervalTree() : Root(nullptr), NodesSet() {}

	~IntervalTree() {
		clear();
	}

	ITResult insert(uint64_t Start, uint64_t End);

	template<bool SearchInParts>
	bool remove(uint64_t Start, uint64_t End) {
		std::cout << "REMOVING RANGE: " << Start << " TO " << End << "\n";
		if(!SearchInParts) {
			auto *Node = internalSearch(Start, End);
			if(!Node)
				return false;
			removeNode(Node);
			NodesSet.erase(Node);
			delete Node;
			return true;
		} else {
			auto Result = detailedInternalRemove(Start, End);
			if(Result.getOverlapResult() == ITResult::NoOverlap) {
				std::cout << "NO OVERLAP\n";
				return false;
			}
		}
		return true;
	}

	template<bool SearchInParts>
	bool search(uint64_t Start, uint64_t End) const {
		if(SearchInParts) {
			auto Result = detailedInternalSearch(Start, End);
			if(Result == ITResult::PartialCompleteOverlap
			|| Result == ITResult::CompleteOverlap) {
				return true;
			}
		} else {
			if(auto *Node = internalSearch(Start, End))
				return true;
		}
		return false;
	}

	ITResult getSearchDetails(uint64_t Start, uint64_t End) const {
		return detailedInternalSearch(Start, End);
	}

	ITResult getRemoveDetails(uint64_t Start, uint64_t End) const {
		return detailedInternalRemove(Start, End);
	}

	bool empty() const {
		return Root == nullptr;
	}

	void clear() {
	// Iterate over NodesSet and erase all the nodes from the tree
		for(auto *Node : NodesSet)
			delete Node;
		NodesSet.clear();
		Root = nullptr;
	}

	uint64_t size() const {
		return NodesSet.size();
	}

	std::pair<uint64_t, uint64_t> getRootInterval() const {
		if(!Root)
			return std::pair<uint64_t, uint64_t>();
		return std::make_pair(Root->Start, Root->End);
	}

	std::vector<std::pair<uint64_t, uint64_t>> getIntervals() const {
		std::vector<std::pair<uint64_t, uint64_t>> IntervalsVect;
		for(auto *Node : NodesSet)
			IntervalsVect.push_back(std::make_pair(Node->Start, Node->End));
		return IntervalsVect;
	}

	void print() const {
		std::cout << "\nPRINTING INTERVAL TREE\n";
		std::cout << "ROOT: ";
		Root->print();
		for(auto *Node : NodesSet) {
			if(Node != Root)
				Node->print();
		}
		std::cout << "----------------------\n";
	}
};

template<bool OptimizeSearch>
signed IntervalTree<OptimizeSearch>::nodeHeight(IntervalNode *Node) {
	if(!Node) {
		auto LeftHeight = nodeHeight(Node->Left);
		auto RightHeight = nodeHeight(Node->Right);
		if(LeftHeight > RightHeight)
			return (LeftHeight + 1);
		return (RightHeight + 1);
	}
	return 0;
}

template<bool OptimizeSearch>
signed IntervalTree<OptimizeSearch>::heightDiff(IntervalNode *Node) {
	return (nodeHeight(Node->Left) - nodeHeight(Node->Right));
}

template<bool OptimizeSearch>
typename IntervalTree<OptimizeSearch>::IntervalNode *
IntervalTree<OptimizeSearch>::rightRightRotate(IntervalNode *Node) {
	IntervalNode *MoveNode = Node->Right;
	Node->Right = MoveNode->Left;
	MoveNode->Left->Parent = Node;
	MoveNode->Left = Node;
	Node->Parent = MoveNode;
	std::cout<<"Right-Right Rotation";
	return MoveNode;
}

template<bool OptimizeSearch>
typename IntervalTree<OptimizeSearch>::IntervalNode *
IntervalTree<OptimizeSearch>::leftLeftRotate(IntervalNode *Node) {
	IntervalNode *MoveNode = Node->Left;
	Node->Left = MoveNode->Right;
	MoveNode->Right->Parent = Node;
	MoveNode->Right = Node;
	Node->Parent = MoveNode;
	std::cout<<"Left-Left Rotation";
	return MoveNode;
}

template<bool OptimizeSearch>
typename IntervalTree<OptimizeSearch>::IntervalNode *
IntervalTree<OptimizeSearch>::leftRightRotate(IntervalNode *Node) {
	IntervalNode *MoveNode = Node->Left;
	Node->Left = rightRightRotate(MoveNode);
	std::cout<<"Left-Right Rotation";
	return leftLeftRotate(Node);
}

template<bool OptimizeSearch>
typename IntervalTree<OptimizeSearch>::IntervalNode *
IntervalTree<OptimizeSearch>::rightLeftRotate(IntervalNode *Node) {
	IntervalNode *MoveNode = Node->Right;
	Node->Right = leftLeftRotate(MoveNode);
	std::cout<<"Right-Left Rotation";
	return rightRightRotate(Node);
}

// Function to balance a binary tree
template<bool OptimizeSearch>
typename IntervalTree<OptimizeSearch>::IntervalNode *
IntervalTree<OptimizeSearch>::balanceTree(IntervalNode *Node) {
	auto BalFactor = heightDiff(Node);
	auto *BalancedNode = Node;
	if(BalFactor > 1) {
		if(heightDiff(Node->Left) > 0)
			BalancedNode = leftLeftRotate(Node);
		else
			BalancedNode = leftRightRotate(Node);
	} else if(BalFactor < -1) {
		if(heightDiff(Node->Right) > 0)
			BalancedNode = rightLeftRotate(Node);
		else
			BalancedNode = rightRightRotate(Node);
	}
	return BalancedNode;
}

// This is similar to inserting a node in a binary tree
template<bool OptimizeSearch>
typename IntervalTree<OptimizeSearch>::IntervalNode *
IntervalTree<OptimizeSearch>::insertNode(IntervalNode *Node) {
	std::cout << "====================INSERTING NODE:\n";
	Node->print();
	std::cout << "==================== PRINTING TREE:";
	this->print();
	if(!Root) {
		Root = Node;
		return Node;
	}

	auto *CurNode = Root;
	while(CurNode) {
	// Look for complete overlaps
		if(OptimizeSearch) {
			std::cout << "OPTIMIZED SEARCH ON\n";
			if(Node->Start >= CurNode->Start
			&& Node->End < CurNode->End) {
			// Found a node that we overlap with, so return
				std::cout << "COMPLETE OVERLAP FOUND\n";
				return CurNode;
			}
			if(Node->Start == CurNode->End) {
				std::cout << "APPEND\n";
				CurNode->End = Node->End;
				CurNode->updateMiddle();

			// Check if this node needs to be moved
				if(CurNode->Right && CurNode->Middle > CurNode->Right->Middle) {
					removeNode(CurNode);
					auto *InsertedNode = insertNode(CurNode);
					if(InsertedNode != CurNode) {
					// This means a node was found to have been in the tree already
					// so we do not need the current node anymore.
						NodesSet.erase(CurNode);
						delete CurNode;
						CurNode = InsertedNode;
					} else {
					// Balance the tree
						//CurNode = balanceTree(CurNode);
					}
				}
				return CurNode;
			}
			if(Node->End == CurNode->Start) {
				std::cout << "PREPEND\n";
				CurNode->Start = Node->Start;
				CurNode->updateMiddle();

			// Check if this node needs to be moved
				if(CurNode->Left && CurNode->Middle <= CurNode->Left->Middle) {
				// This node needs to be removed and added back
					removeNode(CurNode);
					auto *InsertedNode = insertNode(CurNode);
					if(InsertedNode != CurNode) {
					// This means a node was found to have been in the tree already
					// so we do not need the current node anymore.
						NodesSet.erase(CurNode);
						delete CurNode;
						CurNode = InsertedNode;
					} else {
					// Balance the tree
						//CurNode = balanceTree(CurNode);
					}
				}
				return CurNode;
			}
			if(Node->Start < CurNode->Start && Node->End > CurNode->End) {
			// Coalesce the nodes by merging it with existing node, removing it and
			// reinserting it into the tree.
				CurNode->Start = Node->Start;
				CurNode->End = Node->End;
				CurNode->updateMiddle();

			// Check if this node needs to be moved
				if((CurNode->Left && CurNode->Middle <= CurNode->Left->Middle)
				|| (CurNode->Right && CurNode->Middle > CurNode->Right->Middle)) {
					removeNode(CurNode);
					auto *InsertedNode = insertNode(CurNode);
					if(InsertedNode != CurNode) {
					// This means a node was found to have been in the tree already
					// so we do not need the current node anymore.
						NodesSet.erase(CurNode);
						delete CurNode;
						CurNode = InsertedNode;
					} else {
					// Balance the tree
						//CurNode = balanceTree(CurNode);
					}
				}
				return CurNode;
			}
		}

		if(CurNode->Middle > Node->Middle) {
			if(!CurNode->Left) {
			// Insert here
				CurNode->Left = Node;
				Node->Parent = CurNode;
				return Node;
			}
			CurNode = CurNode->Left;
		} else {
			if(!CurNode->Right) {
			// Insert here
				CurNode->Right = Node;
				Node->Parent = CurNode;
				return Node;
			}
			CurNode = CurNode->Right;
		}
	}
	return Node; // Keep the compiler happy
}

// This is very similar to removing a node from a binary tree
template<bool OptimizeSearch>
void IntervalTree<OptimizeSearch>::removeNode(IntervalNode *Node) {
// If root does not exist, just exit
	if(!Root)
		return;

// Check if the given node is a leaf
	if(!Node->Left && !Node->Right) {
		std::cout << "REMOVING LEAF NODE\n";
		if(Node->Parent) {
			if(Node->Parent->Left == Node)
				Node->Parent->Left = nullptr;
			else
				Node->Parent->Right = nullptr;
		} else {
		// This node is root
			std::cout << "NODE TO BE REMOVED HAS NO PARENT\n";
			Root = nullptr;
		}

	// Reset the given node
		Node->reset();
		return;
	}

// The given node is not a leaf. So find the leftmost element in the
// right subtree if it exists.
	if(Node->Left && Node->Right) {
		std::cout << "NODE HAS 2 CHILDREN\n";
		IntervalNode *CurNode = Node->Right;
		while(CurNode->Left)
			CurNode = CurNode->Left;

	// Remove the current node from the tree
		if(CurNode != Node->Right) {
			if(CurNode->Parent->Left == CurNode)
				CurNode->Parent->Left = CurNode->Right;
			else
				CurNode->Parent->Right = CurNode->Right;
			if(CurNode->Right)
				CurNode->Right->Parent = CurNode->Parent;
			CurNode->Right = Node->Right;
			Node->Right->Parent = CurNode;
		}
		CurNode->Left = Node->Left;
		CurNode->Parent = Node->Parent;
		Node->Left->Parent = CurNode;

	// Now remove the node to replace the given node
		IntervalNode **CheckNode;
		if(auto *Parent = Node->Parent) {
			if(Parent->Left == Node)
				Parent->Left = CurNode;
			else
				Parent->Right = CurNode;
			CheckNode = &Parent;
		} else {
		// The given node is a root
			Root = CurNode;
			CheckNode = &Root;
		}

	// Reset the given node
		Node->reset();

	// Balance the tree
		//*CheckNode = balanceTree(*CheckNode);
		return;
	}

// In this case one child exists
	std::cout << "NODE HAS ONE CHILD\n";
	IntervalNode *SubNode;
	IntervalNode **CheckNode;
	if(Node->Right)
		SubNode = Node->Right;
	else
		SubNode = Node->Left;
	if(Node->Parent) {
		if(Node->Parent->Left == Node)
			Node->Parent->Left = SubNode;
		else
			Node->Parent->Right = SubNode;
		SubNode->Parent = Node->Parent;
		*CheckNode = Node->Parent;
	} else {
	// This node is root
		Root = SubNode;
		*CheckNode = Root;
		std::cout << "NODE TO BE REMOVED IS A ROOT\n";
		std::cout << "PRINTING ROOT:\n";
		Root->print();
	}

// Reset the given node
	Node->reset();

// Balance the tree
	//*CheckNode = balanceTree(*CheckNode);
}

// This does NOT look for partial overlaps of range
template<bool OptimizeSearch>
typename IntervalTree<OptimizeSearch>::IntervalNode *
IntervalTree<OptimizeSearch>::internalSearch(uint64_t Start, uint64_t End) const {
	std::cout << "INTERNAL SEARCH " << Start << " TO " << End << "\n";

// Find a node that completely overlaps
	uint64_t Middle = getMiddle(Start, End);
	auto *CurNode = Root;
	while(CurNode) {
		std::cout << "CHECKING NODE :";
		CurNode->print();

	// Look for complete overlap
		if(Start >= CurNode->Start
		&& End <= CurNode->End) {
			std::cout << "NODE FOUND\n";
			return CurNode;
		}

		if(CurNode->Middle > Middle)
			CurNode = CurNode->Left;
		else
			CurNode = CurNode->Right;
	}

// Node not found
	return nullptr;
}

template<bool OptimizeSearch>
ITResult IntervalTree<OptimizeSearch>::
detailedInternalRemove(uint64_t Start, uint64_t End, bool AllowPartialRemoval) {
// Search for the nodes
	auto Result = detailedInternalSearch(Start, End);
	std::cout << "INTERNAL SEARCH DONE\n";
	if(Result.getOverlapResult() == ITResult::NoOverlap
	|| (!AllowPartialRemoval && Result.getOverlapResult() == ITResult::PartialOverlap)) {
	// Nothing to remove
		std::cout << "NOTHING TO REMOVE\n";
		return Result;
	}

// Iterate over all the nodes we found overlaps with
	std::vector<std::tuple<IntervalNodeConcept *, ITResult::OverlapResult,
						   ITResult::NodeRange>> OverlapIntervalNodesVect;
	std::vector<ITResult::NodeRange> NodesStatusVect;
	for(auto NodeAndOverlapTuple : Result.getNodesAndOverlapResults()) {
	// Get the node
		IntervalNode *Node = dynamic_cast<IntervalNode *>(std::get<0>(NodeAndOverlapTuple));

		std::cout << "OVERLAP FOUND WITH: \n";
		Node->print();

	// Modify and remove it
		ITResult::NodeRange OverlapRange = std::get<2>(NodeAndOverlapTuple);
		std::cout << "OVERLAP RANGE\n";
		OverlapRange.print();
		switch(std::get<1>(NodeAndOverlapTuple)) {
			case ITResult::PartialOverlap:
				std::cout << "PARTIAL OVERLAP\n";
			// Record the previous state of the node
				NodesStatusVect.push_back(ITResult::NodeRange(Node->Start, Node->End));

			// Adjust the range
				if(OverlapRange.Start >= Node->Start && OverlapRange.Start < Node->End) {
					Node->End = OverlapRange.Start;
					Node->updateMiddle();

				// Check if this node needs to be removed and reinserted
					if(Node->Left && Node->Middle <= Node->Left->Middle) {
					// This node needs to be removed and added back
						removeNode(Node);
						auto *NewNode = insertNode(Node);
						if(NewNode != Node) {
						// This means a node was found to have been in the tree already
						// so we do not need the current node anymore.
							NodesSet.erase(Node);
							delete Node;
							Node = NewNode;
						}
					}
				} else if(OverlapRange.End > Node->Start && OverlapRange.End <= Node->End) {
					Node->Start = OverlapRange.End;
					Node->updateMiddle();

				// Check if this node needs to be removed and reinserted
					if(Node->Right && Node->Middle <= Node->Right->Middle) {
					// This node needs to be removed and added back
						removeNode(Node);
						auto *NewNode = insertNode(Node);
						if(NewNode != Node) {
						// This means a node was found to have been in the tree already
						// so we do not need the current node anymore.
							NodesSet.erase(Node);
							delete Node;
							Node = NewNode;
						}
					}
				}
				OverlapIntervalNodesVect.push_back(std::make_tuple(Node,
												   ITResult::PartialOverlap,
												   OverlapRange));
				break;

			case ITResult::CompleteOverlap:
				std::cout << "COMPLETE OVERLAP\n";
			// Record the previous state of the node
				NodesStatusVect.push_back(ITResult::NodeRange(Node->Start, Node->End));

			// Check if the interval is on either end
				if(OverlapRange.Start == Node->Start) {
					Node->Start = OverlapRange.End;
					Node->updateMiddle();

				// Check if this node needs to be removed and reinserted
					if(Node->Right && Node->Middle <= Node->Right->Middle) {
					// This node needs to be removed and added back
						removeNode(Node);
						auto *NewNode = insertNode(Node);
						if(NewNode != Node) {
						// This means a node was found to have been in the tree already
						// so we do not need the current node anymore.
							NodesSet.erase(Node);
							delete Node;
							Node = NewNode;
						}
					}
					OverlapIntervalNodesVect.push_back(std::make_tuple(Node,
													   ITResult::CompleteOverlap,
													   OverlapRange));
				} else if(OverlapRange.End == Node->End) {
					Node->End = OverlapRange.Start;
					Node->updateMiddle();

				// Check if this node needs to be removed and reinserted
					if(Node->Left && Node->Middle <= Node->Left->Middle) {
					// This node needs to be removed and added back
						removeNode(Node);
						auto *NewNode = insertNode(Node);
						if(NewNode != Node) {
						// This means a node was found to have been in the tree already
						// so we do not need the current node anymore.
							NodesSet.erase(Node);
							delete Node;
							Node = NewNode;
						}
					}
					OverlapIntervalNodesVect.push_back(std::make_tuple(Node,
									   	   	   	   	   ITResult::CompleteOverlap,
													   OverlapRange));
				} else {
					std::cout << "SPLIT NODE\n";
				// Overlap is somewhere in the middle so we split this node into two, so
				// allocate one more node. But frst adjust this existing node.
					auto OldNodeEnd = Node->End;
					Node->End = OverlapRange.Start;
					Node->updateMiddle();

				// Check if this node needs to be removed and reinserted
					if(Node->Left && Node->Middle <= Node->Left->Middle) {
						removeNode(Node);
						auto *InsertedNewNode = insertNode(Node);
						if(InsertedNewNode != Node) {
						// This means a node was found to have been in the tree already
						// so we do not need the current node anymore.
							NodesSet.erase(Node);
							delete Node;
							Node = InsertedNewNode;
						}
					}

				// Now allocate the new node
					auto *NewNode = new IntervalNode(OverlapRange.End, OldNodeEnd);
					std::cout << "INSERTING RANGE: " << OverlapRange.End << " TO " << OldNodeEnd << "\n";
					auto *InsertedNewNode = insertNode(NewNode);
					if(InsertedNewNode != NewNode) {
						std::cout << "DELETE NEW NODE\n";
					// This means a node was found to have been in the tree already
					// so we do not need the current node anymore.
						delete NewNode;
						NewNode = InsertedNewNode;
					} else {
					// Add this new node to the set
						NodesSet.insert(NewNode);
					}

				// Add both the nodes to the result
					OverlapIntervalNodesVect.push_back(std::make_tuple(Node,
									   ITResult::CompleteOverlap,
									   OverlapRange));
					OverlapIntervalNodesVect.push_back(std::make_tuple(NewNode,
									   ITResult::CompleteOverlap,
									   OverlapRange));

				// Add to node range again for the newly allocated node
					NodesStatusVect.push_back(NodesStatusVect.back());
				}
				continue;
				//return ITResult(Result.getOverlapResult(), OverlapIntervalNodesVect, NodesStatusVect);

			case ITResult::CompletelyPerfectOverlap:
				std::cout << "COMPLETELY PERFECT OVERLAP\n";
			// Record the previous state of the node
				NodesStatusVect.push_back(ITResult::NodeRange(Node->Start, Node->End));

			// Remove the entire node and update result
				removeNode(Node);
				NodesSet.erase(Node);
				delete Node;
				OverlapIntervalNodesVect.push_back(std::make_tuple(nullptr,
								   ITResult::CompletelyPerfectOverlap,
								   OverlapRange));
				continue;
				//return ITResult(Result.getOverlapResult(), OverlapIntervalNodesVect, NodesStatusVect);
		}
	}
	return ITResult(Result.getOverlapResult(), OverlapIntervalNodesVect, NodesStatusVect);
}

// This looks for partial overlaps of given range
template<bool OptimizeSearch>
ITResult IntervalTree<OptimizeSearch>::
detailedInternalSearch(uint64_t Start, uint64_t End) const {
	std::cout << "DETAILED SEARCHING NODE\n";

	std::vector<std::tuple<IntervalNodeConcept *, ITResult::OverlapResult,
						   ITResult::NodeRange>> OverlapIntervalNodesVect;
	std::vector<std::tuple<uint64_t, uint64_t, IntervalNode *>> IntervalWorklist;
	IntervalWorklist.push_back(std::make_tuple(Start, End, Root));
	bool IntervalNotFound = false;
	while(!IntervalWorklist.empty()) {
	// Look for complete and partial overlaps
		std::cout << "LOOP BACK\n";
		auto Tuple = IntervalWorklist.back();
		IntervalWorklist.pop_back();
		auto &Start = std::get<0>(Tuple);
		auto &End = std::get<1>(Tuple);
		auto *CurNode = std::get<2>(Tuple);
		std::cout << "LOOKING AT INTERVAL: " << Start << " - " << End << "\n";
		if(!CurNode) {
			IntervalNotFound = true;
			std::cout << "INTERVAL NOT FOUND\n";
			continue;
		}

		std::cout << "SEARCHING NODE: ";
		CurNode->print();

	// Look for overlaps with existing intervals
		if(Start == CurNode->Start && End == CurNode->End) {
			std::cout << "COMPLETE OVERLAP\n";
			OverlapIntervalNodesVect.push_back(std::make_tuple(CurNode,
												ITResult::CompletelyPerfectOverlap,
												ITResult::NodeRange(Start, End)));
			if(OverlapIntervalNodesVect.size() == 1)
				return ITResult(ITResult::CompletelyPerfectOverlap, OverlapIntervalNodesVect);
			continue;
		} else if(Start >= CurNode->Start && End <= CurNode->End) {
			std::cout << "COMPLETE OVERLAP\n";
			OverlapIntervalNodesVect.push_back(std::make_tuple(CurNode, ITResult::CompleteOverlap,
												ITResult::NodeRange(Start, End)));
			if(OverlapIntervalNodesVect.size() == 1)
				return ITResult(ITResult::CompleteOverlap, OverlapIntervalNodesVect);
			continue;
		} else if(Start < CurNode->Start && End > CurNode->End) {
			std::cout << "LARGER PARTIAL OVERLAP\n";
		// Also consider the case where the given range could be larger than the node range
			OverlapIntervalNodesVect.push_back(std::make_tuple(CurNode,
															   ITResult::CompletelyPerfectOverlap,
															   ITResult::NodeRange(Start, End)));
		// Push the new intervals to the wait list
			IntervalNode *NextNode;
			auto Middle = getMiddle(CurNode->End, End);
			if(CurNode->Middle > Middle)
				NextNode = CurNode->Left;
			else
				NextNode = CurNode->Right;
			IntervalWorklist.push_back(std::make_tuple(CurNode->End, End, NextNode));
			std::cout << "NEW INTERVAL: " << CurNode->End << " - " << End << "\n";
			End = CurNode->Start;
			//IntervalWorklist.push_back(std::make_tuple(Start, End, CurNode));
			std::cout << "NEW INTERVAL: " << Start << " - " << End << "\n";
		} else if(Start >= CurNode->Start && Start < CurNode->End) {
			std::cout << "PARTIAL OVERLAP\n";
		// Looked for partial overlap. Malloc bug found here if CurNode is used as a
		// reference to the pointer and not a pure pointer. Bloody hell.
			OverlapIntervalNodesVect.push_back(std::make_tuple(CurNode, ITResult::PartialOverlap,
															ITResult::NodeRange(Start, End)));
		// Update Start and continue
			Start = CurNode->End;
			//IntervalWorklist.push_back(std::make_tuple(Start, End, CurNode));
			std::cout << "NEW START: " << Start << "\n";
		} else if(End > CurNode->Start && End <= CurNode->End) {
			std::cout << "PARTIAL OVERLAP\n";
		// Looked for partial overlap
			OverlapIntervalNodesVect.push_back(std::make_tuple(CurNode, ITResult::PartialOverlap,
															ITResult::NodeRange(Start, End)));
		// Update End and continue
			End = CurNode->Start;
			//IntervalWorklist.push_back(std::make_tuple(Start, End, CurNode));
			std::cout << "NEW END: " << End << "\n";
		}

		auto Middle = getMiddle(Start, End);
		if(CurNode->Middle > Middle)
			IntervalWorklist.push_back(std::make_tuple(Start, End, CurNode->Left));
		else
			IntervalWorklist.push_back(std::make_tuple(Start, End, CurNode->Right));
	}
	if(OverlapIntervalNodesVect.empty())
		return ITResult(ITResult::NoOverlap, OverlapIntervalNodesVect);

	if(!IntervalNotFound)
		return ITResult(ITResult::PartialCompleteOverlap, OverlapIntervalNodesVect);
	else
		return ITResult(ITResult::PartialOverlap, OverlapIntervalNodesVect);
}

// Note that the End is not inclusive in the range, unlike Start
template<bool OptimizeSearch>
ITResult IntervalTree<OptimizeSearch>::insert(uint64_t Start, uint64_t End) {
	std::cout << "INSERTING INTERVAL IN INTERVAL TREE: "  << Start << " TO " << End << "\n";
	uint64_t Middle = getMiddle(Start, End);
	std::cout << "MIDDLE: " << Middle << "\n";
	IntervalNode **CurNodeAddr = &Root;
	IntervalNode *ParentNode = nullptr;
	while(IntervalNode *CurNode = *CurNodeAddr) {
		std::cout << "PRINTING CURRENT NODE: ";
		CurNode->print();

		if(OptimizeSearch) {
		// If complete overlap is found
			if(Start == CurNode->Start && End == CurNode->End) {
				std::cout << "COMPLETE OVERLAP\n";
				return ITResult(ITResult::CompletelyPerfectOverlap, CurNode);
			}
			if(Start >= CurNode->Start && End <= CurNode->End) {
				std::cout << "COMPLETE OVERLAP\n";
				return ITResult(ITResult::CompleteOverlap, CurNode);
			}

		// No overlap but contiguous cases
			if(Start == CurNode->End) {
				std::cout << "APPEND NODE\n";
				CurNode->End = End;
				CurNode->updateMiddle();

			// Check if this node needs to be moved
				if(CurNode->Right && CurNode->Middle > CurNode->Right->Middle) {
				// This node needs to be removed and added back
					removeNode(CurNode);
					std::cout << "CURRENT NODE REMOVED\n";
					auto *Node = insertNode(CurNode);
					std::cout << "CURRENT NODE REINSERTED\n";
					if(Node != CurNode) {
					// This means a node was found to have been in the tree already
					// so we do not need the current node anymore.
						NodesSet.erase(CurNode);
						delete CurNode;
						CurNode = Node;
					}
				}
				return ITResult(ITResult::NoOverlap, CurNode);
			}
			if(End == CurNode->Start) {
				std::cout << "PREPEND NODE\n";
				CurNode->Start = Start;
				CurNode->updateMiddle();

			// Check if this node needs to be moved
				if(CurNode->Left && CurNode->Middle <= CurNode->Left->Middle) {
				// This node needs to be removed and added back
					removeNode(CurNode);
					auto *Node = insertNode(CurNode);
					if(Node != CurNode) {
					// This means a node was found to have been in the tree already
					// so we do not need the current node anymore.
						NodesSet.erase(CurNode);
						delete CurNode;
						CurNode = Node;
					}
				}
				return ITResult(ITResult::NoOverlap, CurNode);
			}

		// If partial overlap is found
			if(Start >= CurNode->Start && Start < CurNode->End) {
			// Record current state	of node that is about to be updated
				auto State = ITResult::NodeRange(CurNode->Start, CurNode->End);

			// Update the node
				CurNode->End = End;
				CurNode->updateMiddle();

			// Check if this node needs to be moved
				if(CurNode->Right && CurNode->Middle > CurNode->Right->Middle) {
				// This node needs to be removed and added back
					removeNode(CurNode);
					auto *Node = insertNode(CurNode);
					if(Node != CurNode) {
					// This means a node was found to have been in the tree already
					// so we do not need the current node anymore.
						NodesSet.erase(CurNode);
						delete CurNode;
						CurNode = Node;
					}
				}
				return ITResult(ITResult::PartialOverlap, CurNode, State);
			}
			if(End > CurNode->Start && End <= CurNode->End) {
			// Record current state	of node that is about to be updated
				auto State = ITResult::NodeRange(CurNode->Start, CurNode->End);

			// Update the node
				CurNode->Start = Start;
				CurNode->updateMiddle();

			// Check if this node needs to be moved
				if(CurNode->Left && CurNode->Middle <= CurNode->Left->Middle) {
				// This node needs to be removed and added back
					removeNode(CurNode);
					auto *Node = insertNode(CurNode);
					if(Node != CurNode) {
					// This means a node was found to have been in the tree already
					// so we do not need the current node anymore.
						NodesSet.erase(CurNode);
						delete CurNode;
						CurNode = Node;
					}
				}
				return ITResult(ITResult::PartialOverlap, CurNode, State);
			}
			if(Start < CurNode->Start && End > CurNode->End) {
			// Record current state	of node that is about to be updated
				auto State = ITResult::NodeRange(CurNode->Start, CurNode->End);
				CurNode->Start = Start;
				CurNode->End = End;
				CurNode->updateMiddle();

			// Check if this node needs to be moved
				if((CurNode->Left && CurNode->Middle <= CurNode->Left->Middle)
				|| (CurNode->Right && CurNode->Middle > CurNode->Right->Middle)) {
					removeNode(CurNode);
					auto *Node = insertNode(CurNode);
					if(Node != CurNode) {
					// This means a node was found to have been in the tree already
					// so we do not need the current node anymore.
						NodesSet.erase(CurNode);
						delete CurNode;
						CurNode = Node;
					}
				}
			}
		}

	// No overlap cases
		ParentNode = CurNode;
		std::cout << "CURRENT NODE MIDDDLE: " << CurNode->Middle << "\n";
		if(Middle < CurNode->Middle) {
			CurNodeAddr = &(CurNode->Left);
			std::cout << "LEFT\n";
		} else {
			CurNodeAddr = &(CurNode->Right);
			std::cout << "RIGHT\n";
		}
		std::cout << "PRINTING PARENT NODE:\n";
		ParentNode->print();
	}

// Just add a new node
	std::cout << "NODE INSERTED AT ROOT\n";
	*CurNodeAddr = new IntervalNode(Start, End);
	std::cout << "INTERVAL NODE ALLOCATED\n";
	NodesSet.insert(*CurNodeAddr);
	(*CurNodeAddr)->Parent = ParentNode;
	return ITResult(ITResult::NoOverlap, *CurNodeAddr);
}


#endif  // INTERVAL_TREE_H_
