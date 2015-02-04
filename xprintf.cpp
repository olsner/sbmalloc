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

#ifdef STATIC_XPRINTF
#define xprintf(...) xfprintf(stdout, __VA_ARGS__)
#define XPRINTF_LINKAGE static
#endif

#ifndef XPRINTF_LINKAGE
#define XPRINTF_LINKAGE
#endif

static void format_num(FILE* file, int width, bool leading_zero, bool sign, int base, bool show_base, uintptr_t num)
{
	if (sign && (intptr_t)num < 0)
	{
		num = -num;
		fputc_unlocked('-', file);
	}
	if (show_base)
	{
		assert(base == 16);
		fputc_unlocked('0', file);
		fputc_unlocked('x', file);
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
			fputc_unlocked(c, file);
		}
	}
	while (len--)
	{
		fputc_unlocked(buf[len], file);
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

XPRINTF_LINKAGE void xvfprintf(FILE* file, const char* fmt, va_list ap)
{
	flockfile(file);
	while (*fmt)
	{
		const char* nextformat = strchr(fmt, '%');
		if (!nextformat)
		{
			fwrite_unlocked(fmt, 1, strlen(fmt), file);
			break;
		}
		else
		{
			fwrite_unlocked(fmt, 1, nextformat - fmt, file);
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
			switch (*fmt++)
			{
			case '%':
				fputc_unlocked('%', file);
				break;
			case 's':
			{
				const char* arg = va_arg(ap, const char*);
				if (arg)
					fwrite_unlocked(arg, 1, strlen(arg), file);
				else
					fwrite_unlocked("(null)", 1, sizeof("(null)")-1, file);
				break;
			}
			case 'x':
				base = 16;
			case 'u':
				sign = false;
			case 'd':
#define format_num_type(atype, ntype) format_num(file, width, leading_zero, sign, base, show_base, (ntype)va_arg(ap, atype))
				if (is_long)
					sign ? format_num_type(long, intptr_t) : format_num_type(unsigned long, uintptr_t);
				else if (is_size)
					format_num_type(size_t, uintptr_t);
				else
					format_num_type(int, intptr_t);
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
				abort();
			}
			break;
		}
	}
	funlockfile(file);
	fflush(file);
	fflush(stderr); // HACK
}

XPRINTF_LINKAGE void xfprintf(FILE* fp, const char* fmt, ...)
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
		if (strcmp(memstream_buffer, result) != 0) { \
			fprintf(stderr, "%s (" fmt "):\n\tactual   \"%s\"\n\texpected \"%s\"\n", \
				fmt, ## __VA_ARGS__, memstream_buffer, result); \
			res++; \
		} \
	} while (0)

int main()
{
	char* memstream_buffer = NULL;
	size_t memstream_size = 0;
	int res = 0;
	test("-2147483648", "%d", INT_MIN);
	test("-9223372036854775808", "%ld", LONG_MIN);
	if (res) {
		fprintf(stderr, "%d failed test cases\n", res);
	}
	return res;
}
#endif
