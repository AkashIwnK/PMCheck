//========================== WRITE ALIAS CHECK ===========================//
//
//------------------------------------------------------------------------//
//
// This contains the function for checking whether an instruction that
// performs memory operation(s) aliases with a given set of memory regions
// represented by some values.
//
//------------------------------------------------------------------------//


#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/AliasAnalysis.h"

#include "WriteAliasCheck.h"

using namespace llvm;

static bool WriteAliasCheck(const MemoryLocation &LocA,
														const MemoryLocation &LocB, AAResults &AA) {
	auto Res = AA.alias(LocA, LocB);
	if(Res == AliasResult::MayAlias || Res == AliasResult::NoAlias) {
		errs() << "NO OR MAY ALIAS\n";
		return false;
	}
	if(Res == AliasResult::PartialAlias || Res == AliasResult::MustAlias) {
		errs() << "MUST OR PARTIAL ALIAS\n";
		return true;
	}
	return false;
}

static bool WriteAliasCheck(const MemoryLocation &WriteLoc,
												    SmallVector<Value *, 16> &StackAndGlobalVarVect,
												    AAResults &AA) {
	for(auto *Val : StackAndGlobalVarVect) {
		Val->print(errs());
		errs() << "\n";
		if(AllocaInst *AI = dyn_cast<AllocaInst>(Val)) {
		// Check alias with a local variable
			const auto &DL = AI->getModule()->getDataLayout();
			errs() << "GOT DATA LAYOUT\n";
			uint64_t Size = DL.getTypeAllocSizeInBits(AI->getAllocatedType());
			if(AI->isArrayAllocation()) {
				auto C = dyn_cast<ConstantInt>(AI->getArraySize());
				if(!C)
					continue;
				Size *= C->getZExtValue();
			}
			errs() << "SIZE: " << Size << "\n";
			const MemoryLocation ValLoc = MemoryLocation(AI, LocationSize(Size));
			if(WriteAliasCheck(WriteLoc, ValLoc, AA))
				return true;
			continue;
		} else if(GlobalVariable *GV = dyn_cast<GlobalVariable>(Val)) {
		// Check alias with a global variable
			const auto &DL = GV->getParent()->getDataLayout();
			uint64_t Size = DL.getTypeAllocSizeInBits(GV->getValueType());
			const MemoryLocation ValLoc = MemoryLocation(GV, LocationSize(Size));
			if(WriteAliasCheck(WriteLoc, ValLoc, AA))
				return true;
			continue;
		}
	}
	return false; // Keep the compiler happy
}

bool WriteAliases(StoreInst *SI, SmallVector<Value *, 16> &StackAndGlobalVarVect,
		   	   	   		AAResults &AA) {
	errs() << "WRITE: ";
	SI->print(errs());
	errs() << "\n";
	const MemoryLocation WriteLoc = MemoryLocation::get(SI);
	return WriteAliasCheck(WriteLoc, StackAndGlobalVarVect, AA);
}

bool WriteAliases(CallInst *CI, SmallVector<Value *, 16> &StackAndGlobalVarVect,
		   	   	   		AAResults &AA) {
	errs() << "WRITE: ";
	CI->print(errs());
	errs() << "\n";

// Deal with memory intrinsics
	if(auto *MemInst = dyn_cast<AnyMemIntrinsic>(CI)) {
		const MemoryLocation WriteLoc = MemoryLocation::getForDest(MemInst);
		return WriteAliasCheck(WriteLoc, StackAndGlobalVarVect, AA);
	}

// Deal with memory library function calls
	auto Size = LocationSize::unknown();
	if(CI->getCalledFunction()->getFunctionType()->getNumParams() >= 3) {
		if(ConstantInt *C = dyn_cast<ConstantInt>(CI->getArgOperand(2)))
			Size = LocationSize::precise(C->getValue().getZExtValue());
	} else {
	// The number of parameters is 2 and we do not know the size.
	}
	AAMDNodes AATags;
	CI->getAAMetadata(AATags);
	const MemoryLocation WriteLoc = MemoryLocation(CI->getArgOperand(0), Size, AATags);
	return WriteAliasCheck(WriteLoc, StackAndGlobalVarVect, AA);
}
