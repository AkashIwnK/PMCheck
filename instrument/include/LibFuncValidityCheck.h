
#ifndef LIB_FUNC_VALIDITY_CHECK_H__
#define LIB_FUNC_VALIDITY_CHECK_H__

#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Attributes.h"

using namespace llvm;

bool IsValidLibMemoryOperation(const FunctionType &FTy, LibFunc F,
			       const DataLayout &DL);

bool CalleeIsTerminatesProgram(Function *Callee);

#endif // LIB_FUNC_VALIDITY_CHECK_H__
