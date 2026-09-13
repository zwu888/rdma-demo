CXX      ?= g++
CXXFLAGS ?= -O2 -Wall -std=c++17
LDLIBS   := -lfabric

fi_bw: fi_bw.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f fi_bw

.PHONY: clean
