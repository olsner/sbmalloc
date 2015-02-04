.PHONY: all clean

CXXFLAGS = \
	-fpic -DPIC -fvisibility=hidden -fvisibility-inlines-hidden \
	-fno-rtti -fno-exceptions -fomit-frame-pointer \
	-Wall -g
FASTCXXFLAGS = $(CXXFLAGS) -Os -march=native
DEBUGCXXFLAGS = $(CXXFLAGS) \
	-DDEBUG \
	-ftrapv -funwind-tables
LDFLAGS = -lrt -ldl -lpthread -Wl,-Bsymbolic
LDSOFLAGS = $(LDFLAGS) -Wl,-no-undefined -Wl,-Bsymbolic
GHCFLAGS = -O2 -fvia-c
DEPFLAGS = -MP -MT $@ $(addprefix -MT ,$(TARGETS))

DEPFILES = malloc.D

TARGETS = malloc.so malloc_debug.so test debugtest

ifeq ($(filter clean,$(MAKECMDGOALS)),clean)
all: | clean
	@$(MAKE) --no-print-directory $(TARGETS)
else
all: $(TARGETS)
endif

HUSH_DEP = @echo " [DEP]\t$<";
HUSH_CXX = @echo " [CXX]\t$@";
HUSH_CXX_DEBUG = @echo " [CXX]\t$@ [DEBUG]";
HUSH_RM = @x_rm() { echo " [RM]\t$$@"; rm -f "$$@"; };
RM = x_rm

%.D: %.cpp
	$(HUSH_DEP) $(CXX) $(CXXFLAGS) $(DEPFLAGS) -MM -o $@ $<

malloc.so: malloc.cpp
	$(HUSH_CXX) $(CXX) $(FASTCXXFLAGS) -shared -o $@ $< $(LDSOFLAGS)

malloc_debug.so: malloc.cpp
	$(HUSH_CXX_DEBUG) $(CXX) $(DEBUGCXXFLAGS) -shared -o $@ $< $(LDSOFLAGS)

test: malloc.cpp
	$(HUSH_CXX) $(CXX) $(FASTCXXFLAGS) -o $@ $< $(LDFLAGS) -DTEST

debugtest: malloc.cpp
	$(HUSH_CXX_DEBUG) $(CXX) $(DEBUGCXXFLAGS) -o $@ $< $(LDFLAGS) -DTEST

clean:
	$(HUSH_RM) $(RM) $(TARGETS) $(DEPFILES)

-include $(DEPFILES)
