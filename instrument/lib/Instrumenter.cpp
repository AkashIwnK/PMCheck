

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/Casting.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/ADT/STLExtras.h"

#include "CondBlockBase.h"
#include "CondBlockBaseImpl.h"
#include "GenCondInfo.h"
#include "GenCondInfoImpl.h"
#include "Interfaces.h"
#include "SCC_Iterator.h"
#include "FlowAwarePostOrder.h"
#include "WriteAliasCheck.h"
#include "PMModelVerifier.h"
#include "InstsSet.h"
#include "CommonSCCOps.h"
#include "Instrumenter.h"
#include "LibFuncValidityCheck.h"

#include <string>

using namespace llvm;

// Register the instrumentation pass
char InstrumentationPass::ID = 0;

static RegisterPass<InstrumentationPass> PassObj("PMInstrumenter",
						 											"Perform Instrumentation for Verifier tool");
/*
static cl::opt<bool> PrintRedFlushes("PMVerifier", cl::Hidden,
	 	 	 	 	 	 cl::desc("Instrument code to get redundant flushes"),
						 cl::init(true));

static cl::opt<bool> PMAnalysis("PersistencyAnalysis", cl::Hidden,
						cl::desc("Instrument code to check for correctness of \"
								"implementatioon of persistency model"), cl::init(true));
*/

// The higher order two bytes of the reference IDs for instructions are computed
// using function name and multiplicative hashing technique, more specifically,
// Kernighan and Ritchie's function. One could also use Bernstein's function, in
// which case INITIAL_VALUE below is set to 5381 and MULTIPLIER is set to 33.
//
// For reference, check out https://www.strchr.com/hash_functions.
#define MAX_PREFIX 		((uint32_t)1 << 16)  // Prefix should not exceed 2-bytes
#define INITIAL_VALUE	0   // Because of Kernighan and Ritchie's function
#define MULTIPLIER 		31  // Because of Kernighan and Ritchie's function

static uint32_t ComputeRefIDPrefix(const std::string FuncName) {
	const char *Key = FuncName.c_str();
	uint32_t Hash = INITIAL_VALUE;
	for(unsigned Index = 0; Index != FuncName.size(); ++Index)
		Hash = MULTIPLIER * Hash + Key[Index];
	return ((Hash % MAX_PREFIX) << 16);
}

static void InstrumentWrite(Instruction *I, LLVMContext &Context,
	 													const PMInterfaces<> &PMI, const DataLayout &DL,
														TargetLibraryInfo &TLI, Function *Strlen,
														AllocaInst *WriteIdArray, AllocaInst *WriteAddrArray,
														AllocaInst *WriteSizeArray, Value *WriteIndex,
														DenseMap<const Instruction *, uint32_t>  &InstToIdMap,
													  uint32_t RefIDPrefix, uint32_t &InstCounter) {
	errs() << "INSTRUMENTING WRITE: ";
	I->print(errs());
	errs() << "\n";

	auto *One = ConstantInt::get(Type::getInt64Ty(Context), 1);
	auto *Zero = ConstantInt::get(Type::getInt64Ty(Context), 0);
	auto &PMMI = PMI.getPmemInterface();

// Increment the index
	auto *Index = new LoadInst(Type::getInt64Ty(Context), WriteIndex, "", I);
	auto *NewIndex = BinaryOperator::Create(Instruction::Add, Index, One, "", I);
	new StoreInst(NewIndex, WriteIndex, I);

// Compute the ID for this instruction
	auto Id = RefIDPrefix + InstCounter++;
	//InstToIdMap[I] = Id;
	InstToIdMap.insert(std::make_pair(I, Id));
	errs() << "MAP UPDATED\n";
	errs() << "MAP SIZE: " << InstToIdMap.size() << "\n";

// Index the ID into the write arrays
	std::vector<Value *> IndexVect;
	IndexVect.push_back(Zero);
	IndexVect.push_back(NewIndex);
	auto *IdArrayPtr = GetElementPtrInst::Create(WriteIdArray->getAllocatedType(),
														WriteIdArray, ArrayRef<Value *>(IndexVect), "", I);
	auto *AddrArrayPtr = GetElementPtrInst::Create(WriteAddrArray->getAllocatedType(),
													WriteAddrArray, ArrayRef<Value *>(IndexVect), "", I);
	auto *SizeArrayPtr = GetElementPtrInst::Create(WriteSizeArray->getAllocatedType(),
													WriteSizeArray, ArrayRef<Value *>(IndexVect), "", I);

// Write to the arrays
	auto *IdValue = ConstantInt::get(Type::getInt32Ty(Context), Id);
	new StoreInst(IdValue, IdArrayPtr, I);
	if(auto *SI = dyn_cast<StoreInst>(I)) {
		auto *AddrInt = new PtrToIntInst(SI->getPointerOperand(),
																		 Type::getInt64Ty(Context), "", I);
		new StoreInst(AddrInt, AddrArrayPtr, I);
		auto *Size = ConstantInt::get(Type::getInt64Ty(Context),
											DL.getTypeStoreSize(SI->getValueOperand()->getType()));
		new StoreInst(Size, SizeArrayPtr, I);
		I->getParent()->getParent()->print(errs());
		return;
	}

// This has to be a call instruction
	auto *CI = dyn_cast<CallInst>(I);
	assert(CI && "Error in PM Analysis record results.");

// Check if it is an memory intrinsic or PMDK interface
	if(AnyMemIntrinsic *MI = dyn_cast<AnyMemIntrinsic>(CI)) {
		auto *AddrInt = new PtrToIntInst(MI->getRawDest(),
																		 Type::getInt64Ty(Context), "", I);
		new StoreInst(AddrInt, AddrArrayPtr, I);
		new StoreInst(MI->getLength(), SizeArrayPtr, I);
		I->getParent()->getParent()->print(errs());
		return;
	}

// It is a persistent write
	if(PMMI.isValidInterfaceCall(CI)) {
		auto *AddrInt = new PtrToIntInst(PMMI.getDestOperand(CI),
																		 Type::getInt64Ty(Context), "", I);
		new StoreInst(AddrInt, AddrArrayPtr, I);
		new StoreInst(PMMI.getLengthOperand(CI), SizeArrayPtr, I);
		return;
	}

// Or, it could be a call to a library function
	LibFunc TLIFn;
	auto *Callee = CI->getCalledFunction();
	assert(Callee && "Indirect function call in the set.");
	assert(TLI.getLibFunc(*Callee, TLIFn)
			&& IsValidLibMemoryOperation(*(Callee->getFunctionType()), TLIFn, DL)
			&& "Unknown function in set.");
	auto *AddrInt = new PtrToIntInst(CI->getArgOperand(0),
																	 Type::getInt64Ty(Context), "", I);
	new StoreInst(AddrInt, AddrArrayPtr, I);

// Check if the library function being called has a size operand
	if(Callee->getFunctionType()->getNumParams() >= 3) {
		new StoreInst(CI->getArgOperand(2), SizeArrayPtr, I);
	} else {
	// We do not have a size operand to work with. This is characteristically
	// common for string library functions operating on strings. So we can insert
	// strlen function to get string length. Now, if the string is not null
	// terminated, any operation might cause undefined behaviour. Same is true for
	// strlen on src operand, therefore, it really would not make a huge difference.
		std::vector<Value *> ArgVect;
		ArgVect.push_back(CI->getArgOperand(1));
		auto *StringSize = CallInst::Create(Strlen->getFunctionType(),
									 	 										Strlen, ArrayRef<Value *>(ArgVect), "", I);
		new StoreInst(StringSize, SizeArrayPtr, I);
	}
}

static void InstrumentFlush(Instruction *I, LLVMContext &Context,
	 													const PMInterfaces<> &PMI, const DataLayout &DL,
														AllocaInst *FlushIdArray, AllocaInst *FlushAddrArray,
														AllocaInst *FlushSizeArray, Value *FlushIndex,
														DenseMap<const Instruction *, uint32_t>  &InstToIdMap,
														uint32_t RefIDPrefix, uint32_t &InstCounter) {
	errs() << "INSTRUMENTING FLUSH: ";
	I->print(errs());
	errs() << "\n";

// This has to be a call instruction
	auto *CI = dyn_cast<CallInst>(I);
	assert(CI && "Error in PM Analysis record results.");

	auto *One = ConstantInt::get(Type::getInt64Ty(Context), 1);
	auto *Zero = ConstantInt::get(Type::getInt64Ty(Context), 0);
	auto &FI = PMI.getFlushInterface();
	auto &PI = PMI.getPersistInterface();

// Increment the index
	auto *Index = new LoadInst(Type::getInt64Ty(Context), FlushIndex, "", I);
	auto *NewIndex = BinaryOperator::Create(Instruction::Add, Index, One, "", I);
	new StoreInst(NewIndex, FlushIndex, I);

// Compute the ID for this instruction
	auto Id = RefIDPrefix + InstCounter++;
	//InstToIdMap[I] = Id;
	InstToIdMap.insert(std::make_pair(I, Id));
	errs() << "MAP UPDATED\n";
	errs() << "MAP SIZE: " << InstToIdMap.size() << "\n";

// Index the ID into the write arrays
	std::vector<Value *> IndexVect;
	IndexVect.push_back(Zero);
	IndexVect.push_back(NewIndex);
	auto *IdArrayPtr = GetElementPtrInst::Create(FlushIdArray->getAllocatedType(),
														FlushIdArray, ArrayRef<Value *>(IndexVect), "", I);
	auto *AddrArrayPtr = GetElementPtrInst::Create(FlushAddrArray->getAllocatedType(),
													FlushAddrArray, ArrayRef<Value *>(IndexVect), "", I);
	auto *SizeArrayPtr = GetElementPtrInst::Create(FlushSizeArray->getAllocatedType(),
													FlushSizeArray, ArrayRef<Value *>(IndexVect), "", I);

// Write to the arrays
	auto *IdValue = ConstantInt::get(Type::getInt32Ty(Context), Id);
	new StoreInst(IdValue, IdArrayPtr, I);
	if(FI.isValidInterfaceCall(CI)) {
		auto *AddrInt = new PtrToIntInst(FI.getPMemAddrOperand(CI),
																		 Type::getInt64Ty(Context), "", I);
		new StoreInst(AddrInt, AddrArrayPtr, I);
		new StoreInst(FI.getPMemLenOperand(CI), SizeArrayPtr, I);
		return;
	}
	if(PI.isValidInterfaceCall(CI)) {
		auto *AddrInt = new PtrToIntInst(PI.getPMemAddrOperand(CI),
																		 Type::getInt64Ty(Context), "", I);
		new StoreInst(AddrInt, AddrArrayPtr, I);
		new StoreInst(PI.getPMemLenOperand(CI), SizeArrayPtr, I);
		return;
	}
}

static void InstrumentForPMModelVerifier(Function *F,
									SmallVector<Instruction *, 4> &FencesVect,
									PerfCheckerInfo<> &PerfCheckerWriteInfo,
									PerfCheckerInfo<> &PerfCheckerFlushInfo,
									DenseMap<const Instruction *, uint32_t>  &InstToIdMap,
									const PMInterfaces<> &PMI, TargetLibraryInfo &TLI,
									GenCondBlockSetLoopInfo &GI, Function *FenceEncountered,
									Function *RecordNonStrictWrites, Function *RecordFlushes,
									Function *Strlen) {
	errs() << "START INSTRUMENTING FUNCTION: " << F->getName() << "\n";
// Get the reference ID prefix for the given function
	uint32_t RefIDPrefix = ComputeRefIDPrefix(std::string(F->getName()));

	auto &Context = F->getContext();
	auto *One = ConstantInt::get(Type::getInt64Ty(Context), 1);
	auto *Zero = ConstantInt::get(Type::getInt64Ty(Context), 0);
	auto &DL = F->getParent()->getDataLayout();
	auto &PMMI = PMI.getPmemInterface();

// Allocate the variables to record instruction IDs, operation addresses, size, etc.
	AllocaInst *WriteIdArray;
	AllocaInst *WriteAddrArray;
	AllocaInst *WriteSizeArray;
	AllocaInst *WriteIndex;
	AllocaInst *FlushIdArray;
	AllocaInst *FlushAddrArray;
	AllocaInst *FlushSizeArray;
	AllocaInst *FlushIndex;
	auto *FirstInstInEntryBlock = F->getEntryBlock().getFirstNonPHI();
	if(PerfCheckerWriteInfo.size(F)) {
		auto *WriteArray32Ty = ArrayType::get(Type::getInt32Ty(Context),
																		PerfCheckerWriteInfo.maxSetSize(F));
		auto *WriteArray64Ty = ArrayType::get(Type::getInt64Ty(Context),
																		PerfCheckerWriteInfo.maxSetSize(F));
		WriteIdArray = new AllocaInst(WriteArray32Ty, 0, One, 0,
	 																"", FirstInstInEntryBlock);
		WriteAddrArray = new AllocaInst(WriteArray64Ty, 0, One,
																		0, "", FirstInstInEntryBlock);
		WriteSizeArray = new AllocaInst(WriteArray64Ty, 0, One,
																		0, "", FirstInstInEntryBlock);
		WriteIndex = new AllocaInst(Type::getInt64Ty(Context), 0, One, 0, "",
									  						FirstInstInEntryBlock);
		new StoreInst(Zero, WriteIndex, FirstInstInEntryBlock);
	}
	if(PerfCheckerWriteInfo.size(F)) {
		auto *FlushArray32Ty = ArrayType::get(Type::getInt32Ty(Context),
																		PerfCheckerFlushInfo.maxSetSize(F));
		auto *FlushArray64Ty = ArrayType::get(Type::getInt64Ty(Context),
																		PerfCheckerFlushInfo.maxSetSize(F));
		FlushIdArray = new AllocaInst(FlushArray32Ty, 0, One, 0,
																	"", FirstInstInEntryBlock);
		FlushAddrArray = new AllocaInst(FlushArray64Ty, 0, One,
																		0, "", FirstInstInEntryBlock);
		FlushSizeArray = new AllocaInst(FlushArray64Ty, 0, One,
																		0, "", FirstInstInEntryBlock);
		FlushIndex = new AllocaInst(Type::getInt64Ty(Context), 0, One, 0, "",
									  						FirstInstInEntryBlock);
		new StoreInst(Zero, FlushIndex, FirstInstInEntryBlock);
	}
	errs() << "ALL ALLOCAS ARE INSERTED\n";
	F->print(errs());

// Instrument to record persist operations
	auto RecordOpsBefore = [&](Instruction *I, AllocaInst *OpIdArray,
														 AllocaInst *OpAddrArray, AllocaInst *OpSizeArray,
														 Value *OpIndex, Function *RecordFunc) {
	// Instrument the write
		auto *IdPtrToInt = new PtrToIntInst(OpIdArray,
																				Type::getInt64Ty(Context), "", I);
		auto *AddrPtrToInt = new PtrToIntInst(OpAddrArray,
																					Type::getInt64Ty(Context), "", I);
		auto *SizePtrToInt = new PtrToIntInst(OpSizeArray,
																					Type::getInt64Ty(Context), "", I);
		auto *ArraysSize =
								new LoadInst(Type::getInt64Ty(Context), OpIndex, "", I);
		std::vector<Value *> ArgVect;
		ArgVect.push_back(IdPtrToInt);
		ArgVect.push_back(AddrPtrToInt);
		ArgVect.push_back(SizePtrToInt);
		ArgVect.push_back(ArraysSize);
		CallInst::Create(RecordFunc->getFunctionType(),
										 RecordFunc, ArrayRef<Value *>(ArgVect), "", I);
		new StoreInst(Zero, OpIndex, I);
		F->print(errs());
	};

// Instrument Writes
	uint32_t InstCounter = 0;
	for(PerfCheckerInfo<>::iterator It = PerfCheckerWriteInfo.begin(F);
			It != PerfCheckerWriteInfo.end(F); ++It) {
		auto SerialInsts = *It;
		Instruction *PrevInst = SerialInsts[0];
		GenLoop *L = GI.getLoopFor(SerialInsts[0]->getParent());
		Instruction *PrevInstrumentedInst = nullptr;
		for(auto *I : SerialInsts) {
			InstrumentWrite(I, Context, PMI, DL, TLI, Strlen, WriteIdArray,
											WriteAddrArray, WriteSizeArray, WriteIndex, InstToIdMap,
											RefIDPrefix, InstCounter);
			errs() << "--MAP SIZE: " << InstToIdMap.size() << "\n";
			if(L != GI.getLoopFor(I->getParent())) {
			// Since this is a different loop, record the write
				PrevInstrumentedInst = I;
				RecordOpsBefore(I, WriteIdArray, WriteAddrArray, WriteSizeArray,
													 WriteIndex, RecordNonStrictWrites);
			}
		}

	// Instrument the rest of set if it has not been yet
		auto *LastInstInSet = SerialInsts[SerialInsts.size() - 1];
		if(!PrevInstrumentedInst || PrevInstrumentedInst != LastInstInSet) {
			RecordOpsBefore(LastInstInSet, WriteIdArray, WriteAddrArray,
				 							WriteSizeArray, WriteIndex, RecordNonStrictWrites);
		}
	}

// Instrument Flushes
	for(PerfCheckerInfo<>::iterator It = PerfCheckerFlushInfo.begin(F);
			It != PerfCheckerFlushInfo.end(F); ++It) {
		auto SerialInsts = *It;
		Instruction *PrevInst = SerialInsts[0];
		GenLoop *L = GI.getLoopFor(SerialInsts[0]->getParent());
		Instruction *PrevInstrumentedInst = nullptr;
		for(auto *I : SerialInsts) {
			InstrumentFlush(I, Context, PMI, DL, FlushIdArray, FlushAddrArray,
											FlushSizeArray, FlushIndex, InstToIdMap,
											RefIDPrefix, InstCounter);
			errs() << "--MAP SIZE: " << InstToIdMap.size() << "\n";
			if(L != GI.getLoopFor(I->getParent())) {
			// Since this is a different loop, record the write
				PrevInstrumentedInst = I;
				RecordOpsBefore(I, FlushIdArray, FlushAddrArray, FlushSizeArray,
												FlushIndex, RecordFlushes);
			}
		}

	// Instrument the rest of set if it has not been yet
		auto *LastInstInSet = SerialInsts[SerialInsts.size() - 1];
		if(!PrevInstrumentedInst || PrevInstrumentedInst != LastInstInSet) {
			RecordOpsBefore(LastInstInSet, FlushIdArray, FlushAddrArray,
											FlushSizeArray, FlushIndex, RecordFlushes);
		}
	}

// Iterate over the fences and instrument them
	for(auto *Fence : FencesVect) {
		errs() << "INSTRUMENTING FENCE: ";
		Fence->print(errs());
		errs() << "\n";
	// Assign a static ID to the fence
		auto Id = RefIDPrefix + InstCounter++;
		InstToIdMap.insert(std::make_pair(Fence, Id));
		errs() << "MAP SIZE: " << InstToIdMap.size() << "\n";
		errs() << "FENCE ID: " << Id << "\n";

	// Instrument now
		std::vector<Value *> ArgVect;
		ArgVect.push_back(ConstantInt::get(Type::getInt32Ty(Context), Id));
		CallInst::Create(FenceEncountered->getFunctionType(),
						 				 FenceEncountered, ArrayRef<Value *>(ArgVect), "", Fence);
		errs() << "FENCE INSTRUMENTED\n";
	}
	errs() << "ALL FENCES INSTRUMENTED\n";
	errs() << "+++MAP SIZE: " << InstToIdMap.size() << "\n";
	F->print(errs());
}

static void DefineConstructor(Module &M, LLVMContext &Context,
							 								DenseMap<uint32_t, uint64_t> &InstIdToLineNoMap) {
	errs() << "DEFINING CONSTRUCTOR NOW\n";
// Add constructor
	std::vector<Type *> TypeVect;
	auto *FuncType = FunctionType::get(Type::getVoidTy(Context),
										   ArrayRef<Type *>(TypeVect), 0);
	auto *PMConstructor = Function::Create(FuncType, GlobalValue::ExternalLinkage,
										   "RuntimeInit", &M);
	llvm::appendToGlobalCtors(M, PMConstructor, 0);

// Add a runtime library function to check if the constructor is supposed
// to execute.
	auto *RuntimeConsructorCheck = Function::Create(FuncType,
																									GlobalValue::ExternalLinkage,
																									"RuntimeConsructorCheck", &M);

// Define the constructor now
	auto *EntryBlock = BasicBlock::Create(Context, "", PMConstructor);

// The condblock registers all instruction IDs and their line numbers with the runtime.
// First allocate two arrays: one to put instruction IDs and another for line numbers.
	errs() << "ENTRY BLOCK ADDED\n";
	uint64_t NumInsts = InstIdToLineNoMap.size();
	auto *ArraySize = ConstantInt::get(Type::getInt64Ty(Context), NumInsts);
	auto *IdArray =
			new AllocaInst(Type::getInt32Ty(Context), 0, ArraySize, 0, "", EntryBlock);
	auto *LineArray =
			new AllocaInst(Type::getInt64Ty(Context), 0, ArraySize, 0, "", EntryBlock);
	errs() << "ALLOCAS INSERTED\n";
	uint64_t Index = 0;
	auto *Zero = ConstantInt::get(Type::getInt8Ty(Context), 0);
	std::vector<Value *> IndexVect;
	IndexVect.push_back(Zero);
	for(auto &Pair : InstIdToLineNoMap) {
	// Index into the arrays
		auto ConstantIndex =
				ConstantInt::get(Type::getInt64Ty(Context), NumInsts);
		IndexVect.push_back(ConstantIndex);
		auto *IdIndexedPtr =
				GetElementPtrInst::CreateInBounds(Type::getInt32Ty(Context),
								IdArray, ArrayRef<Value *>(IndexVect), "", EntryBlock);
		auto *LineIndexedPtr =
				GetElementPtrInst::CreateInBounds(Type::getInt64Ty(Context),
								LineArray, ArrayRef<Value *>(IndexVect), "", EntryBlock);
		IndexVect.pop_back();

	// Insert IDs and corresponding line numbers
		auto ConstantId = ConstantInt::get(Type::getInt64Ty(Context), Pair.getFirst());
		auto ConstantLine = ConstantInt::get(Type::getInt64Ty(Context), Pair.getSecond());
		new StoreInst(ConstantId, IdIndexedPtr, EntryBlock);
		new StoreInst(ConstantLine, LineIndexedPtr, EntryBlock);
	}

// Pass the arrays to the runtime function. Create the function first.
	TypeVect.push_back(ArrayType::get(Type::getInt32Ty(Context), NumInsts));
	TypeVect.push_back(ArrayType::get(Type::getInt64Ty(Context), NumInsts));
	TypeVect.push_back(Type::getInt64Ty(Context));
	FuncType = FunctionType::get(Type::getVoidTy(Context),
											   ArrayRef<Type *>(TypeVect), 0);
	auto *RegisterInstructionsInfo = Function::Create(FuncType, GlobalValue::ExternalLinkage,
													  												"RegisterInstructionsInfo", &M);

// Insert a call to register the instructions with the runtime
	std::vector<Value *> ArgVect;
	ArgVect.push_back(IdArray);
	ArgVect.push_back(LineArray);
	ArgVect.push_back(ArraySize);
	CallInst::Create(RegisterInstructionsInfo, ArrayRef<Value *>(ArgVect), "", EntryBlock);

// Create return
	ReturnInst::Create(Context, EntryBlock);
}

bool InstrumentationPass::doInitialization(Module &M) {
	errs() << "INITIALIZING PASS\n";

// Define the functions that we need to insert
	auto &Context = M.getContext();
	std::vector<Type *> TypeVect;
	TypeVect.push_back(Type::getInt32Ty(Context));
	auto *FuncType = FunctionType::get(Type::getVoidTy(Context),
																		 ArrayRef<Type *>(TypeVect), 0);
	FenceEncountered = Function::Create(FuncType, GlobalValue::ExternalLinkage,
																					"FenceEncountered", &M);
	FenceEncountered->setOnlyAccessesInaccessibleMemory();
	TypeVect.clear();
	TypeVect.push_back(Type::getInt64Ty(Context));
	TypeVect.push_back(Type::getInt64Ty(Context));
	TypeVect.push_back(Type::getInt64Ty(Context));
	TypeVect.push_back(Type::getInt64Ty(Context));
	//TypeVect.push_back(Type::getInt32Ty(Context));
	FuncType = FunctionType::get(Type::getVoidTy(Context),
															 ArrayRef<Type *>(TypeVect), 0);
	RecordNonStrictWrites = Function::Create(FuncType, GlobalValue::ExternalLinkage,
																					 "RecordNonStrictWrites", &M);
	RecordNonStrictWrites->setOnlyAccessesInaccessibleMemory();
	RecordStrictWrites = Function::Create(FuncType, GlobalValue::ExternalLinkage,
																				"RecordStrictWrites", &M);
	RecordStrictWrites->setOnlyAccessesInaccessibleMemory();
	RecordFlushes = Function::Create(FuncType, GlobalValue::ExternalLinkage,
																	 "RecordFlushes", &M);
	RecordFlushes->setOnlyAccessesInaccessibleMemory();

// We might been strlen function in the string library
	TypeVect.clear();
	TypeVect.push_back(PointerType::get(Type::getInt8Ty(Context), 0));
	FuncType = FunctionType::get(Type::getInt64Ty(Context),
															 ArrayRef<Type *>(TypeVect), 0);

	auto *StrlenCallee = M.getOrInsertFunction(StringRef("strlen"), FuncType);
	Strlen = cast<Function>(StrlenCallee);
	if(!Strlen) {
	// A bitcast of the function was returned. Strip the cast.
		Strlen = cast<Function>(StrlenCallee->stripPointerCasts());
		assert(Strlen && "Error in getting strlen declaration.");
	}
	errs() << "PASS INITIALIZED\n";
	return false;
}

bool InstrumentationPass::doFinalization(Module &M) {
	errs() << "PRINTING MODULE: ";
	M.print(errs(), nullptr);
// Now define the constructors and destructors
	DefineConstructor(M, M.getContext(), InstIdToLineNoMap);
	//DefineDestructor(M, Context);
	errs() << "PRINTING MODULE AGAIN:";
	M.print(errs(), nullptr);
	return false;
}

bool InstrumentationPass::runOnFunction(Function &F) {
	if(!F.size())
		return false;

	errs() << "RUNNING INSTRUMENTER\n";
	DenseMap<const Instruction *, uint32_t> InstToIdMap;

	//if(PMAnalysis) {
	// Get PM Model info
	auto PerfCheckerWriteInfo =
					getAnalysis<ModelVerifierWrapperPass>().getPerfCheckerWriteInfo();
	errs() << "GOT PERF WRITE INFO\n";
	auto PerfCheckerFlushInfo =
					getAnalysis<ModelVerifierWrapperPass>().getPerfCheckerFlushInfo();
	errs() << "GOT PERF CHECKER FLUSH INFO\n";
	auto &PMI = getAnalysis<ModelVerifierWrapperPass>().getPmemInterfaces();
	errs() << "GOT MODEL VERIFIER RESULTS\n";
	auto &GI =
		getAnalysis<GenCondBlockSetLoopInfoWrapperPass>().getGenCondInfoWrapperPassInfo();
	auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
	errs() << "GOT CONDBLOCK SET AND LOOP INFO\n";

	auto FencesVect = getAnalysis<ModelVerifierWrapperPass>().getFencesInfoFor(&F);
	errs() << "GOT FENCE VECTOR\n";
	for(auto *Fence : FencesVect) {
		errs() << "FENCE INST: ";
		Fence->print(errs());
		errs() << "\n";
	}

	InstrumentForPMModelVerifier(&F, FencesVect, PerfCheckerWriteInfo,
															 PerfCheckerFlushInfo, InstToIdMap,
															 PMI, TLI, GI, FenceEncountered,
															 RecordNonStrictWrites, RecordFlushes, Strlen);

// Get line number of an instruction
	LLVMContext &Context = F.getContext();
	auto GetLineNumber = [&Context](const Instruction *I) {
		if(MDNode *N = I->getMetadata("dbg")) {
		if(DILocation *Loc = dyn_cast<DILocation>(N))
			return ConstantInt::get(Type::getInt32Ty(Context), Loc->getLine());
		}
		return (ConstantInt *)nullptr;
	};

	errs() << "MAP SIZE: " << InstToIdMap.size() << "\n";
	for(auto &InstToIdPair : InstToIdMap) {
		ConstantInt *CI = GetLineNumber(InstToIdPair.getFirst());
		InstIdToLineNoMap.insert(std::make_pair(InstToIdPair.getSecond(),
																						CI->getSExtValue()));
		errs() << "ID: " << InstToIdPair.getSecond() << " ";
		errs() << "Line: " << CI->getSExtValue() << "\n";
	}

	errs() << "DONE\n";
	//}
	return true;
}
