#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"

#include "LibFuncValidityCheck.h"

using namespace llvm;

bool IsValidLibMemoryOperation(const FunctionType &FTy, LibFunc F,
		const DataLayout &DL) {
	auto &Context = FTy.getContext();
	Type *SizeTTy = DL.getIntPtrType(Context, 0);
	auto IsSizeTTy = [SizeTTy](Type *Ty) {
		return SizeTTy ? Ty == SizeTTy : Ty->isIntegerTy();
	};
	unsigned NumParams = FTy.getNumParams();

	// Look for specific library calls performing memory operations
	switch(F) {
		case LibFunc_strcat:
			return (NumParams == 2 && FTy.getReturnType()->isPointerTy() &&
					FTy.getParamType(0) == FTy.getReturnType() &&
					FTy.getParamType(1) == FTy.getReturnType());

		case LibFunc_strncat:
			return (NumParams == 3 && FTy.getReturnType()->isPointerTy() &&
					FTy.getParamType(0) == FTy.getReturnType() &&
					FTy.getParamType(1) == FTy.getReturnType() &&
					IsSizeTTy(FTy.getParamType(2)));

		case LibFunc_strcpy_chk:
		case LibFunc_stpcpy_chk:
			--NumParams;
			if (!IsSizeTTy(FTy.getParamType(NumParams)))
				return false;

		case LibFunc_strcpy:
		case LibFunc_stpcpy:
			return (NumParams == 2 && FTy.getReturnType() == FTy.getParamType(0) &&
					FTy.getParamType(0) == FTy.getParamType(1) &&
					FTy.getParamType(0) == Type::getInt8PtrTy(Context));

		case LibFunc_strncpy_chk:
		case LibFunc_stpncpy_chk:
			--NumParams;
			if (!IsSizeTTy(FTy.getParamType(NumParams)))
				return false;

		case LibFunc_strncpy:
		case LibFunc_stpncpy:
			return (NumParams == 3 && FTy.getReturnType() == FTy.getParamType(0) &&
					FTy.getParamType(0) == FTy.getParamType(1) &&
					FTy.getParamType(0) == Type::getInt8PtrTy(Context) &&
					IsSizeTTy(FTy.getParamType(2)));

		case LibFunc_memcpy_chk:
		case LibFunc_memmove_chk:
			--NumParams;
			if (!IsSizeTTy(FTy.getParamType(NumParams)))
				return false;

		case LibFunc_memcpy:
		case LibFunc_mempcpy:
		case LibFunc_memmove:
			return (NumParams == 3 && FTy.getReturnType() == FTy.getParamType(0) &&
					FTy.getParamType(0)->isPointerTy() &&
					FTy.getParamType(1)->isPointerTy() &&
					IsSizeTTy(FTy.getParamType(2)));

		case LibFunc_memset_chk:
			--NumParams;
			if (!IsSizeTTy(FTy.getParamType(NumParams)))
				return false;

		case LibFunc_memset:
			return (NumParams == 3 && FTy.getReturnType() == FTy.getParamType(0) &&
					FTy.getParamType(0)->isPointerTy() &&
					FTy.getParamType(1)->isIntegerTy() &&
					IsSizeTTy(FTy.getParamType(2)));

		case LibFunc_memccpy:
			return (NumParams >= 2 && FTy.getParamType(1)->isPointerTy());

		default:
			return false;
	}
	return false;
}

bool CalleeIsTerminatesProgram(Function *Callee) {
	bool PotentiallyTermFuncCall = false;
	for(auto &CallAttrSet : Callee->getAttributes()) {
		for(auto &Attr : CallAttrSet) {
			if(Attr.hasAttribute(Attribute::NoReturn)
					|| Attr.hasAttribute(Attribute::NoUnwind)) {
				if(!PotentiallyTermFuncCall) {
					PotentiallyTermFuncCall = true;
				} else {
					// This is a function call that terminates program. Ignore it.
					return true;
				}
			}
		}
	}
	return false;
}
