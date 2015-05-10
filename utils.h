#include <stdint.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE (1 << PAGE_SHIFT)

// TODO Mark printf-like for format warnings
static void panic(const char *fmt, ...) __attribute__((noreturn));
static void assert_failed(const char *e, const char *file, int line) __attribute__((noreturn));
static void assert_failed(const char *e, const char *file, int line) {
	panic("Assertion failed! %s:%d: %s", file, line, e);
}
#define xassert(e) if (likely(e)); else assert_failed(#e, __FILE__, __LINE__)
#define xassert_abort(e) if (likely(e)); else abort()

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

static void panic(const char *fmt, ...)
{
	static bool panicked = false;
	if (panicked)
	{
		xprintf("Recursive panic. Aborting.\n");
		abort();
	}
	panicked++;

	va_list ap;
	va_start(ap, fmt);
	xvfprintf(stderr, fmt, ap);
	va_end(ap);

	dump_pages();

	va_list aq;
	va_start(aq, fmt);
	xvfprintf(stderr, fmt, aq);
	va_end(aq);

	fflush(stdout);
	fflush(stderr);
	abort();
}
#define panic(fmt, ...) panic(fmt "\n", ## __VA_ARGS__)

#define MUL_NO_OVERFLOW (size_t(1) << (sizeof(size_t) * 4))

static inline bool mul_safe(size_t x, size_t y)
{
	if ((x >= MUL_NO_OVERFLOW || y >= MUL_NO_OVERFLOW) &&
		y > 0 && SIZE_MAX / y < x)
	{
		return false;
	}
	return true;
}
