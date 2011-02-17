.PHONY: all clean

CXXFLAGS = \
	-fpic -DPIC -fvisibility=hidden -fvisibility-inlines-hidden \
	-fno-rtti -fno-exceptions -fomit-frame-pointer \
	-Wall
FASTCXXFLAGS = $(CXXFLAGS) -O2 -march=native
DEBUGCXXFLAGS = $(CXXFLAGS) \
	-g -DDEBUG \
	-ftrapv -funwind-tables
LDFLAGS = -lrt -ldl
LDSOFLAGS = $(LDFLAGS) -Wl,-no-undefined
GHCFLAGS = -O2 -fvia-c

all: malloc.so malloc_debug.so test debugtest

malloc.so: malloc.cpp
	$(CXX) $(FASTCXXFLAGS) -shared -o $@ $< $(LDSOFLAGS)

malloc_debug.so: malloc.cpp
	$(CXX) $(DEBUGCXXFLAGS) -shared -o $@ $< $(LDSOFLAGS)

test: malloc.cpp
	$(CXX) $(FASTCXXFLAGS) -o $@ $< $(LDFLAGS) -DTEST

debugtest: malloc.cpp
	$(CXX) $(DEBUGCXXFLAGS) -o $@ $< $(LDFLAGS) -DTEST

clean:
	rm -f malloc.so malloc_debug.so test debugtest
