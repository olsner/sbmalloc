#include <stdint.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE (1 << PAGE_SHIFT)

static void panic1();
static void panic2() __attribute__((noreturn));
#define assert_failed(e, file, line) panic("Assertion failed! %s:%d: %s", file, line, e)
#define xassert(e) if (likely(e)); else assert_failed(#e, __FILE__, __LINE__)
#define xassert_abort(e) if (likely(e)); else abort()
#define panic(fmt, ...) do { \
	panic1(); \
	xfprintf(stderr, "PANIC: " fmt "\n", ## __VA_ARGS__); \
	panic2(); } while (0);

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#if defined DEBUG
#define assert xassert
#else
#define assert(...) (void)0
#endif

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define STATIC_XPRINTF
#include "xprintf.cpp"

#ifdef DEBUG
#define debug xprintf
#define IFDEBUG(X) X
#else
#define debug(...) (void)0
#define IFDEBUG(X) /* nothing */
#endif
#define printf xprintf

void panic1()
{
	static bool panicked = false;
	if (panicked)
	{
		xprintf("Recursive panic. Aborting.\n");
		abort();
	}
	panicked++;
}

void panic2()
{
	dump_pages();
	fflush(stdout);
	fflush(stderr);
	abort();
}

