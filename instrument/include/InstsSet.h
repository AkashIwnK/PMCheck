//========================== INSTRUCTION SETS ===========================//
//
//-----------------------------------------------------------------------//
//
// This file contains classes responsible for collecting instruction sets
// for analyis or instrumentation.
//
//-----------------------------------------------------------------------//


#ifndef INSTS_SETS_H_
#define INSTS_SETS_H_

#include <vector>
#include <map>

namespace llvm {

// This holds sets of "consecutive" instructions of a kind
template<typename T = Instruction>
class SerialInstsSet : public std::vector<T *> {
public:
	using iterator = typename std::vector<T *>::iterator;
	using reverse_iterator = typename std::vector<T *>::reverse_iterator;
	using const_iterator = typename std::vector<T *>::const_iterator;

	void printSerialInsts() const {
		errs() << "PRINTING SERIAL INSTS\n";
		for(auto &I : *this) {
			errs() << "PARENT: ";
			I->getParent()->printAsOperand(errs(), false);
			errs() << " ";
			I->print(errs());
			errs() << "\n";
		}
	}
};

template<typename Func = Function, typename Inst = Instruction>
class PerfCheckerInfo {
	//template<class Inst>
	struct SerialInstsSetVectTy : public std::vector<SerialInstsSet<Inst>> {
		typename std::vector<SerialInstsSet<Inst>>::iterator I;

		SerialInstsSetVectTy() : std::vector<SerialInstsSet<Inst>>() {
			I = std::vector<SerialInstsSet<Inst>>::begin();
		}

		using iterator = typename std::vector<SerialInstsSet<Inst>>::iterator;
		using reverse_iterator = typename std::vector<SerialInstsSet<Inst>>::reverse_iterator;
		using const_iterator = typename std::vector<SerialInstsSet<Inst>>::const_iterator;
	};

	DenseMap<Func *, SerialInstsSetVectTy> FuncToSerialInstsSetMap;

public:
	void addSerialInstsSet(Func *F, SerialInstsSet<Inst> &InstsVect) {
		FuncToSerialInstsSetMap[F].push_back(InstsVect);
	}

	unsigned maxSetSize(Func *F) const {
		unsigned Size = 0;
		const auto &SIVect = FuncToSerialInstsSetMap.lookup(F);
		for(auto &SI : SIVect) {
			if(SI.size() > Size)
				Size = SI.size();
		}
		return Size;
	}

	unsigned size(Func *F) const {
		const auto &SIVect = FuncToSerialInstsSetMap.lookup(F);
		return SIVect.size();
	}

	void clear() {
		FuncToSerialInstsSetMap.shrink_and_clear();
	}

// Primarily used for debugging
	void printFuncToSerialInstsSetMap() const {
		for(auto &FuncToSerialInstsPair : FuncToSerialInstsSetMap) {
			Func *F = FuncToSerialInstsPair.first;
			const SerialInstsSetVectTy &SFV = FuncToSerialInstsPair.second;

		// Print info
			errs() << "FUNCTION NAME:" << F->getName() << "\n";
			for(auto &SF : SFV) {
				SF.printSerialInsts();
				errs() << "------------------------------------------\n";
			}
			errs() << "\n\n\n";
		}
	}

// Define the iterators
		using iterator = typename SerialInstsSetVectTy::iterator;

		iterator begin(Function *F) {
			return FuncToSerialInstsSetMap[F].begin();
		}

		iterator end(Function *F) {
			return FuncToSerialInstsSetMap[F].end();
		}
};

}  // end of namespace llvm

#endif  // INSTS_SETS_H_
