//=================== COMMON SCC OPERATIONS =====================//
//
//--------------------------------------------------------------//
//
// This contains some decalration of common operations that passes
// perform on strongly connected components.
//
//==============================================================//


#ifndef COMMON_SCC_OPS_H_
#define COMMON_SCC_OPS_H_

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"

#include "CondBlockBase.h"
#include "CondBlockBaseImpl.h"
#include "GenCondInfo.h"
#include "GenCondInfoImpl.h"
#include "SCC_Iterator.h"
#include "InstsSet.h"

namespace llvm {

typedef std::pair<SCC_Iterator<Function *>, SerialInstsSet<>> SCCToInstsPairTy;
typedef SmallVector<SCCToInstsPairTy, 16> SCCToInstsPairVectTy;


// This merges the SCCs that do not contain certain "stop" instructions.
// We define "stop" instructions as instructions at which we stop
// looking for certain persist operations and commit the accumulated sets
// of persistent operations.
void MergeAcrossSCCs(SCCToInstsPairVectTy &SCCToInstsPairVect,
					 SCCToInstsPairVectTy &StopFreeSCCToInstsPairVect,
					 SmallVector<BasicBlock *, 4> &BBWithFirstSerialInsts,
					 DenseMap<BasicBlock *, SCC_Iterator<Function *>> &BlockToSCCMap);

// This function seperates the persist operations if they happen to be in condblock sets
// because we cannot statically analyze the perisist operations in condblock sets, however,
// they can be analyzed dynamically.
void SeparateAcrossSCCsAndCondBlockSets(
					SCCToInstsPairVectTy &StopFreeSCCToInstsPairVect,
					DenseMap<BasicBlock *, SCC_Iterator<Function *>> &BlockToSCCMap,
					GenCondBlockSetLoopInfo &GI);

}  // end of namespace llvm

#endif  // COMMON_SCC_OPS_H_
