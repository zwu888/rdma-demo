CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -std=c++17
LDLIBS   := -lfabric

DPDK_CFLAGS := $(shell pkg-config --cflags libdpdk)
DPDK_LIBS   := $(shell pkg-config --libs libdpdk)

all: fi_bw dpdk_perf ibv_bw

fi_bw: fi_bw.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

# Debug build: no optimization (accurate line-by-line stepping in gdb),
# AddressSanitizer + UndefinedBehaviorSanitizer catch buffer/lifetime
# bugs around the registered-memory slots and fi_context bookkeeping.
fi_bw-debug: fi_bw.cpp
	$(CXX) -O0 -g -fsanitize=address,undefined -Wall -std=c++17 -o $@ $< $(LDLIBS)

dpdk_perf: dpdk_perf.cpp
	$(CXX) $(CXXFLAGS) $(DPDK_CFLAGS) -o $@ $< $(DPDK_LIBS)

ibv_bw: ibv_bw.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -libverbs

clean:
	rm -f fi_bw fi_bw-debug dpdk_perf ibv_bw

.PHONY: clean all
