//============================= Run-time Checker =============================//
//
//============================================================================//
//
// This has all the interval trees for runtime checks for looking
// for performance bugs and looking for persistent memory mapping ranges.
//
//============================================================================//

#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

#include "IntervalTree.h"


// This maps the instruction IDs with their line numbers
using DebugInfoRecord = std::unordered_map<uint32_t, uint32_t>;

// This maps the context (call site) id to the name of the function is being invoked
using ContextNameRecord = std::unordered_map<uint32_t, std::string>;

// This puts all the memory ranges allocated in persistent memory in an interval
// tree. This is essentially shadow memory for us to make sure which addresses being
// written to lie in persistent memory.
using PMRecord = IntervalTree<true>;

// This class records information about persist operations i.e. writes and flushes.
// It contains all the necessary information regarding instruction IDs, addresses
// ranges, context IDs and time stamp of when those instructions were executed.
class OpRecord {
// Vector of tuples containing instruction ID, time stamp and context ID
	typedef std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> OpIdInfoTy;

// Tuple containing interval pair, time stamp, and context ID
	typedef std::tuple<std::pair<uint64_t, uint64_t>, uint32_t, uint32_t> OpIdTupleInfoTy;

// Interval Tree to record intervals
	IntervalTree<true> OpIntervalTree;

// Use a hash table for recording instruction IDs, their context and their
// corresponding intervals. It records a history of all the operations performed
// on interval trees.
	std::unordered_map<std::pair<uint64_t, uint64_t>, OpIdInfoTy> RangeToOpIdsHashMap;

// Maintain a map that maintains the flushes and their ranges. This is for the slow
// access to information, when the information from the hash map is not enough.
// A single instruction can operate on multiple intervals.
	std::map<uint32_t , std::vector<OpIdTupleInfoTy>> OpIdToInfoMap;

// Vector of intervals. This is not used until interval tree iterators are used.
	std::vector<std::pair<uint64_t, uint64_t>> IntervalVect;

public:
	OpRecord() : OpIntervalTree(), RangeToOpIdsHashMap() , OpIdToInfoMap() {}

	~OpRecord() {
		~RangeToOpIdsHashMap();
		~OpIdToInfoMap();
		~OpIntervalTree();
	}

	ITResult::OverlapResult insert(uint32_t Id, uint64_t StartAddr, uint64_t Size,
																 uint32_t TimeStamp, uint32_t Context) {
	// Add the interval to the interval tree
		ITResult Result = OpIntervalTree.insert(Start, Start + Size);

	// Add the information to the map
		auto Pair = std::make_pair(StartAddr, Start + Size);
		OpIdToInfoMap[Id].push_back(std::make_tuple(Pair, TimeStamp, Context));

	// Add the new interval	to the hash map
		auto OR = Result.getOverlapResult();
		switch(OR)) {
			case ITResult::CompleteOverlap:

			case ITResult::CompletlyPerfectOverlap:

			case ITResult::NoOverlap:
				auto Pair = std::make_pair(Result.getNode(0)->Start, Result.getNode(0)->End);
				RangeToOpIdsHashMap[Pair].push_back(std::make_tuple(Id, TimeStamp, Context));
				return OR;

			case ITResult::PartialOverlap:
			// This is trickier because we will have to update the interval as well
				ITResult::NodeState PrevState = Result.getPreviousNodeRange(0);
				auto OldPair = std::make_pair(PrevState.Start, PrevState.End);
				auto NewPair = std::make_pair(Result.getNode(0)->Start, Result.getNode(0)->End);
				RangeToOpIdsHashMap[NewPair] = RangeToOpIdsHashMap[OldPair];
				RangeToOpIdsHashMap[OldPair].clear();
				RangeToOpIdsHashMap[NewPair].push_back(std::make_tuple(Id, TimeStamp, Context));
				return OR;
		}
		return OR;
	}

	ITResult searchInterval(uint64_t StartAddr, uint64_t EndAddr) const {
		return OpIntervalTree.getSearchDetails(StartAddr, EndAddr);
	}

	std::vector<std::pair<uint64_t, uint64_t>> &getIntervalsFor(uint32_t Id) const {
		std::vector<std::pair<uint64_t, uint64_t>> IntervalPairVect;
		for(auto &Tuple : OpIdToInfoMap[Id)
			IntervalPairVect.push_back(std::get<0>(Tuple));
		return IntervalPairVect;
	}

	std::vector<uint32_t> getTimeStampsFor(uint32_t Id) const {
		std::vector<uint32_t> TimeStampsVect;
		for(auto &Tuple : OpIdToInfoMap[Id)
			TimeStampsVect.push_back(std::get<1>(Tuple));
		return TimeStampsVect;
	}

	std::vector<uint32_t> getContextsFor(uint32_t Id) const {
		std::vector<uint32_t> ContextsVect;
		for(auto &Tuple : OpIdToInfoMap[Id)
			ContextsVect.push_back(std::get<2>(Tuple));
		return ContextsVect;
	}

	OpIdInfoTy &getIdAndContextAndTimeStampFor(uint64_t Start, uint64_t End) const {
		return RangeToOpIdsHashMap[std::make_pair(Start, End)];
	}

	void clear() {
	// Clear everything
		RangeToOpIdsHashMap.clear();
		OpIntervalTree.clear();
		OpIdToInfoMap.clear();
	}

	bool empty() {
	// The record depends on the interval tree primarily
		return OpIntervalTree.empty();
	}

	ITResult remove(uint64_t Start, uint64_t End) {
		auto Result = OpIntervalTree.getRemoveDetails(Start, End);
		auto OR = Result.getOverlapResult();
		switch(OR) {
			case ITResult::CompletlyPerfectOverlap:
			// Entire range is removed. Remove node info too.
				auto PrevState = Result.getPreviousNodeRange(0);
				auto OldPair = std::make_pair(PrevState.Start, PrevState.End);
				//RangeToOpIdsHashMap[OldPair].clear();
				return Result;

			case ITResult::CompleteOverlap:
			// Update the node info. Note that if there is an ID where the interval
			// associated with it is removed from the interval tree, we do not remove
			// the ID from the hash map because that would require standard map look up
			// which can be really slow.
				auto PrevState = Result.getPreviousNodeRanges(0);
				auto OldPair = std::make_pair(PrevState.Start, PrevState.End);
				if(PrevState.size() == 2) {
				// The interval was split somewhere in the middle
					auto NewPair1 = std::make_pair(Result.getNode(0)->Start, Result.getNode(0)->End);
					auto NewPair2 = std::make_pair(Result.getNode(1)->Start, Result.getNode(1)->End);
					RangeToOpIdsHashMap[NewPair1] = RangeToOpIdsHashMap[OldPair];
					RangeToOpIdsHashMap[NewPair2] = RangeToOpIdsHashMap[OldPair];
				} else {
					auto NewPair = std::make_pair(Result.getNode(0)->Start, Result.getNode(0)->End);
					RangeToOpIdsHashMap[NewPair] = RangeToOpIdsHashMap[OldPair];
				}
				//RangeToOpIdsHashMap[OldPair].clear();
				return Result;

			case ITResult::PartialCompleteOverlap:
			// Update the nodes info. Note that if there is an ID where the interval
			// associated with it is removed from the interval tree, we do not remove
			// the ID from the hash map because that would require standard map look up
			// which can be really slow.
				for(uint32_t Index = 0; Index != Result.getNumOverlapNodes(); ++Index) {
					auto PrevState = Result.getPreviousNodeRange(Index);
					auto OldPair = std::make_pair(PrevState.Start, PrevState.End);
					auto NewPair = std::make_pair(Result.getNode(Index)->Start,
																				Result.getNode(Index)->End);
					RangeToOpIdsHashMap[NewPair] = RangeToOpIdsHashMap[OldPair];
					//RangeToOpIdsHashMap[OldPair].clear();
				}
				return Result;
		}
		return Result;
	}

// Iterators for the standard map
	using iterator =
			typename std::map<uint32_t , OpIdTupleInfoTy>::iterator;
	using reverse_iterator =
			typename std::map<uint32_t , OpIdTupleInfoTy>>::reverse_iterator;
	using const_iterator =
			typename std::map<uint32_t , OpIdTupleInfoTy>::const_iterator;

	iterator begin() {
		return OpIdToInfoMap.begin();
	}

	iterator end() {
		return OpIdToInfoMap.end();
	}

	reverse_iterator rbegin() {
		return OpIdToInfoMap.rbegin();
	}

	reverse_iterator rend() {
		return OpIdToInfoMap.rend();
	}

// Iterators for the interval tree
		using IT_iterator = typename std::vector<std::pair<uint64_t, uint64_t>>::iterator;

		IT_iterator IT_begin() {
			IntervalVect = OpIntervalTree.getIntervals();
			return IntervalVect.begin();
		}

		IT_iterator IT_end() {
			return IntervalVect.end();
		}
};

// Instantiate all the records as globals
OpRecord WR;
OpRecord FR;
DebugInfoRecord DIR;
ContextNameRecord CNR;
PMRecord PMR;

// A vecrtor to keep track of all the calling contexts
std::vector<uint32_t> ContextVect;

void AddContext(uint32_t Context) {
	ContextVect.push_back(Context);
}

void RemoveContext() {
	ContextVect.pop_back();
}

void RegisterDebugInfo(uint32_t *OpArray, uint32_t *LineNumArray, uint32_t N) {
	for(uint32_t Index = 0; Index != N; ++Index)
		DIR.insert(std::make_pair(OpArray[Index], LineNumArray[Index]));
}

void RegisterContextNameInfo(uint32_t *CallSiteIdArray, char *[] NamesArray, uint32_t N) {
	for(uint32_t Index = 0; Index != N; ++Index) {

	}
}

void AllocatePM(uint64_t Addr, uint64_t Size) {
	PMR.insert(Addr, Addr + Size);
}

// Use this for writes that are not supposed to follow strict persistency
void RecordNonStrictWrites(uint32_t *IdArray, uint64_t *AddrArray,
													 uint64_t *SizeArray, uint32_t *TimeArray, uint32_t N) {
	for(uint32_t Index = 0; Index != N ; ++Index) {
		if(!PMR.search<true>(AddrArray[Index], SizeArray[Index]))
			continue;
		auto OR = WR.insert(IdArray[Index], AddrArray[Index], SizeArray[Index],
				  							TimeArray[Index], ContextVect.back());

	// Check if the write overlaps with any executed write, throw an error
		if(OR != ITResult::NoOverlap) {
			errs() << "Write at line " << DIR[IdArray[Index]] << " that writes from "
					   << AddrArray[Index] << " upto size " << SizeArray[Index] << " in a function "
					   << CNR[ContextVect.back()] << " invoked from line"
					   << DIR[ContextVect.back()] << " writes .\n";
			exit(-1);
		}
	}
}

// Use this for writes that are supposed to follow strict persistency
void RecordStrictsWrites(uint32_t *IdArray, uint64_t *AddrArray,
	  	   	   	   	     	 uint64 *SizeArray, uint32_t *TimeArray, uint32_t N) {
	for(uint32_t Index = 0; Index != N ; ++Index) {
		if(!PMR.search<true>(AddrArray[Index], SizeArray[Index]))
			continue;
		if(WR.size() == 1) {
		// Throw an error since strict persistency requires one write to persist
		// at a time.
			errs() << "Write at line " << DIR[IdArray[Index]] << " that writes from "
				   << AddrArray[Index] << " upto size " << SizeArray[Index] << " in a function "
				   << CNR[Context] << " invoked from line"
				   << DIR[Context] << " is immediately preceded by a perisistent write "
				   << "and therefore does not conform with strict persistency as required.\n";
			exit(-1);
		}
		auto OR = WR.insert(IdArray[Index], AddrArray[Index], SizeArray[Index],
				  		TimeArray[Index], ContextVect.back());

	// Check if the write address range overlaps with other writes
		if(OR != ITResult::NoOverlap) {
			errs() << "Write at line " << DIR[IdArray[Index]] << " that writes from "
						 << AddrArray[Index] << " upto size " << SizeArray[Index] << " in a function "
						 << CNR[ContextVect.back()] << " invoked from line"
						 << DIR[ContextVect.back()] << " writes .\n";
			exit(-1);
		}
	}
}

void RecordFlushes(uint32_t *IdArray, uint64_t *AddrArray, uint64 *SizeArray,
									 uint32_t *TimeArray, uint32_t N) {
	for(uint32_t Index = 0; Index != N ; ++Index) {
		FR.insert(IdArray[Index], AddrArray[Index], SizeArray[Index],
				  		TimeArray[Index], ContextVect.back());
	}
}

static void PrintForRedundancyFlushes() {
	// Print redundant flushes
		for(auto It = FR.IT_begin(); It != FR.IT_end(); It++) {
			auto IntervalPair = *It;
			uint64_t Start = std::get<0>(IntervalPair);
			uint64_t End = std::get<1>(IntervalPair);

		// Get the ids of the flushes that this interval corresponds to
			auto FlushIdAndContextAndTimeStampVect = FR.getIdAndContextAndTimeStampFor(Start, End);
			if(FlushIdAndContextAndTimeStampVect.size() == 1) {
				auto FlushId = std::get<0>(FlushIdAndContextAndTimeStampVect[0]);
				auto ContextId = std::get<1>(FlushIdAndContextAndTimeStampVect[0]);
				errs() << "Flush at line " << DIR[FlushId] << "in a function "
							 << CNR[ContextId] << " invoked from line " << DIR[ContextId]
							 << " is completely redudant.\n";
				continue;
			}

		// Look up for the what ids actually that flush this interval
			for(auto FlushIdAndContextAndTimeStampTuple : FlushIdAndContextAndTimeStampVect) {
			// See if the interval for this Id actually overlaps with this interval
				auto FlushId = std::get<0>(FlushIdAndContextAndTimeStampTuple);
				for(auto &IdIntervalPair : FR.getIntervalsFor(FlushId)) {
					auto IdIntervalStart = std::get<0>(IdIntervalPair);
					auto IdIntervalEnd = std::get<1>(IdIntervalPair);

				// Look for complete overlap
					if(Start < IdIntervalEnd && IdIntervalStart < End) {
						auto ContextId = std::get<1>(FlushIdAndContextAndTimeStampTuple);
						errs() << "Flush at line " << DIR[FlushId] << " flushing between "
									 << IdIntervalStart << " and " << IdIntervalEnd
									 << " in a function " << CNR[ContextId] << " invoked from line "
							   	 << DIR[ContextId] << " is completely redudant.\n";
						continue;
					}

				// Look for partial overlap
					if(Start < IdIntervalEnd || IdIntervalStart < End) {
						auto ContextId = std::get<1>(FlushIdAndContextAndTimeStampTuple);
						errs() << "Flush at line " << DIR[FlushId] << " flushing between "
									 << IdIntervalStart << " and " << IdIntervalEnd
									 << " in a function " << CNR[ContextId] << " invoked from line "
							   	 << DIR[ContextId] << " is partially redudant.\n";
						continue;
					}

				// This means that there is no overlap, so we can just move on.
				}
			}
		}
}

static bool CheckOutOfOrderPersistOps(ITResult Result, uint32_t WriteId,
								uint64_t WriteStart, uint64_t WriteEnd, uint32_t WriteTimeStamp,
								std::vector<std::pair<uint32_t,
														std::pair<uint64_t, uint64_t>>> *FlushesInfoVectPtr = nullptr) {
	bool Ret = false;
	for(uint32_t Index = 0; Index != Result.getPreviousNodeRangeSize(); Index++) {
		auto PrevState = Result.getPreviousNodeRange(Index);
		auto FlushIdAndContextAndTimeStampVect =
						FR.getIdAndContextAndTimeStampFor(PrevState.Start, PrevState.End);
		if(FlushIdAndContextAndTimeStampVect.size() == 1) {
			auto FlushId = std::get<0>(FlushIdAndContextAndTimeStampVect[0]);
			auto ContextId = std::get<1>(FlushIdAndContextAndTimeStampVect[0]);
			auto FlushTimeStamp = std::get<2>(FlushIdAndContextAndTimeStampVect[0]);
			if(FlushTimeStamp < WriteTimeStamp) {
			// The flush executes before writes
				errs() << "Flush at line " << DIR[FlushId] << "in a function "
							 << CNR[ContextId] << " invoked from line "
							 << DIR[ContextId] << " executes before write at "
						 	 << DIR[WriteId] << "\n";
				Ret = true;
			}
			continue;
		}

		for(auto FlushIdAndContextAndTimeStampTuple : FlushIdAndContextAndTimeStampVect) {
		// See if the interval for this Id actually overlaps with this interval
			auto FlushId = std::get<0>(FlushIdAndContextAndTimeStampTuple);
			for(auto &IdIntervalPair : FR.getIntervalsFor(FlushId)) {
				auto IdIntervalStart = std::get<0>(IdIntervalPair);
				auto IdIntervalEnd = std::get<1>(IdIntervalPair);

			// Look for some overlap
				if((WriteStart < IdIntervalEnd && IdIntervalStart < WriteEnd)
				|| (WriteStart < IdIntervalEnd || IdIntervalStart < WriteEnd)) {
					if(FlushesInfoVectPtr) {
						auto Pair = std::make_pair(FlushId,
																	std::make_pair(IdIntervalStart, IdIntervalEnd);
						(*FlushesInfoVectPtr).push_back(Pair));
					}
					auto ContextId = std::get<1>(FlushIdAndContextAndTimeStampTuple);
					auto FlushTimeStamp = std::get<2>(FlushIdAndContextAndTimeStampTuple);
					if(FlushTimeStamp < WriteTimeStamp) {
					// The flush executes before writes
						errs() << "Flush at line " << DIR[FlushId] << " flushing between "
									 << IdIntervalStart << " and " << IdIntervalEnd
									 << "in a function " << CNR[ContextId] << " invoked from line "
									 << DIR[ContextId] << " executes before write at "
									 << DIR[WriteId] << " writing between " << WriteStart
									 << " and " << WriteEnd << "\n";
						Ret = true;
					}
				}

			// This means that there is no overlap, so we can just move on.
			}
		}
	}
	return Ret;
}

// This is the slowest way of dealing with persists when fences are encountered
void FenceEncountered(uint32_t FenceId) {
	if(WR.empty() && FR.empty()) {
	// This is a redundant fence
		errs() << "Fence at line " << DIR[FenceId] << " is redundant.\n";
		exit(-1);
	}

	if(WR.empty()) {
	// All the recorded flushes are redundant
		for(auto MapElem : FR) {
			auto FlushId = MapElem.first;
			auto ContextId = std::get<2>(MapElem.second);
			errs() << "Flush at line " << DIR[FlushId] << " is redundant "
				   << "in a function " << CNR[ContextId] << " invoked from line "
				   << DIR[ContextId] << " is redudant.\n";
		}
		exit(-1);
	}

	if(FR. empty ()) {
	// Writes have not been flushed
		for(auto WriteInfo : WR) {
			auto Tuple = std::get<1>(WriteInfo);
			errs() << "Write at line " << DIR[std::get<0>(WriteInfo)] << " that writes from "
				   << std::get<0>(Tuple) << " upto size " << std::get<1>(Tuple) - std::get<0>(Tuple)
				   << " in a function " << CNR[std::get<2>(Tuple)] << " invoked from line"
				   << DIR[std::get<2>(Tuple)] << " is not flushed.\n";
		}
		exit(-1);
	}

// Iterate over all writes and see whether they have been flushed
	for(auto It = WR.IT_begin(); It != WR.IT_end(); It++) {
		auto IntervalPair = *It;
		uint64_t WriteStartAddr = std::get<0>(IntervalPair);
		uint64_t WriteEndAddr = std::get<1>(IntervalPair);
		auto Result = FR.remove(WriteStartAddr, WriteEndAddr);
		switch(Result.getOverlapResult()) {
			case ITResult::NoOverlap:
			// Since there is no overlap, throw an error
				auto WriteIdAndContextAndTimeStampVect =
										WR.getIdAndContextAndTimeStampFor(WriteStart, WriteEndAddr);
				if(WriteIdAndContextAndTimeStampVect.size() == 1) {
					auto WriteId = std::get<0>(WriteIdAndContextAndTimeStampVect[0]);
					auto ContextId = std::get<1>(WriteIdAndContextAndTimeStampVect[0]);
					errs() << "Write at line " << DIR[WriteId] << " that writes from "
						   	 << WriteStartAddr << " upto size " << WriteEndAddr - WriteStartAddr
						   	 << " in a function " << CNR[ContextId] << " invoked from line"
						   	 << DIR[ContextId] << " is not flushed.\n";
				} else {
					for(auto WriteIdAndContextAndTimeStampTuple : WriteIdAndContextAndTimeStampVect) {
					// See if the interval for this Id actually overlaps with this interval
						auto WriteId = std::get<0>(WriteIdAndContextAndTimeStampTuple);
						auto ContextId = std::get<1>(WriteIdAndContextAndTimeStampTuple);
						errs() << "Write at line " << DIR[WriteId] << " in a function "
						       << CNR[ContextId] << " invoked from line"
							   	 << DIR[ContextId] << " is not flushed.\n";
					}
				}
				exit(-1);

			case ITResult::PartialOverlap:
			// Since there is partial overlap, throw an error
				auto WriteIdAndContextAndTimeStampVect =
										WR.getIdAndContextAndTimeStampFor(WriteStart, WriteEndAddr);
				if(WriteIdAndContextAndTimeStampVect.size() == 1) {
					auto WriteId = std::get<0>(WriteIdAndContextAndTimeStampVect[0]);
					auto ContextId = std::get<1>(WriteIdAndContextAndTimeStampVect[0]);
					errs() << "Write at line " << DIR[WriteId] << " that writes from "
						   	 << WriteStartAddr << " upto size " << WriteEndAddr - WriteStartAddr
						   	 << " in a function " << CNR[ContextId] << " invoked from line"
						   	 << DIR[ContextId] << " is partially flushed.\n";

				 // Also check if the flushes happened before the writes did
				 	auto WriteTimeStamp = std::get<2>(WriteIdAndContextAndTimeStampVect[0]);
				 	CheckOutOfOrderPersistOps(Result, WriteId, WriteStartAddr,
																		WriteEndAddr, WriteTimeStamp);
				} else {
					for(auto WriteIdAndContextAndTimeStampTuple : WriteIdAndContextAndTimeStampVect) {
					// See if the interval for this Id actually overlaps with this interval
						auto WriteId = std::get<0>(WriteIdAndContextAndTimeStampTuple);
						auto ContextId = std::get<1>(WriteIdAndContextAndTimeStampTuple);
						for(auto &IdIntervalPair : WR.getIntervalsFor(WriteId)) {
							auto IdIntervalStart = std::get<0>(IdIntervalPair);
							auto IdIntervalEnd = std::get<1>(IdIntervalPair);
							if(IdIntervalStart < WriteEndAddr && IdIntervalEnd > WriteStartAddr) {
								errs() << "Write at line " << DIR[WriteId] << " that writes from "
											 << IdIntervalStart << " upto size " << IdIntervalEnd - IdIntervalStart
											 << " in a function " << CNR[ContextId] << " invoked from line"
											 << DIR[ContextId] << " is partially flushed.\n";

							 // Also check if the flushes happened before the writes did
							 	auto WriteTimeStamp = std::get<2>(WriteIdAndContextAndTimeStampTuple);
								CheckOutOfOrderPersistOps(Result, WriteId, IdIntervalStart,
																					IdIntervalEnd, WriteTimeStamp);
							}
						}
					}

				// Print any flushes that could possibly me merged
					if(FlushesVect.size() > 1) {
						for(auto FlushesInfoPair : FlushesInfoVect) {
							auto FlushId = std::get<0>(FlushesInfoPair);
							auto FlushIntervalPair = std::get<1>(FlushesInfoPair);
							auto FlushStartAddr = std::get<0>(FlushIntervalPair);
							auto FlushEndAddr = std::get<1>(FlushIntervalPair);
							errs() << "Flushes at line " << DIR[FlushId]
										 << " flushing between " << FlushStartAddr << " and "
										 << FlushEndAddr << " can be merged.\n";
						}
					}
				}
				exit(-1);

			case ITResult::CompletlyPerfectOverlap:

			case ITResult::CompleteOverlap:

			case ITResult::PartialCompleteOverlap:
				bool Ret;
				auto WriteIdAndContextAndTimeStampVect =
									WR.getIdAndContextAndTimeStampFor(WriteStart, WriteEndAddr);
				if(WriteIdAndContextAndTimeStampVect.size() == 1) {
					auto WriteId = std::get<0>(WriteIdAndContextAndTimeStampVect[0]);
				 	auto WriteTimeStamp = std::get<2>(WriteIdAndContextAndTimeStampVect[0]);
				 	Ret = CheckOutOfOrderPersistOps(Result, WriteId, WriteStartAddr,
																					WriteEndAddr, WriteTimeStamp);
				} else {
				// This means that the write range is written by multiple write IDs.
				// So now we need to spit the flush IDs that overlap with specific writes.
				// We record the flushes that
					std::vector<std::pair<uint32_t, std::pair<uint64_t, uint64_t>>> FlushesInfoVect;
					for(auto WriteIdAndContextAndTimeStampTuple : WriteIdAndContextAndTimeStampVect) {
						auto WriteId = std::get<0>(WriteIdAndContextAndTimeStampTuple);
						for(auto &IdIntervalPair : WR.getIntervalsFor(WriteId)) {
							auto IdIntervalStart = std::get<0>(IdIntervalPair);
							auto IdIntervalEnd = std::get<1>(IdIntervalPair);
							if(IdIntervalStart < WriteEndAddr && IdIntervalEnd > WriteStartAddr) {
								auto WriteTimeStamp = std::get<2>(WriteIdAndContextAndTimeStampTuple);
								Ret = CheckOutOfOrderPersistOps(Result, WriteId, IdIntervalStart,
																			IdIntervalEnd, WriteTimeStamp, &FlushesInfoVect);
							}
						}
					}

				// Print any flushes that could possibly me merged
					if(FlushesVect.size() > 1) {
						for(auto FlushesInfoPair : FlushesInfoVect) {
							auto FlushId = std::get<0>(FlushesInfoPair);
							auto FlushIntervalPair = std::get<1>(FlushesInfoPair);
							auto FlushStartAddr = std::get<0>(FlushIntervalPair);
							auto FlushEndAddr = std::get<1>(FlushIntervalPair);
							errs() << "Flushes at line " << DIR[FlushId]
										 << " flushing between " << FlushStartAddr << " and "
										 << FlushEndAddr << " can be merged.\n";
							Ret = true;
						}
					}
				}

			// Terminate if we threw any errors
				if(Ret)
					exit(-1);

				break;
		}
	}

// Print the redundant flushes
	PrintForRedundancyFlushes();

// Empty records
	WR.clear();
	FR.clear();
}
