//===----------------------------------------------------------------------===//
//
// This pass uses the top-down data structure graphs to implement a simple
// context sensitive alias analysis.
//
//===----------------------------------------------------------------------===//


#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/Passes.h"

#include "DataStructure.h"
#include "DSGraph.h"

using namespace llvm;


class DSAA : public ModulePass, public AAResults {
  TDDataStructures *TD;
  BUDataStructures *BU;

// These members are used to cache mod/ref information to make us return
// results faster, particularly for aa-eval.  On the first request of
// mod/ref information for a particular call site, we compute and store the
// calculated nodemap for the call site.  Any time DSA info is updated we
// free this information, and when we move onto a new call site, this
// information is also freed.
  CallBase *MapCB;
  std::multimap<DSNode*, const DSNode*> CallerCalleeMap;

public:
  static char ID;

  DSAA(TargetLibraryInfo &TLI) : TD(nullptr), BU(nullptr), ModulePass(ID), AAResults(TLI) {}

  ~DSAA() {
    InvalidateCache();
  }

  void InvalidateCache() {
    MapCB = nullptr;
    CallerCalleeMap.clear();
  }

  
  bool runOnModule(Module &M) {
    //InitializeAAResults(this);
    TD = &getAnalysis<TDDataStructures>();
    BU = &getAnalysis<BUDataStructures>();
    return false;
  }

  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    //AAResults::getAnalysisUsage(AU);
    AU.setPreservesAll();                         // Does not transform code
    AU.addRequiredTransitive<TDDataStructures>(); // Uses TD Datastructures
    AU.addRequiredTransitive<BUDataStructures>(); // Uses BU Datastructures
  }

  
  AliasResult alias(const Value *V1, LocationSize V1Size,
                    const Value *V2, LocationSize V2Size);

  ModRefInfo getModRefInfo(CallBase *CB, Value *P, LocationSize Size);

  ModRefInfo getModRefInfo(CallBase *CB1, CallBase *CB2) {
    return AAResults::getModRefInfo(CB1, CB2);
  }

  virtual void deleteValue(Value *V) {
    InvalidateCache();
    BU->deleteValue(V);
    TD->deleteValue(V);
  }

  virtual void copyValue(Value *From, Value *To) {
    if (From == To) return;
    InvalidateCache();
    BU->copyValue(From, To);
    TD->copyValue(From, To);
  }

private:
  DSGraph *getGraphForValue(const Value *V);
};

// Register the pass...
//RegisterPass<DSAA> X("ds-aa", "Data Structure Graph Based Alias Analysis");

// Register as an implementation of AAResults
//RegisterAnalysisGroup<AAResults, false> Y;//DSAA> Y;

//ModulePass *llvm::createDSAAPass(TargetLibraryInfo &TLI) { return new DSAA(TLI); }

DSGraph *DSAA::getGraphForValue(const Value *V) {
  if (const Instruction *I = dyn_cast<Instruction>(V))
    return &TD->getDSGraph(*I->getParent()->getParent());
  else if (const Argument *A = dyn_cast<Argument>(V))
    return &TD->getDSGraph(*A->getParent());
  else if (const BasicBlock *BB = dyn_cast<BasicBlock>(V))
    return &TD->getDSGraph(*BB->getParent());
  return 0;
}

AliasResult DSAA::alias(const Value *V1, LocationSize V1Size,
                        const Value *V2, LocationSize V2Size) {
  if (V1 == V2)
    return MustAlias;

  DSGraph *G1 = getGraphForValue(V1);
  DSGraph *G2 = getGraphForValue(V2);
  assert((!G1 || !G2 || G1 == G2) && "Alias query for 2 different functions?");

  DSGraph &G = *(G1 ? G1 : (G2 ? G2 : &TD->getGlobalsGraph()));
  const DSGraph::ScalarMapTy &GSM = G.getScalarMap();
  DSGraph::ScalarMapTy::const_iterator I = GSM.find((Value*)V1);
  if (I == GSM.end())
    return NoAlias;

  DSGraph::ScalarMapTy::const_iterator J = GSM.find((Value*)V2);
  if (J == GSM.end())
    return NoAlias;

  DSNode  *N1 = I->second.getNode(),  *N2 = J->second.getNode();
  unsigned O1 = I->second.getOffset(), O2 = J->second.getOffset();
  if (N1 == 0 || N2 == 0) {
    // Can't tell whether anything aliases null.
    return AAResults::alias(V1, V1Size, V2, V2Size);
  }

  // We can only make a judgment if one of the nodes is complete.
  if (N1->isComplete() || N2->isComplete()) {
    if (N1 != N2)
      return NoAlias;   // Completely different nodes.

    // See if they point to different offsets. If so, we may be able to
    // determine that they do not alias.
    if (O1 != O2) {
      if (O2 < O1) {    // Ensure that O1 <= O2
        std::swap(V1, V2);
        std::swap(O1, O2);
        std::swap(V1Size, V2Size);
      }
      if (O1 + V1Size.getValue() <= O2)
        return NoAlias;
    }
  }

  // FIXME: we could improve on this by checking the globals graph for aliased
  // global queries.
  return AAResults::alias(V1, V1Size, V2, V2Size);
}


//AAResults::ModRefInfo
ModRefInfo
DSAA::getModRefInfo(CallBase *CB, Value *P, LocationSize Size) {
  DSNode *N = nullptr;

  // First step, check our cache.
  if (CB == MapCB) {
    {
      const Function *Caller = CB->getParent()->getParent();
      DSGraph &CallerTDGraph = TD->getDSGraph(*Caller);
      // Figure out which node in the TD graph this pointer corresponds to.
      DSScalarMap &CallerSM = CallerTDGraph.getScalarMap();
      DSScalarMap::iterator NI = CallerSM.find(P);
      if (NI == CallerSM.end()) {
        InvalidateCache();
        return DSAA::getModRefInfo(CB, P, Size);
      }
      N = NI->second.getNode();
    }

  HaveMappingInfo:
    assert(N && "Null pointer in scalar map??");
    typedef std::multimap<DSNode*, const DSNode*>::iterator NodeMapIt;
    std::pair<NodeMapIt, NodeMapIt> Range = CallerCalleeMap.equal_range(N);

    // Loop over all of the nodes in the callee that correspond to "N", keeping
    // track of aggregate mod/ref info.
    bool NeverReads = true, NeverWrites = true;
    for (; Range.first != Range.second; ++Range.first) {
      if (Range.first->second->isModified())
        NeverWrites = false;
      if (Range.first->second->isRead())
        NeverReads = false;
      if (NeverReads == false && NeverWrites == false)
        return AAResults::getModRefInfo(CB, P, Size);
    }
    ModRefInfo Result = ModRefInfo::ModRef;
    if (NeverWrites)      // We proved it was not modified.
      Result = ModRefInfo(static_cast<int>(Result) & ~static_cast<int>(ModRefInfo::Mod));
    if (NeverReads)       // We proved it was not read.
      Result = ModRefInfo(static_cast<int>(Result) & ~static_cast<int>(ModRefInfo::Ref));
    return ModRefInfo(static_cast<int>(Result) & static_cast<int>(AAResults::getModRefInfo(CB, P, Size)));
  }

// Any cached info we have is for the wrong function.
  InvalidateCache();
  Function *F = CB->getCalledFunction();
  if(!F) return AAResults::getModRefInfo(CB, P, Size);
  if(!F->size()) {
  // If we are calling an external function, and if this global doesn't escape
  // the portion of the program we have analyzed, we can draw conclusions
  // based on whether the global escapes the program.
    Function *Caller = CB->getParent()->getParent();
    DSGraph *G = &TD->getDSGraph(*Caller);
    DSScalarMap::iterator NI = G->getScalarMap().find(P);
    if (NI == G->getScalarMap().end()) {
    // If it wasn't in the local function graph, check the global graph.  This
    // can occur for globals who are locally reference but hoisted out to the
    // globals graph despite that.
      G = G->getGlobalsGraph();
      NI = G->getScalarMap().find(P);
      if (NI == G->getScalarMap().end())
        return AAResults::getModRefInfo(CB, P, Size);
    }

  // If we found a node and it's complete, it cannot be passed out to the
  // called function.
    if (NI->second.getNode()->isComplete())
      return ModRefInfo::NoModRef;

    return AAResults::getModRefInfo(CB, P, Size);
  }

  // Get the graphs for the callee and caller.  Note that we want the BU graph
  // for the callee because we don't want all caller's effects incorporated!
  const Function *Caller = CB->getParent()->getParent();
  DSGraph &CallerTDGraph = TD->getDSGraph(*Caller);
  DSGraph &CalleeBUGraph = BU->getDSGraph(*F);

  // Figure out which node in the TD graph this pointer corresponds to.
  DSScalarMap &CallerSM = CallerTDGraph.getScalarMap();
  DSScalarMap::iterator NI = CallerSM.find(P);
  if (NI == CallerSM.end()) {
    ModRefInfo Result = ModRefInfo::ModRef;
    if (isa<ConstantPointerNull>(P) || isa<UndefValue>(P)) {
      return ModRefInfo::NoModRef;                 // null is never modified :)
    } else {
      assert(isa<GlobalVariable>(P) &&
    cast<GlobalVariable>(P)->getType()->getElementType()->isFirstClassType() &&
             "This isn't a global that DSA inconsiderately dropped "
             "from the graph?");
      DSGraph &GG = *CallerTDGraph.getGlobalsGraph();
      DSScalarMap::iterator NI = GG.getScalarMap().find(P);
      if (NI != GG.getScalarMap().end() && !NI->second.isNull()) {
        // Otherwise, if the node is only M or R, return this.  This can be
        // useful for globals that should be marked const but are not.
        DSNode *N = NI->second.getNode();
        if (!N->isModified())
          Result = (ModRefInfo)(static_cast<int>(Result) & ~static_cast<int>(ModRefInfo::Mod));
        if (!N->isRead())
          Result = (ModRefInfo)(static_cast<int>(Result) & ~static_cast<int>(ModRefInfo::Ref));
      }
    }

    if (Result == ModRefInfo::NoModRef)
      return Result;

    return ModRefInfo(static_cast<int>(Result) & static_cast<int>(AAResults::getModRefInfo(CB, P, Size)));
  }

  // Compute the mapping from nodes in the callee graph to the nodes in the
  // caller graph for this call site.
  DSGraph::NodeMapTy CalleeCallerMap;
  DSCallSite DSCS = CallerTDGraph.getDSCallSiteForCallSite(CallSite(dyn_cast<CallInst>(CB)));
  CallerTDGraph.computeCalleeCallerMapping(DSCS, *F, CalleeBUGraph,
                                           CalleeCallerMap);

  // Remember the mapping and the call base for future queries.
  MapCB = CB;

  // Invert the mapping into CalleeCallerInvMap.
  for (DSGraph::NodeMapTy::iterator I = CalleeCallerMap.begin(),
         E = CalleeCallerMap.end(); I != E; ++I)
    CallerCalleeMap.insert(std::make_pair(I->second.getNode(), I->first));
  N = NI->second.getNode();

  goto HaveMappingInfo;
}
