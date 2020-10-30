#include "malloc.cpp"

#include <random>
#include <thread>
#include <vector>

static size_t total_alloced = 0;

static void* test_malloc(size_t s)
{
	total_alloced += s;
	return malloc(s);
}

static void* test_realloc(void *p, size_t s)
{
	total_alloced += s;
	return realloc(p, s);
}

#define malloc test_malloc
#define realloc test_realloc
#define calloc test_calloc

static void fill_pattern(void* buf_, size_t size)
{
	u8* p = (u8*)buf_;
	while (size--)
	{
		p[size] = size & 0xff;
	}
}

static void test_pattern(void* buf_, size_t size)
{
	u8* p = (u8*)buf_;
	while (size--)
	{
		xassert(p[size] == (size & 0xff));
	}
}

static void selftest_realloc()
{
	void* buffer = NULL;
	void* temp = NULL;
	size_t size = 12, prev_size;

	buffer = realloc(NULL, size);
	fill_pattern(buffer, size);

	prev_size = size;
	size += 10;
	buffer = realloc(buffer, size);
	test_pattern(buffer, prev_size);
	fill_pattern(buffer, size);

	buffer = realloc(buffer, prev_size);
	test_pattern(buffer, prev_size);

	free(buffer);
	buffer = malloc(32);
	temp = realloc(buffer, 17);
	xassert(temp == buffer);

	size = 4097;
	prev_size = 17;
	fill_pattern(buffer, 17);
	buffer = realloc(buffer, size);
	test_pattern(buffer, 17);
	fill_pattern(buffer, size);

	buffer = realloc(buffer, 2 * size);
	test_pattern(buffer, size);
	fill_pattern(buffer, 2 * size);

	buffer = realloc(buffer, size);
	test_pattern(buffer, size);

	free(buffer);
}

static void selftest_limits()
{
	for (size_t n = 1; n < 2 * PAGE_SIZE; n++) {
		xassert(malloc(-n) == NULL);
	}
}

static void selftest(uint32_t seed)
{
	std::minstd_rand xrand { seed };

	const size_t DELAY = 1024;
	const size_t NTESTS = 1000000;
	const size_t MAXALLOC = 4096;

	size_t iters = 1;

	void* ptrs[DELAY] = {0};
	for (size_t i = 0; i < DELAY; i++)
	{
		ptrs[i] = malloc(xrand() % MAXALLOC);
	}
	while (iters--)
	{
		for (size_t i = 0; i < NTESTS; i++)
		{
			size_t size = xrand() % MAXALLOC;
			size_t ifree = (xrand() % DELAY);
			size_t imalloc = (xrand() % DELAY);
			free(ptrs[ifree]);
			ptrs[ifree] = ptrs[imalloc];
			ptrs[imalloc] = malloc(size);
		}
	}
	for (size_t i = 0; i < DELAY; i++)
	{
		free(ptrs[i]);
	}
	
	selftest_realloc();
	selftest_limits();
}

static int getintarg(int index, int argc, const char *argv[])
{
	int c = 0;
	if (argc > index) c = atoi(argv[index]);
	return c > 0 ? c : 1;
}

namespace __gnu_cxx {
	void __freeres();
}

int main(int argc, const char *argv[])
{
	const int c = getintarg(1, argc, argv);
	const int nthreads = getintarg(2, argc, argv);
	printf("Running test for %d iterations in %d threads...\n", c, nthreads);
	// Scope to clean up everything before getting to dump_pages
	{
		std::vector<std::thread> threads;
		threads.reserve(nthreads);
		for (int tid = 0; tid < nthreads; tid++)
		{
			const int seed_base = tid * c;
			threads.emplace_back([c, seed_base]() {
				for (int n = c; n--;) selftest(seed_base + n);
			});
		}
		for (auto& thread: threads)
		{
			thread.join();
		}
	}
	__gnu_cxx::__freeres();
	printf("Allocated %zu bytes (%zu per iteration)\n", total_alloced, total_alloced / c);
	printf("\"OK, dumping left-over state:\"!\n");
	dump_pages();
	printf("\"OK\"!\n");
}
