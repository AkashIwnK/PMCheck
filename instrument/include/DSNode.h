
#ifndef LLVM_ANALYSIS_DSNODE_H
#define LLVM_ANALYSIS_DSNODE_H

#include "llvm/IR/DataLayout.h"

#include "DSSupport.h"

#include <unordered_map>
#include <unordered_set>

namespace llvm {

template<typename BaseType>
class DSNodeIterator;          // Data structure graph traversal iterator
class DSGraph;

//===----------------------------------------------------------------------===//
/// DSNode - Data structure node class
///
/// This class represents an untyped memory object of Size bytes.  It keeps
/// track of any pointers that have been stored into the object as well as the
/// different types represented in this object.
class DSNode {
  /// NumReferrers - The number of DSNodeHandles pointing to this node... if
  /// this is a forwarding node, then this is the number of node handles which
  /// are still forwarding over us.
  unsigned NumReferrers;

  /// ForwardNH - This NodeHandle contain the node (and offset into the node)
  /// that this node really is.  When nodes get folded together, the node to be
  /// eliminated has these fields filled in, otherwise ForwardNH.getNode() is
  /// null.
  DSNodeHandle ForwardNH;

  /// Next, Prev - These instance variables are used to keep the node on a
  /// doubly-linked ilist in the DSGraph.
  DSNode *Next, *Prev;
  friend struct ilist_traits<DSNode>;

  /// Size - The current size of the node.  This should be equal to the size of
  /// the current type record.
  unsigned Size;

  /// ParentGraph - The graph this node is currently embedded into.
  DSGraph *ParentGraph;

  /// Ty - Keep track of the current outer most type of this object, in addition
  /// to whether or not it has been indexed like an array or not.  If the
  /// isArray bit is set, the node cannot grow.
  const Type *Ty;                 // The type itself.

  /// Links - Contains one entry for every sizeof(void*) bytes in this memory
  /// object.  Note that if the node is not a multiple of size(void*) bytes
  /// large, that there is an extra entry for the "remainder" of the node as
  /// well.  For this reason, nodes of 1 byte in size do have one link.
  std::vector<DSNodeHandle> Links;

  /// Globals - The list of global values that are merged into this node.
  std::vector<GlobalValue*> Globals;

  void operator=(const DSNode &); // DO NOT IMPLEMENT
  DSNode(const DSNode &);         // DO NOT IMPLEMENT

public:
  enum NodeTy {
    ShadowNode  = 0,        // Nothing is known about this node...
    AllocaNode  = 1 << 0,   // This node was allocated with alloca
    HeapNode    = 1 << 1,   // This node was allocated with malloc
    PMNode      = 1 << 8,   // This node was allocated using PM interface
    GlobalNode  = 1 << 2,   // This node was allocated by a global var decl
    UnknownNode = 1 << 3,   // This node points to unknown allocated memory
    Incomplete  = 1 << 4,   // This node may not be complete
    Modified    = 1 << 5,   // This node is modified in this context
    Read        = 1 << 6,   // This node is read in this context
    Array       = 1 << 7,   // This node is treated like an array
    //#ifndef NDEBUG
    DEAD        = 1 << 8,   // This node is dead and should not be pointed to
    //#endif
    Composition = AllocaNode | HeapNode | GlobalNode | UnknownNode,
  };

  /// NodeType - A union of the above bits.  "Shadow" nodes do not add any flags
  /// to the nodes in the data structure graph, so it is possible to have nodes
  /// with a value of 0 for their NodeType.
private:
  unsigned short NodeType;

public:
  DSNode(const Type *T, DSGraph *G);

  DSNode(const DSNode &N, DSGraph *G, bool NullLinks = false);

  ~DSNode() {
    dropAllReferences();
    assert(hasNoReferrers() && "Referrers to dead node exist!");
  }

  // Iterator for graph interface... Defined in DSGraphTraits.h
  typedef DSNodeIterator<DSNode> iterator;
  typedef DSNodeIterator<const DSNode> const_iterator;
  inline iterator begin();
  inline iterator end();
  inline const_iterator begin() const;
  inline const_iterator end() const;


  unsigned getSize() const { return Size; }

  const Type *getType() const { return Ty; }

  bool isArray() const { return NodeType & Array; }

  bool hasNoReferrers() const { return getNumReferrers() == 0; }

  unsigned getNumReferrers() const { return NumReferrers; }

  DSGraph *getParentGraph() const { return ParentGraph; }

  void setParentGraph(DSGraph *G) { ParentGraph = G; }

  const DataLayout &getDataLayout() const;

  const LLVMContext &getContext() const;

  DSNode *getForwardNode() const { return ForwardNH.getNode(); }

  bool isForwarding() const { return !ForwardNH.isNull(); }

  void stopForwarding() {
    assert(isForwarding() &&
           "Node isn't forwarding, cannot stopForwarding()!");
    ForwardNH.setTo(0, 0);
    assert(ParentGraph == 0 &&
           "Forwarding nodes must have been removed from graph!");
    delete this;
  }

  bool hasLink(unsigned Offset) const {
    assert((Offset & ((1 << DS::PointerShift)-1)) == 0 &&
           "Pointer offset not aligned correctly!");
    unsigned Index = Offset >> DS::PointerShift;
    assert(Index < Links.size() && "Link index is out of range!");
    return Links[Index].getNode();
  }

  DSNodeHandle &getLink(unsigned Offset) {
    assert((Offset & ((1 << DS::PointerShift)-1)) == 0 &&
           "Pointer offset not aligned correctly!");
    unsigned Index = Offset >> DS::PointerShift;
    assert(Index < Links.size() && "Link index is out of range!");
    return Links[Index];
  }

  const DSNodeHandle &getLink(unsigned Offset) const {
    assert((Offset & ((1 << DS::PointerShift)-1)) == 0 &&
           "Pointer offset not aligned correctly!");
    unsigned Index = Offset >> DS::PointerShift;
    assert(Index < Links.size() && "Link index is out of range!");
    return Links[Index];
  }

  unsigned getNumLinks() const { return Links.size(); }

  typedef std::vector<DSNodeHandle>::iterator edge_iterator;
  typedef std::vector<DSNodeHandle>::const_iterator const_edge_iterator;
  edge_iterator edge_begin() { return Links.begin(); }
  edge_iterator edge_end() { return Links.end(); }
  const_edge_iterator edge_begin() const { return Links.begin(); }
  const_edge_iterator edge_end() const { return Links.end(); }

  bool mergeTypeInfo(const Type *Ty, unsigned Offset,
                     bool FoldIfIncompatible = true);

  void foldNodeCompletely();

  bool isNodeCompletelyFolded() const;

  void setLink(unsigned Offset, const DSNodeHandle &NH) {
    assert((Offset & ((1 << DS::PointerShift)-1)) == 0 &&
           "Pointer offset not aligned correctly!");
    unsigned Index = Offset >> DS::PointerShift;
    assert(Index < Links.size() && "Link index is out of range!");
    Links[Index] = NH;
  }

  unsigned getPointerSize() const { return DS::PointerSize; }

  void addEdgeTo(unsigned Offset, const DSNodeHandle &NH);

  void mergeWith(const DSNodeHandle &NH, unsigned Offset);

  void addGlobal(GlobalValue *GV);

  void removeGlobal(GlobalValue *GV);
  void mergeGlobals(const std::vector<GlobalValue*> &RHS);
  void clearGlobals() { std::vector<GlobalValue*>().swap(Globals); }

  const std::vector<GlobalValue*> &getGlobalsList() const { return Globals; }

  void addFullGlobalsList(std::vector<GlobalValue*> &List) const;

  void addFullFunctionList(std::vector<Function*> &List) const;

  typedef std::vector<GlobalValue*>::const_iterator globals_iterator;
  globals_iterator globals_begin() const { return Globals.begin(); }
  globals_iterator globals_end() const { return Globals.end(); }

  void maskNodeTypes(unsigned Mask) {
    NodeType &= Mask;
  }

  void mergeNodeFlags(unsigned RHS) {
    NodeType |= RHS;
  }

  unsigned getNodeFlags() const { return NodeType & ~DEAD; }
  bool isAllocaNode()  const { return NodeType & AllocaNode; }
  bool isHeapNode()    const { return NodeType & HeapNode; }
  bool isPMNode()      const {return NodeType & PMNode; }
  bool isGlobalNode()  const { return NodeType & GlobalNode; }
  bool isUnknownNode() const { return NodeType & UnknownNode; }
  bool isModified() const   { return NodeType & Modified; }
  bool isRead() const       { return NodeType & Read; }
  bool isIncomplete() const { return NodeType & Incomplete; }
  bool isComplete() const   { return !isIncomplete(); }
  bool isDeadNode() const   { return NodeType & DEAD; }
  DSNode *setAllocaNodeMarker()  { NodeType |= AllocaNode;  return this; }
  DSNode *setHeapNodeMarker()    { NodeType |= HeapNode;    return this; }
  DSNode *setPMNodeMarker()      { NodeType |= PMNode;    return this; }
  DSNode *setGlobalNodeMarker()  { NodeType |= GlobalNode;  return this; }
  DSNode *setUnknownNodeMarker() { NodeType |= UnknownNode; return this; }
  DSNode *setIncompleteMarker() { NodeType |= Incomplete; return this; }
  DSNode *setModifiedMarker()   { NodeType |= Modified;   return this; }
  DSNode *setReadMarker()       { NodeType |= Read;       return this; }
  DSNode *setArrayMarker()      { NodeType |= Array; return this; }

  void makeNodeDead() {
    Globals.clear();
    assert(hasNoReferrers() && "Dead node shouldn't have refs!");
    NodeType = DEAD;
  }

  void forwardNode(DSNode *To, unsigned Offset);

  void print(std::ostream &O, const DSGraph *G) const;

  void dump() const;

  void assertOK() const;

  void dropAllReferences() {
    Links.clear();
    if (isForwarding())
      ForwardNH.setTo(0, 0);
  }

  void remapLinks(std::unordered_map<const DSNode*, DSNodeHandle> &OldNodeMap);

  void markReachableNodes(std::unordered_set<const DSNode*> &ReachableNodes) const;

private:
  friend class DSNodeHandle;

  // static mergeNodes - Helper for mergeWith()
  static void MergeNodes(DSNodeHandle& CurNodeH, DSNodeHandle& NH);
};

template<>
struct ilist_traits<DSNode> {
  static DSNode *getPrev(const DSNode *N) { return N->Prev; }

  static DSNode *getNext(const DSNode *N) { return N->Next; }

  static void setPrev(DSNode *N, DSNode *Prev) { N->Prev = Prev; }

  static void setNext(DSNode *N, DSNode *Next) { N->Next = Next; }

  static DSNode *createSentinel() { return new DSNode(0,0); }

  static void destroySentinel(DSNode *N) { delete N; }
  //static DSNode *createNode(const DSNode &V) { return new DSNode(V); }

  void addNodeToList(DSNode *NTy) {}

  void removeNodeFromList(DSNode *NTy) {}

  //void transferNodesFromList(iplist<DSNode, ilist_traits> &L2,
    //                         ilist_iterator<DSNode> first,
      //                       ilist_iterator<DSNode> last) {}
};

template<>
struct ilist_traits<const DSNode> : public ilist_traits<DSNode> {};

inline DSNode *DSNodeHandle::getNode() const {
  // Disabling this assertion because it is failing on a "magic" struct
  // in named (from bind).  The fourth field is an array of length 0,
  // presumably used to create struct instances of different sizes.
  assert((!N ||
          N->isNodeCompletelyFolded() ||
          (N->Size == 0 && Offset == 0) ||
          (int(Offset) >= 0 && Offset < N->Size) ||
          (int(Offset) < 0 && -int(Offset) < int(N->Size)) ||
          N->isForwarding()) && "Node handle offset out of range!");
  if (N == 0 || !N->isForwarding())
    return N;
  return HandleForwarding();
}

inline void DSNodeHandle::setTo(DSNode *n, unsigned NewOffset) const {
  assert(!n || !n->isForwarding() && "Cannot set node to a forwarded node!");
  if (N) getNode()->NumReferrers--;
  N = n;
  Offset = NewOffset;
  if (N) {
    N->NumReferrers++;
    if (Offset >= N->Size) {
      assert((Offset == 0 || N->Size == 1) &&
             "Pointer to non-collapsed node with invalid offset!");
      Offset = 0;
    }
  }
  assert(!N || ((N->NodeType & DSNode::DEAD) == 0));
  assert((!N || Offset < N->Size || (N->Size == 0 && Offset == 0) ||
          N->isForwarding()) && "Node handle offset out of range!");
}

inline bool DSNodeHandle::hasLink(unsigned Num) const {
  assert(N && "DSNodeHandle does not point to a node yet!");
  return getNode()->hasLink(Num+Offset);
}

inline const DSNodeHandle &DSNodeHandle::getLink(unsigned Off) const {
  assert(N && "DSNodeHandle does not point to a node yet!");
  return getNode()->getLink(Offset+Off);
}

inline DSNodeHandle &DSNodeHandle::getLink(unsigned Off) {
  assert(N && "DSNodeHandle does not point to a node yet!");
  return getNode()->getLink(Off+Offset);
}

inline void DSNodeHandle::setLink(unsigned Off, const DSNodeHandle &NH) {
  assert(N && "DSNodeHandle does not point to a node yet!");
  getNode()->setLink(Off+Offset, NH);
}

inline void DSNodeHandle::addEdgeTo(unsigned Off, const DSNodeHandle &Node) {
  assert(N && "DSNodeHandle does not point to a node yet!");
  getNode()->addEdgeTo(Off+Offset, Node);
}

inline void DSNodeHandle::mergeWith(const DSNodeHandle &Node) const {
  if (!isNull())
    getNode()->mergeWith(Node, Offset);
  else {   // No node to merge with, so just point to Node
    Offset = 0;
    DSNode *NN = Node.getNode();
    setTo(NN, Node.getOffset());
  }
}

} // End llvm namespace

#endif
