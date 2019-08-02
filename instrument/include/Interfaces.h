//======================== PMDK Interfaces Record ==================//
//
// Contains classes with bunch of lists of the developer PMDK API
//
//==================================================================//

#ifndef PMDK_INTERFACE_H
#define PMDK_INTERFACE_H

#define _GNU_SOURCE
#include <vector>
#include <string>
#include <iostream>

#include "llvm/ADT/SmallVector.h"
//#include "llvm/IR/InstTypes.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Instructions.h"


namespace llvm {

template<class T = CallInst>
class InterfacesRecordBase {
// Vector of PM functions used in PMDK
	SmallVector<std::string, 8> PMDKInterfaces;

// Vector of intrinsics, such as that of Intel's
	SmallVector<std::string, 4> Intrinsics;

public:
	bool isValidInterfaceCall(const T *I) const {
		return (isIntrinsicCall(I) || isPMDKInterfaceCall(I));
	}

	bool isIntrinsicCall(const T *I) const {
		for(auto &interface : Intrinsics) {
			if(interface.size() != I->getCalledFunction()->getName().size())
				 continue;

			unsigned i;
			for(i = 0; i != interface.size(); ++i) {
				if(interface[i] != I->getCalledFunction()->getName()[i])
					break;
			}
			if(i == interface.size())
				return true;
		}
		return false;
	}

	bool isPMDKInterfaceCall(const T *I) const {
		for(auto &interface : PMDKInterfaces) {
			if(interface.size() != I->getCalledFunction()->getName().size())
				 continue;

			unsigned i;
			for(i = 0; i != interface.size(); ++i) {
				if(interface[i] != I->getCalledFunction()->getName()[i])
					break;
			}
			if(i == interface.size())
				return true;
		}
		return false;
	}

	void addPMDKInterface(std::string Interface) {
		PMDKInterfaces.push_back(Interface);
	}

	void addIntrinsic(std::string Interface) {
		Intrinsics.push_back(Interface);
	}

// Iterators
	using pmdk_iterator = typename SmallVector<std::string, 8>::const_iterator;
	using intrinsic_iterator = typename SmallVector<std::string, 4>::const_iterator;

	pmdk_iterator pmdk_begin() {
		return PMDKInterfaces.begin();
	}

	pmdk_iterator pmdk_end() {
		return PMDKInterfaces.end();
	}

	intrinsic_iterator intrinsic_begin() {
		return Intrinsics.begin();
	}

	intrinsic_iterator instrinsic_end() {
		return Intrinsics.end();
	}
};

template<class T = CallInst>
struct PmemOpInterface : public InterfacesRecordBase<T> {
	Value *getDestOperand(const T *I) const;

	Value *getSrcOperand(const T *I) const;

	Value *getLengthOperand(const T *I) const;
};

template<class T = CallInst>
struct PMemPersistInterface : public InterfacesRecordBase<T> {
	Value *getPMemAddrOperand(const T *I) const {
		if(!InterfacesRecordBase<T>::isPMDKInterfaceCall(I))
			return nullptr;

	// Get the first operand
		return I->getArgOperand(0)->stripPointerCasts();
	}

	Value *getPMemLenOperand(const T *I) const {
		if(!InterfacesRecordBase<T>::isPMDKInterfaceCall(I))
			return nullptr;

	// Get the second operand
		return I->getArgOperand(1);
	}

	Value *getFlushAlignedAddrOperand(const T *I) const {
		if(!isIntrinsicInterface(I))
			return nullptr;
		return I->getArgOperand(0)->stripPointerCasts();
	}
};

template<class T = CallInst>
struct MapInterface : public InterfacesRecordBase<T> {
	MapInterface();

	Value *getPmemFlagOperand(const T *I) const {
	// Return the last argument of the call instruction since the
	// last operand is supposed to determine whether or not the
	// mapped memory region is in volatile memory or not.
		return I->getArgOperand(I->getNumArgOperands() - 1);
	}

	Value *getPMemLenOperand(const T *I) const {
	// Return the second argument
		return I->getArgOperand(1);
	}
};

template<class T = CallInst>
struct UnmapInterface : public InterfacesRecordBase<T> {
	UnmapInterface();
};

template<class T = CallInst>
struct FlushInterface : public PMemPersistInterface<T> {
	FlushInterface();
};

template<class T = CallInst>
struct PersistInterface : public PMemPersistInterface<T> {
	PersistInterface();
};

template<class T = CallInst>
struct DrainInterface : public PmemOpInterface<T> {
	DrainInterface();
};

template<class T = CallInst>
struct MsyncInterface : public PMemPersistInterface<T> {
	MsyncInterface();
};

template<class T = CallInst>
struct PmemInterface : public PmemOpInterface<T> {
	PmemInterface();
};

template<class T = CallInst>
struct AllocInterface : public InterfacesRecordBase<T> {
	AllocInterface();
};

// This class includes all the memory and string operations
template<class T = CallInst>
struct GenMemInterface {
private:
	SmallVector<std::string, 8> GenInterfaces;

public:
	GenMemInterface();

	bool isValidInterfaceCall(const T *I) const {
		for(auto &interface : GenInterfaces) {
			if(I->getCalledFunction()->getName() == interface)
				return true;
		}
		return false;
	}

	void addGenInterface(std::string Interface) {
		GenInterfaces.push_back(Interface);
	}
};


template<class T = CallInst>
class PMInterfaces {
	MapInterface<T> MI;
	AllocInterface<T> AI;
	PmemInterface<T> PMI;
	MsyncInterface<T> MSI;
	DrainInterface<T> DI;
	PersistInterface<T> PI;
	FlushInterface<T> FI;
	GenMemInterface<T> GI;
	UnmapInterface<T> UI;

public:
	PMInterfaces() : AI(AllocInterface<T>()), PMI(PmemInterface<T>()),
					 MSI(MsyncInterface<T>()), DI(DrainInterface<T>()),
					 PI(PersistInterface<T>()), FI(FlushInterface<T>()),
					 MI(MapInterface<T>()), GI(GenMemInterface<T>()),
					 UI(UnmapInterface<T>()) {}

	const AllocInterface<T> &getAllocInterface() const {
		return AI;
	}

	const PmemInterface<T> &getPmemInterface() const {
		return PMI;
	}

	const MsyncInterface<T> &getMsyncInterface() const{
		return MSI;
	}

	const DrainInterface<T> &getDrainInterface() const {
		return DI;
	}

	const PersistInterface<T> &getPersistInterface() const {
		return PI;
	}

	const FlushInterface<T> &getFlushInterface() const {
		return FI;
	}

	const MapInterface<T> &getMapInterface() const {
		return MI;
	}

	const UnmapInterface<T> &getUnmapInterface() const {
		return UI;
	}

	const GenMemInterface<T> &getGenMemInterface() const {
		return GI;
	}
};

template<class T>
MapInterface<T>::MapInterface() {
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_map_file"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_map_fileU"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_map_fileW"));
}

template<class T>
UnmapInterface<T>::UnmapInterface() {
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_unmap"));
}

template<class T>
FlushInterface<T>::FlushInterface() {
// Intel's flush intrinsics
	InterfacesRecordBase<T>::addIntrinsic(std::string("_mm_cflush"));
	InterfacesRecordBase<T>::addIntrinsic(std::string("_mm_cflushopt"));

// PMDK functions
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_flush"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_deep_flush"));
}

template<class T>
PersistInterface<T>::PersistInterface() {
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_persist"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_deep_persist"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_memset_persist"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_memcpy_persist"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_memmove_persist"));
}

template<class T>
MsyncInterface<T>::MsyncInterface() {
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_msync"));
}

template<class T>
DrainInterface<T>::DrainInterface() {
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_drain"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_deep_drain"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_memset_drain"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_memcpy_drain"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_memset_drain"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("memmove_nodrain_generic"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("memset_nodrain_generic"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("_mm_sfence"));
}

template<class T>
PmemInterface<T>::PmemInterface() {
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_memset"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_memcpy"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pmem_memmove"));
}

template<class T>
AllocInterface<T>::AllocInterface() {
	InterfacesRecordBase<T>::addPMDKInterface(std::string("malloc"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("calloc"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("realloc"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("valloc"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("pvalloc"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("memalign"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("aligned_alloc"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("vmem_malloc"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("vmem_calloc"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("vmem_realloc"));
	InterfacesRecordBase<T>::addPMDKInterface(std::string("vmem_aligned_alloc"));
}

template<class T>
GenMemInterface<T>::GenMemInterface() {
	addGenInterface(std::string("memset"));
	addGenInterface(std::string("memcpy"));
	addGenInterface(std::string("strcpy"));
	addGenInterface(std::string("strncpy"));
}

template<class T>
Value *PmemOpInterface<T>::getDestOperand(const T *I) const {
// Number of arguments has to be more or equal 3.
	if(I->getNumArgOperands() < 3)
		return nullptr;

	Value *argVal = I->getArgOperand(0)->stripPointerCasts();
	if(!argVal->getType()->isIntegerTy())
		return nullptr;

	return argVal;
}

template<class T>
Value *PmemOpInterface<T>::getSrcOperand(const T *I) const {
// Number of arguments has to be more or equal 3.
	if(I->getNumArgOperands() < 3)
		return nullptr;

	Value *argVal = I->getArgOperand(1)->stripPointerCasts();
	if(!argVal->getType()->isIntegerTy())
		return nullptr;

	return argVal;
}

template<class T>
Value *PmemOpInterface<T>::getLengthOperand(const T *I) const {
// Number of arguments has to be more or equal 3.
	if(I->getNumArgOperands() < 3)
		return nullptr;

	Value *argVal = I->getArgOperand(2)->stripPointerCasts();
	if(!argVal->getType()->isIntegerTy())
		return nullptr;

	return argVal;
}

}  // namespace llvm

#endif
