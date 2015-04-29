ifeq ($(filter -j%, $(MAKEFLAGS)),)
NCPU := $(shell grep -c ^processor /proc/cpuinfo)
J ?= $(NCPU)
MAKEFLAGS += -j$J
endif

.PHONY: all clean

CXXFLAGS = \
	-fpic -DPIC -fvisibility=hidden -fvisibility-inlines-hidden \
	-fno-rtti -fno-exceptions -fomit-frame-pointer \
	-Wall -g
FASTCXXFLAGS = $(CXXFLAGS) \
	-Os -march=native \
	-fno-unwind-tables -fno-asynchronous-unwind-tables
DEBUGCXXFLAGS = $(CXXFLAGS) \
	-DDEBUG \
	-ftrapv -funwind-tables
LDFLAGS = -lrt -ldl -lpthread -Wl,-Bsymbolic -Wl,--gc-sections
LDSOFLAGS = $(LDFLAGS) -Wl,-no-undefined -Wl,-Bsymbolic
GHCFLAGS = -O2 -fvia-c
DEPFLAGS = -MP -MT $@ $(addprefix -MT ,$(TARGETS))
STRIPFLAGS = --strip-unneeded

DEPFILES = malloc.D

TARGETS = malloc.so malloc_debug.so malloc.stripped.so test debugtest printf_test

ifeq ($(filter clean,$(MAKECMDGOALS)),clean)
all: MAKEFLAGS := $(filter-out -j%, $(MAKEFLAGS))
all: | clean
	@$(MAKE) --no-print-directory $(TARGETS)
else
all: $(TARGETS)
endif

HUSH_STRIP = @echo " [STRIP]\t$@";
HUSH_DEP = @echo " [DEP]\t$<";
HUSH_CXX = @echo " [CXX]\t$@";
HUSH_CXX_DEBUG = @echo " [CXX]\t$@ [DEBUG]";
HUSH_RM = @x_rm() { echo " [RM]\t$$@"; rm -f "$$@"; };
RM = x_rm
STRIP ?= strip

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

printf_test: xprintf.cpp
	$(HUSH_CXX_DEBUG) $(CXX) $(DEBUGCXXFLAGS) -o $@ $< $(LDFLAGS) -DXPRINTF_TEST
	@./printf_test

%.stripped.so: %.so
	$(HUSH_STRIP) $(STRIP) $(STRIPFLAGS) -o $@ $<
	@echo " [STRIP]\t$@: `stat -c%s $@` bytes"

clean:
	$(HUSH_RM) $(RM) $(TARGETS) $(DEPFILES)

-include $(DEPFILES)

FUZZ_SRC = Fuzzer/FuzzerCrossOver.cpp Fuzzer/FuzzerDFSan.cpp Fuzzer/FuzzerDriver.cpp Fuzzer/FuzzerIO.cpp Fuzzer/FuzzerLoop.cpp Fuzzer/FuzzerMain.cpp Fuzzer/FuzzerMutate.cpp Fuzzer/FuzzerSanitizerOptions.cpp Fuzzer/FuzzerUtil.cpp
FUZZ_OBJS = $(FUZZ_SRC:.cpp=.o)
ASAN_FLAGS = -fsanitize=address -fsanitize-coverage=3
CLANG ?= clang-3.6

Fuzzer/%.o: Fuzzer/%.cpp
	$(CLANG) -c -g -O2 -std=c++11 -o $@ $^

libfuzzer.a: $(FUZZ_OBJS)
	$(AR) cr $@ $^

malloc_asan.o: malloc.cpp
	$(CLANG) -c -g -O2 -std=c++11 $(ASAN_FLAGS) -o $@ $^

fuzzer: fuzzer.cc libfuzzer.a malloc_asan.o
	$(CLANG) -g -O2 -std=c++11 $(ASAN_FLAGS) -o $@ $^

all: fuzzer
