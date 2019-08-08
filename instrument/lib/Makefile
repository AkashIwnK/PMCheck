# Rules to compile the instrumentor

CC = clang
CXX  = clang++

#SOURCES = CommonSCCOps.cpp GenCondInfo.cpp WriteAliasCheck.cpp PMFencesChecker.cpp \
#					PMRedFlushesChecker.cpp PMModelVerifier.cpp
SOURCES = $(wildcard *.cpp)

PM_VERIFY_SOURCES = CommonSCCOps.cpp GenCondInfo.cpp WriteAliasCheck.cpp \
										PMModelVerifier.cpp FlowAwarePostOrder.cpp LibFuncValidityCheck.cpp


OPTIMIZATION = -O2
CC_FLAGS =  `llvm-config --cflags --ldflags --libs --system-libs` -g $(OPTIMIZATION) -lm

OBJECT_FILES = $(SOURCES:%.cpp=%.o)
PM_VERIFY_OBJ_FILES = $(PM_VERIFY_SOURCES:%.cpp=%.o)

EXEBASE = PMCheck
SHARED_LIB = $(addsuffix .so, $(EXEBASE))
PM_VERIFY_EXEBASE = PMModelVerify
PM_VERIFY_SHARED_LIB = $(addsuffix .so, $(PM_VERIFY_EXEBASE))

.SUFFIXES: .o .cpp .so

.PHONY = all model_verify

model_verify: $(PM_VERIFY_OBJ_FILES)
	$(CXX) -shared -o $(PM_VERIFY_SHARED_LIB) $(PM_VERIFY_OBJ_FILES)

all: $(OBJECT_FILES)
	$(CXX) -shared -o $(SHARED_LIB) $(OBJECT_FILES) -I../include $(CC_FLAGS)

%.o: %.cpp
	$(CXX) -o $@ -c $< -fPIC -w $(CC_FLAGS) -I../include

clean:
	rm -rf *.o *.so
