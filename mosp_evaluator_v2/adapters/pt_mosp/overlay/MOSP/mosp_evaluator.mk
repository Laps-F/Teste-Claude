CXX ?= g++
CXXFLAGS ?= -O3 -std=c++20 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
LIBDIR := mosp_evaluator
LIBSRC := $(LIBDIR)/src/evaluator.cpp $(LIBDIR)/src/adaptive.cpp
LIBHEAD := $(wildcard $(LIBDIR)/include/mosp/*.hpp)

.PHONY: all
all: mainMOSP_eval

mainMOSP_eval: mainMOSP.cpp MOSP.cpp MOSP.h results.h MCNH/HNCM.h MCNH/LinkedList.h $(LIBSRC) $(LIBHEAD) $(wildcard ../PTAPI-main/include/*.h)
	$(CXX) $(CXXFLAGS) -I$(LIBDIR)/include mainMOSP.cpp MOSP.cpp $(LIBSRC) $(LDFLAGS) -pthread -o $@
