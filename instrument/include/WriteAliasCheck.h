//========================== WRITE ALIAS CHECK ===========================//
//
//------------------------------------------------------------------------//
//
// This contains a function for checking whether an instruction that
// performs memory operation(s) aliases with a given set of memory regions
// represented by some values.
//
//------------------------------------------------------------------------//


#ifndef PM_WRITE_ALIAS_CHECK_H__
#define PM_WRITE_ALIAS_CHECK_H__

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/AliasAnalysis.h"

//namespace llvm {

using namespace llvm;

bool WriteAliases(StoreInst *SI, SmallVector<Value *, 16> &StackAndGlobalVarVect,
									AAResults &AA);

bool WriteAliases(CallInst *CI, SmallVector<Value *, 16> &StackAndGlobalVarVect,
									AAResults &AA);

//}  // end of namespace llvm

#endif // PM_WRITE_ALIAS_CHECK_H__
