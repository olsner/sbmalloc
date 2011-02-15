.PHONY: all clean

CXXFLAGS = -fpic -DPIC -fvisibility=hidden -g -fno-rtti -fno-exceptions -Wall
LDFLAGS = -lrt -ldl
LDSOFLAGS = $(LDFLAGS) -Wl,-no-undefined
GHCFLAGS = -O2 -fvia-c

all: malloc.so test

malloc.so: malloc.cpp
	$(CXX) $(CXXFLAGS) -shared -o $@ $< $(LDSOFLAGS)

test: malloc.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) -DTEST

clean:
	rm -f malloc.so test
