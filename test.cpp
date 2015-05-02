#include "malloc.cpp"

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

static int32_t xrand()
{
	static int32_t m_w = 1246987127, m_z = 789456123;
	m_z = 36969 * (m_z & 65535) + (m_z >> 16);
	m_w = 18000 * (m_w & 65535) + (m_w >> 16);
	return (m_z << 16) + m_w;  /* 32-bit result */
}

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

static void selftest()
{
	const size_t DELAY = 1000;
	const size_t NTESTS = 1000000;
	const size_t MAXALLOC = 4097;

	size_t iters = 1;
	if (const char *iterenv = getenv("TEST_ITERATIONS"))
		iters = atoi(iterenv);

	for (size_t i = 1; i < PAGE_SIZE / 2; i++)
	{
		size_t ix = size_ix(i);
		xassert(ix < N_SIZES);
		xassert(i <= ix_size(ix));
	}
	for (size_t i = 4; i < PAGE_SHIFT-1; i++)
	{
		size_t ix = size_ix(1 << i);
		xassert(ix < N_SIZES);
		xassert((1u << i) == ix_size(ix));
	}

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
}

static int getcount(int argc, const char *argv[])
{
	int c = 0;
	if (argc > 1) c = atoi(argv[1]);
	return c > 0 ? c : 1;
}

int main(int argc, const char *argv[])
{
	const int c = getcount(argc, argv);
	printf("Running test for %d iterations\n", c);
	for (int n = c; n--;) selftest();
	printf("Allocated %zu bytes (%zu per iteration)\n", total_alloced, total_alloced / c);
	printf("\"OK, dumping left-over state:\"!\n");
	dump_pages();
	printf("\"OK\"!\n");
}
