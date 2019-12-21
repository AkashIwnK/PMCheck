# Rules to compile the instrumentor

CC = clang
CXX  = clang++

#SOURCES = CommonSCCOps.cpp GenCondInfo.cpp WriteAliasCheck.cpp PMFencesChecker.cpp \
#					PMRedFlushesChecker.cpp PMModelVerifier.cpp
SOURCES = $(wildcard *.cpp)

PM_VERIFY_SOURCES = CommonSCCOps.cpp GenCondInfo.cpp WriteAliasCheck.cpp \
										TraceAnalysis.cpp FlowAwarePostOrder.cpp LibFuncValidityCheck.cpp

DSA = DataStructureAliasAnalysis.cpp LocalGraph.cpp CBUGraph.cpp TBCGraph.cpp DataStructure.cpp


OPTIMIZATION = -O2
CC_FLAGS =  `llvm-config --cflags --ldflags --libs --system-libs` -g $(OPTIMIZATION) -lm -fno-rtti -std=c++11

OBJECT_FILES = $(SOURCES:%.cpp=%.o)
PM_VERIFY_OBJ_FILES = $(PM_VERIFY_SOURCES:%.cpp=%.o)
DSA_OBJ_FILES = $(DSA:%.cpp=%.o)

EXEBASE = PMCheck
SHARED_LIB = $(addsuffix .so, $(EXEBASE))
PM_VERIFY_EXEBASE = PMModelVerify
PM_VERIFY_SHARED_LIB = $(addsuffix .so, $(PM_VERIFY_EXEBASE))
DSA_EXEBASE = DSA
DSA_SHARED_LIB = $(addsuffix .so, $(DSA_EXEBASE))


.SUFFIXES: .o .cpp .so

.PHONY = all model_verify

loop_pass: GenCondInfo.o
	$(CXX) -shared -o GenCondInfo.so $^

dsa: $(DSA_OBJ_FILES)
	$(CXX) -shared -o $(DSA_SHARED_LIB) $(DSA_OBJ_FILES)

model_verify: $(PM_VERIFY_OBJ_FILES)
	$(CXX) -shared -o $(PM_VERIFY_SHARED_LIB) $(PM_VERIFY_OBJ_FILES)

all: $(OBJECT_FILES)
	$(CXX) -shared -o $(SHARED_LIB) $(OBJECT_FILES) #-I../include $(CC_FLAGS)

%.o: %.cpp
	$(CXX) -o $@ -c $< -fPIC -w $(CC_FLAGS) -I../include

clean:
	rm -rf *.o *.so
