# Standalone microbenchmark. Not part of the chipcae CMake build.
CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror

.PHONY: all clean
all: poisson_microbench

poisson_microbench: poisson_microbench.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f poisson_microbench
