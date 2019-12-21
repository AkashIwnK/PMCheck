//===----------------------------------------------------------------------===//
//
// Implement the LLVM data structure analysis library.
//
//===----------------------------------------------------------------------===//


#ifndef LLVM_ANALYSIS_DATA_STRUCTURE_H
#define LLVM_ANALYSIS_DATA_STRUCTURE_H

#include "llvm/Pass.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/GlobalValue.h"

#include "DSGraph.h"
#include "DSNode.h"
#include "DSSupport.h"

#include <unordered_map>
#include <unordered_set>
#include <map>

namespace llvm {

	FunctionPass *createDataStructureStatsPass();
	FunctionPass *createDataStructureGraphCheckerPass();

	namespace DataStructureAnalysis {
		/// isPointerType - Return true if this first class type is big enough to hold
		/// a pointer.
		bool isPointerType(const Type *Ty);
	}

	// LocalDataStructures - The analysis that computes the local data structure
	// graphs for all of the functions in the program.
	void initializeLocalDataStructuresPass(PassRegistry &);

	class LocalDataStructures : public ModulePass {
		// DSInfo, one graph for each function
		std::unordered_map<Function*, DSGraph*> DSInfo;
		DSGraph *GlobalsGraph;

		/// GlobalECs - The equivalence classes for each global value that is merged
		/// with other global values in the DSGraphs.
		EquivalenceClasses<GlobalValue*> GlobalECs;

		public:
		static char ID;

		~LocalDataStructures() { releaseMemory(); }

		virtual bool runOnModule(Module &M);

		bool hasGraph(const Function &F) const {
			return DSInfo.find(const_cast<Function*>(&F)) != DSInfo.end();
		}

		DSGraph &getDSGraph(const Function &F) const {
			std::unordered_map<Function*, DSGraph*>::const_iterator I =
				DSInfo.find(const_cast<Function*>(&F));
			assert(I != DSInfo.end() && "Function not in module!");
			return *I->second;
		}

		DSGraph &getGlobalsGraph() const { return *GlobalsGraph; }

		EquivalenceClasses<GlobalValue*> &getGlobalECs() { return GlobalECs; }

		void print(std::ostream &O, const Module *M) const;

		virtual void releaseMemory();

		virtual void getAnalysisUsage(AnalysisUsage &AU) const {
			AU.setPreservesAll();
		}
	};

	/// BUDataStructures - The analysis that computes the interprocedurally closed
	/// data structure graphs for all of the functions in the program.  This pass
	/// only performs a "Bottom Up" propagation (hence the name).
	void initializeBUDataStructuresPass(PassRegistry &);

	class BUDataStructures : public ModulePass {
		protected:
			// DSInfo, one graph for each function
			std::unordered_map<Function*, DSGraph*> DSInfo;
			DSGraph *GlobalsGraph;
			std::set<std::pair<Instruction*, Function*> > ActualCallees;

			// This map is only maintained during construction of BU Graphs
			std::map<std::vector<Function*>,
				std::pair<DSGraph*, std::vector<DSNodeHandle> > > *IndCallGraphMap;

			/// GlobalECs - The equivalence classes for each global value that is merged
			/// with other global values in the DSGraphs.
			EquivalenceClasses<GlobalValue*> GlobalECs;

		public:
			static char ID;

			~BUDataStructures() { releaseMyMemory(); }

			BUDataStructures() : ModulePass(ID) {
				initializeBUDataStructuresPass(*PassRegistry::getPassRegistry());
			}

			virtual bool runOnModule(Module &M);

			bool hasGraph(const Function &F) const {
				return DSInfo.find(const_cast<Function*>(&F)) != DSInfo.end();
			}

			DSGraph &getDSGraph(const Function &F) const {
				std::unordered_map<Function*, DSGraph*>::const_iterator I =
					DSInfo.find(const_cast<Function*>(&F));
				if (I != DSInfo.end())
					return *I->second;
				return const_cast<BUDataStructures*>(this)->
					CreateGraphForExternalFunction(F);
			}

			DSGraph &getGlobalsGraph() const { return *GlobalsGraph; }

			EquivalenceClasses<GlobalValue*> &getGlobalECs() { return GlobalECs; }

			DSGraph &CreateGraphForExternalFunction(const Function &F);

			void deleteValue(Value *V);

			void copyValue(Value *From, Value *To);

			void print(std::ostream &O, const Module *M) const;

			void releaseMyMemory();

			virtual void getAnalysisUsage(AnalysisUsage &AU) const {
				AU.setPreservesAll();
				AU.addRequired<LocalDataStructures>();
			}

			typedef std::set<std::pair<Instruction*, Function*> > ActualCalleesTy;
			const ActualCalleesTy &getActualCallees() const {
				return ActualCallees;
			}

			typedef ActualCalleesTy::const_iterator callee_iterator;
			callee_iterator callee_begin(Instruction *I) const {
				return ActualCallees.lower_bound(std::pair<Instruction*,Function*>(I, 0));
			}
			callee_iterator callee_end(Instruction *I) const {
				I = (Instruction*)((char*)I + 1);
				return ActualCallees.lower_bound(std::pair<Instruction*,Function*>(I, 0));
			}

		private:
			void calculateGraph(DSGraph &G);

			DSGraph &getOrCreateGraph(Function *F);

			unsigned calculateGraphs(Function *F, std::vector<Function*> &Stack,
					unsigned &NextID,
					std::unordered_map<Function*, unsigned> &ValMap);
	};

	void initializeTDDataStructuresPass(PassRegistry &);

	class TDDataStructures : public ModulePass {
		// DSInfo, one graph for each function
		std::unordered_map<Function*, DSGraph*> DSInfo;
		std::unordered_set<Function*> ArgsRemainIncomplete;
		DSGraph *GlobalsGraph;
		BUDataStructures *BUInfo;

		/// GlobalECs - The equivalence classes for each global value that is merged
		/// with other global values in the DSGraphs.
		EquivalenceClasses<GlobalValue*> GlobalECs;

		/// CallerCallEdges - For a particular graph, we keep a list of these records
		/// which indicates which graphs call this function and from where.
		struct CallerCallEdge {
			DSGraph *CallerGraph;        // The graph of the caller function.
			const DSCallSite *CS;        // The actual call site.
			Function *CalledFunction;    // The actual function being called.
			CallerCallEdge(DSGraph *G, const DSCallSite *cs, Function *CF)
				: CallerGraph(G), CS(cs), CalledFunction(CF) {}
			bool operator<(const CallerCallEdge &RHS) const {
				return CallerGraph < RHS.CallerGraph ||
					(CallerGraph == RHS.CallerGraph && CS < RHS.CS);
			}
		};

		std::map<DSGraph*, std::vector<CallerCallEdge> > CallerEdges;

		// IndCallMap - We memoize the results of indirect call inlining operations
		// that have multiple targets here to avoid N*M inlining.  The key to the map
		// is a sorted set of callee functions, the value is the DSGraph that holds
		// all of the caller graphs merged together, and the DSCallSite to merge with
		// the arguments for each function.
		std::map<std::vector<Function*>, DSGraph*> IndCallMap;

		public:
		static char ID;

		~TDDataStructures() { releaseMyMemory(); }

		TDDataStructures() : ModulePass(ID) {
			initializeTDDataStructuresPass(*PassRegistry::getPassRegistry());
		}

		virtual bool runOnModule(Module &M);

		bool hasGraph(const Function &F) const {
			return DSInfo.find(const_cast<Function*>(&F)) != DSInfo.end();
		}

		DSGraph &getDSGraph(const Function &F) const {
			std::unordered_map<Function*, DSGraph*>::const_iterator I =
				DSInfo.find(const_cast<Function*>(&F));
			if (I != DSInfo.end()) return *I->second;
			return const_cast<TDDataStructures*>(this)->
				getOrCreateDSGraph(const_cast<Function&>(F));
		}

		DSGraph &getGlobalsGraph() const { return *GlobalsGraph; }

		EquivalenceClasses<GlobalValue*> &getGlobalECs() { return GlobalECs; }

		void deleteValue(Value *V);

		void copyValue(Value *From, Value *To);

		void print(std::ostream &O, const Module *M) const;

		virtual void releaseMyMemory();

		virtual void getAnalysisUsage(AnalysisUsage &AU) const {
			AU.setPreservesAll();
			AU.addRequired<BUDataStructures>();
		}

		private:
		void markReachableFunctionsExternallyAccessible(DSNode *N,
				std::unordered_set<DSNode*> &Visited);

		void InlineCallersIntoGraph(DSGraph &G);

		DSGraph &getOrCreateDSGraph(Function &F);

		void ComputePostOrder(Function &F, std::unordered_set<DSGraph*> &Visited,
				std::vector<DSGraph*> &PostOrder);
	};

	void initializeCompleteBUDataStructuresPass(PassRegistry &);

	struct CompleteBUDataStructures : public BUDataStructures {
		virtual bool runOnModule(Module &M);

		bool hasGraph(const Function &F) const {
			return DSInfo.find(const_cast<Function*>(&F)) != DSInfo.end();
		}

		DSGraph &getDSGraph(const Function &F) const {
			std::unordered_map<Function*, DSGraph*>::const_iterator I =
				DSInfo.find(const_cast<Function*>(&F));
			assert(I != DSInfo.end() && "Function not in module!");
			return *I->second;
		}

		virtual void getAnalysisUsage(AnalysisUsage &AU) const {
			AU.setPreservesAll();
			AU.addRequired<BUDataStructures>();
			AU.addRequired<TDDataStructures>();
		}

		void print(std::ostream &O, const Module *M) const;

		public:
		static char ID;

		CompleteBUDataStructures() : BUDataStructures() {
			initializeCompleteBUDataStructuresPass(*PassRegistry::getPassRegistry());
		}

		private:
		unsigned calculateSCCGraphs(DSGraph &FG, std::vector<DSGraph*> &Stack,
				unsigned &NextID,
				std::unordered_map<DSGraph*, unsigned> &ValMap);

		DSGraph &getOrCreateGraph(Function &F);

		void processGraph(DSGraph &G);
	};

	void initializeEquivClassGraphsPass(PassRegistry &);

	/// EquivClassGraphs - This is the same as the complete bottom-up graphs, but
	/// with functions partitioned into equivalence classes and a single merged
	/// DS graph for all functions in an equivalence class.  After this merging,
	/// graphs are inlined bottom-up on the SCCs of the final (CBU) call graph.
	struct EquivClassGraphs : public ModulePass {
		CompleteBUDataStructures *CBU;
		DSGraph *GlobalsGraph;

		// DSInfo - one graph for each function.
		std::unordered_map<const Function*, DSGraph*> DSInfo;

		/// ActualCallees - The actual functions callable from indirect call sites.
		///
		std::set<std::pair<Instruction*, Function*> > ActualCallees;

		// Equivalence class where functions that can potentially be called via the
		// same function pointer are in the same class.
		EquivalenceClasses<Function*> FuncECs;

		/// OneCalledFunction - For each indirect call, we keep track of one
		/// target of the call.  This is used to find equivalence class called by
		/// a call site.
		std::map<DSNode *, Function *> OneCalledFunction;

		/// GlobalECs - The equivalence classes for each global value that is merged
		/// with other global values in the DSGraphs.
		EquivalenceClasses<GlobalValue*> GlobalECs;

		public:
		static char ID;

		EquivClassGraphs() : ModulePass(ID) {
			initializeEquivClassGraphsPass(*PassRegistry::getPassRegistry());
		}

		virtual bool runOnModule(Module &M);

		void print(std::ostream &O, const Module *M) const;

		EquivalenceClasses<GlobalValue*> &getGlobalECs() { return GlobalECs; }

		DSGraph &getDSGraph(const Function &F) const {
			std::unordered_map<const Function*, DSGraph*>::const_iterator I = DSInfo.find(&F);
			assert(I != DSInfo.end() && "No graph computed for that function!");
			return *I->second;
		}

		bool hasGraph(const Function &F) const {
			return DSInfo.find(&F) != DSInfo.end();
		}

		bool ContainsDSGraphFor(const Function &F) const {
			return DSInfo.find(&F) != DSInfo.end();
		}

		Function *getSomeCalleeForCallSite(const CallSite &CS) const;

		DSGraph &getGlobalsGraph() const {
			return *GlobalsGraph;
		}

		typedef std::set<std::pair<Instruction*, Function*> > ActualCalleesTy;
		const ActualCalleesTy &getActualCallees() const {
			return ActualCallees;
		}

		typedef ActualCalleesTy::const_iterator callee_iterator;
		callee_iterator callee_begin(Instruction *I) const {
			return ActualCallees.lower_bound(std::pair<Instruction*,Function*>(I, 0));
		}

		callee_iterator callee_end(Instruction *I) const {
			I = (Instruction*)((char*)I + 1);
			return ActualCallees.lower_bound(std::pair<Instruction*,Function*>(I, 0));
		}

		virtual void getAnalysisUsage(AnalysisUsage &AU) const {
			AU.setPreservesAll();
			AU.addRequired<CompleteBUDataStructures>();
		}

		private:
		void buildIndirectFunctionSets(Module &M);
		unsigned processSCC(DSGraph &FG, std::vector<DSGraph*> &Stack,
				unsigned &NextID,
				std::map<DSGraph*, unsigned> &ValMap);
		void processGraph(DSGraph &FG);
		DSGraph &getOrCreateGraph(Function &F);
	};

} // End llvm namespace

#endif

