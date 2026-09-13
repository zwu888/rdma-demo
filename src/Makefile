CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -std=c++17
LDLIBS   := -lfabric

fi_bw: fi_bw.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

# Debug build: no optimization (accurate line-by-line stepping in gdb),
# AddressSanitizer + UndefinedBehaviorSanitizer catch buffer/lifetime
# bugs around the registered-memory slots and fi_context bookkeeping.
fi_bw-debug: fi_bw.cpp
	$(CXX) -O0 -g -fsanitize=address,undefined -Wall -std=c++17 -o $@ $< $(LDLIBS)

clean:
	rm -f fi_bw fi_bw-debug

.PHONY: clean
