#include <stdio.h>
#include <stdarg.h>
#ifndef assert
#include <assert.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>

#ifdef STATIC_XPRINTF
#define xprintf(...) xfprintf(stdout, __VA_ARGS__)
#define XPRINTF_LINKAGE static
#endif

#ifndef XPRINTF_LINKAGE
#define XPRINTF_LINKAGE
#endif

#ifndef xprintf
XPRINTF_LINKAGE void xprintf(const char* fmt, ...)
		__attribute__((format(printf, 1, 2)));
#endif
XPRINTF_LINKAGE void xfprintf(FILE* fp, const char* fmt, ...)
		__attribute__((format(printf, 2, 3)));
XPRINTF_LINKAGE void xvfprintf(FILE* file, const char* fmt, va_list ap);

#ifdef MALLOC_SAFE_PRINTF
static void xputc(char c, FILE* fp)
{
	const int fd = fileno_unlocked(fp);

	// fd will be < 0 if the file object is e.g. a memory stream instead of an
	// actual file.
	if (fd < 0)
	{
		fputc_unlocked(c, fp);
	}
	else
	{
		// Apparently even fputc may allocate memory for the output buffer, so
		// we have to work harder...
		write(fd, &c, 1);
	}
}
static void xwrite(const char* data, size_t n, FILE* fp)
{
	const int fd = fileno_unlocked(fp);

	if (fd < 0)
	{
		fwrite_unlocked(data, 1, n, fp);
	}
	else
	{
		// TODO Check result and repeat write if not all data was written
		write(fd, data, n);
	}
}
#else
static void xputc(int c, FILE* fp)
{
	fputc_unlocked(c, fp);
}
static void xwrite(const char* data, size_t n, FILE* fp)
{
	fwrite_unlocked(data, 1, n, fp);
}
#endif

static void format_num(FILE* file, int width, bool leading_zero, bool sign, int base, bool show_base, uintptr_t num)
{
	if (sign && (intptr_t)num < 0)
	{
		num = -num;
		xputc('-', file);
	}
	if (show_base)
	{
		assert(base == 16);
		xputc('0', file);
		xputc('x', file);
	}
	char buf[32];
	memset(buf, 0, sizeof(buf));
	size_t len = 0;
	do
	{
		buf[len++] = "0123456789abcdef"[num % base];
		num /= base;
	}
	while (num);
	if (width)
	{
		int c = leading_zero ? '0' : ' ';
		while (len < (size_t)width--)
		{
			xputc(c, file);
		}
	}
	while (len--)
	{
		xputc(buf[len], file);
	}
}

static const char* read_width(const char* fmt, int* width)
{
	errno = 0;
	char* endptr = NULL;
	*width = strtol(fmt, &endptr, 10);
	assert(!errno);
	return endptr;
}

void xvfprintf(FILE* file, const char* fmt, va_list ap)
{
	flockfile(file);
#ifdef MALLOC_SAFE_PRINTF
	// Since we'll write directly to the file descriptor, flush any other
	// output first.
	fflush_unlocked(file);
#endif
	while (*fmt)
	{
		const char* nextformat = strchr(fmt, '%');
		if (!nextformat)
		{
			xwrite(fmt, strlen(fmt), file);
			break;
		}
		else
		{
			xwrite(fmt, nextformat - fmt, file);
			fmt = nextformat + 1;
		}
		bool is_long = false;
		bool is_size = false;
		bool leading_zero = false;
		bool sign = true;
		bool show_base = false;
		int width = 0;
		//int before_point = 0;
		int base = 10;
		for (;;)
		{
#define ARG(t) va_arg(ap, t)
			switch (*fmt++)
			{
			case '%':
				xputc('%', file);
				break;
			case 's':
			{
				const char* arg = ARG(const char*);
				if (arg)
					xwrite(arg, strlen(arg), file);
				else
					xwrite("(null)", sizeof("(null)")-1, file);
				break;
			}
			// 'o' is also unsigned, somewhat surprisingly
			case 'o':
				base = 8;
				if (false)
			case 'x':
				base = 16;
			case 'u':
				sign = false;
			case 'd':
#if 0
			case 'i':
#endif
				format_num(file, width, leading_zero, sign, base, show_base,
					is_long ?
						(sign ? ARG(long) : ARG(unsigned long))
					: is_size ?
						(sign ? ARG(size_t) : ARG(ssize_t))
					:
						// Careful here: the int must be sign-extended to the
						// same width type that format_num takes (at least).
						// x?int:unsigned :: unsigned, which may be narrower
						// than that, causing only partial sign extension.
						(sign ? (intptr_t)ARG(int) : ARG(unsigned)));
				break;
			case 'p':
				format_num(file, 0, false, false, 16, true, (uintptr_t)va_arg(ap, void*));
				break;
			case 'l':
				is_long = true;
				continue;
			case 'z':
				is_size = true;
				continue;
			case '#':
				show_base = true;
				continue;
			case '.':
				//before_point = width;
				width = 0;
				continue;
			case '0':
				leading_zero = true;
				fmt = read_width(fmt, &width);
				continue;
			default:
				if (isdigit(fmt[-1]))
				{
					fmt = read_width(fmt - 1, &width);
					continue;
				}
				return; /* -1 */
			}
			break;
		}
	}
	funlockfile(file);
	fflush(file);
	fflush(stderr); // HACK
	/* Should return the number of characters output, or -1 on error. */
}

void xfprintf(FILE* fp, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	xvfprintf(fp, fmt, ap);
	va_end(ap);
}

#ifndef xprintf
XPRINTF_LINKAGE void xprintf(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	xvfprintf(stdout, fmt, ap);
	va_end(ap);
}
#endif

#ifdef XPRINTF_TEST
#define test(result, fmt, ...) \
	do { \
		FILE* fp = open_memstream(&memstream_buffer, &memstream_size); \
		xfprintf(fp, fmt, ## __VA_ARGS__); \
		fflush(fp); \
		fclose(fp); \
		char tmp[256] = {0}; \
		snprintf(tmp, sizeof(tmp), fmt, ## __VA_ARGS__); \
		if (strcmp(memstream_buffer, result) != 0 \
			|| strcmp(tmp, result) != 0) { \
			fprintf(stderr, "%s (" fmt "):\n\tactual   \"%s\"\n\texpected \"%s\"\n", \
				fmt, ## __VA_ARGS__, memstream_buffer, result); \
			fail++; \
		} else { \
			pass++; \
		} \
	} while (0)

int main()
{
	char* memstream_buffer = NULL;
	size_t memstream_size = 0;
	int fail = 0, pass = 0;
	test("-2147483648", "%d", INT_MIN);
	test("0x80000000", "%#x", unsigned(INT_MIN));
	test("80000000", "%x", unsigned(INT_MIN));
	test("20000000000", "%o", unsigned(INT_MIN));
	if (sizeof(long) == 8) {
		test("-9223372036854775808", "%ld", LONG_MIN);
		test("9223372036854775808", "%lu", 1 + (unsigned long)LONG_MAX);
	} else {
		test("-2147483648", "%ld", LONG_MIN);
		test("2147483648", "%lu", 1 + (unsigned long)LONG_MAX);
	}
	if (fail) {
		fprintf(stderr, "FAIL: %d test cases failed\n", fail);
	} else {
		fprintf(stdout, "OK: %d test cases passed\n", pass);
	}
	return fail;
}
#endif
