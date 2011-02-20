.PHONY: all clean

CXXFLAGS = \
	-fpic -DPIC -fvisibility=hidden -fvisibility-inlines-hidden \
	-fno-rtti -fno-exceptions -fomit-frame-pointer \
	-Wall -g
FASTCXXFLAGS = $(CXXFLAGS) -O2 -march=native
DEBUGCXXFLAGS = $(CXXFLAGS) \
	-DDEBUG \
	-ftrapv -funwind-tables
LDFLAGS = -lrt -ldl
LDSOFLAGS = $(LDFLAGS) -Wl,-no-undefined
GHCFLAGS = -O2 -fvia-c

all: malloc.so malloc_debug.so test debugtest

HUSH_CXX = @echo " [CXX]\t$@";
HUSH_CXX_DEBUG = @echo " [CXX]\t$@ [DEBUG]";

malloc.so: malloc.cpp
	$(HUSH_CXX) $(CXX) $(FASTCXXFLAGS) -shared -o $@ $< $(LDSOFLAGS)

malloc_debug.so: malloc.cpp
	$(HUSH_CXX_DEBUG) $(CXX) $(DEBUGCXXFLAGS) -shared -o $@ $< $(LDSOFLAGS)

test: malloc.cpp
	$(HUSH_CXX) $(CXX) $(FASTCXXFLAGS) -o $@ $< $(LDFLAGS) -DTEST

debugtest: malloc.cpp
	$(HUSH_CXX_DEBUG) $(CXX) $(DEBUGCXXFLAGS) -o $@ $< $(LDFLAGS) -DTEST

clean:
	rm -f malloc.so malloc_debug.so test debugtest
