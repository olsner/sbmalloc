#define _POSIX_C_SOURCE 200112L

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <pthread.h>

static void xprintf(const char* fmt, ...);
static void panic(const char* fmt, ...) __attribute__((noreturn));
static void dump_pages();
static void dump_pages_safe();
#define xassert(e) if (likely(e)); else panic("Assertion failed! " #e)
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

#ifdef DEBUG
#define debug xprintf
#define IFDEBUG(X) /*X*/
#else
#define debug(...) (void)0
#define IFDEBUG(X) /* nothing */
#endif
#define printf xprintf

static void free_unlocked(void* ptr);
static void* malloc_unlocked(size_t size);
static void* realloc_unlocked(void* ptr, size_t new_size);

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#ifdef DEBUG
#define MALLOC_CLEAR_MEM 0xcc
#define MALLOC_CLEAR_MEM_AFTER 0xfd
#define FREE_CLEAR_MEM 0xdd
#endif

#ifndef TEST
#define THREAD_SAFE
#define USE_SPINLOCKS
#endif

static void init_lock();
struct mallock
{
#if !defined(THREAD_SAFE)
	void lock() {}
	void unlock() {}
#elif defined(USE_SPINLOCKS)
	pthread_spinlock_t spinlock;

	void init()
	{
		int res = pthread_spin_init(&spinlock, PTHREAD_PROCESS_PRIVATE);
		xassert(res == 0);
	}
	void lock()
	{
		init_lock();
		int res = pthread_spin_lock(&spinlock);
		xassert(res == 0);
	}
	void unlock()
	{
		init_lock();
		int res = pthread_spin_unlock(&spinlock);
		xassert(res == 0);
	}
#else
#error Choose your poison
#endif
};
static mallock g_lock;
#ifdef THREAD_SAFE
pthread_once_t mallock_init = PTHREAD_ONCE_INIT;
static void init_lock_cb()
{
	g_lock.init();
}
void init_lock()
{
	pthread_once(&mallock_init, init_lock_cb);
}
#else
void init_lock()
{}
#endif

class scopelock
{
	scopelock(const scopelock&);
	scopelock& operator=(const scopelock&);
public:
	scopelock()
	{
		g_lock.lock();
	}
	~scopelock()
	{
		g_lock.unlock();
	}
};

#ifdef THREAD_SAFE
#define SCOPELOCK(e) scopelock scopelock_ ## e
#elif defined(TEST)
#define SCOPELOCK(e) (void)0
#else
static pthread_t get_owner();
#define SCOPELOCK(e) xassert_abort(pthread_equal(get_owner(), pthread_self()))
#endif

/**
 * These are adapted to be stored internally in whatever data we have.
 */
struct pairing_ptr_node
{
	/**
	 * down,right: first child and right sibling of this page in pairing heap
	 * of pages with free space.
	 */
	pairing_ptr_node* down;
	pairing_ptr_node* right;
};
struct pairing_ptr_heap
{
	typedef pairing_ptr_node T;
	typedef pairing_ptr_node* Tp;

	Tp min;

	void delete_min()
	{
		assert(!min->right);
		Tp l = min->down;
		min->down = NULL;
		min = mergePairs(l);
	}

	void insert(Tp r)
	{
		min = merge(min, r);
	}

	operator bool() const
	{
		return (bool)min;
	}

	static Tp mergePairs(Tp l)
	{
		if (!l || !l->right) return l;

		Tp r = l->right;
		Tp hs = r->right;
		l->right = r->right = NULL;
		assert(hs != l && hs != r && r != l);
		// FIXME recursion...
		// We can use l->right after merge returns, since merge() always
		// returns something with a right-value of NULL. l will never be
		// touched until we're about to merge it with the result of mergePairs
		l = merge(l,r);
		hs = mergePairs(hs);
		return merge(l,hs);
	}

	static Tp merge(Tp l, Tp r)
	{
		if (!l)
		{
			assert(!r->right);
			return r;
		}
		if (!r)
		{
			assert(!l->right);
			return l;
		}

		assert(!l->right && !r->right);

		if (r < l)
		{
			Tp tmp = r;
			r = l;
			l = tmp;
		}
		// l <= r!

		r->right = l->down;
		l->down = r;
		//l->right = NULL; // we know it's already null
		return l;
	}
};
static pairing_ptr_node* get_min(const pairing_ptr_heap& p)
{
	return p.min;
}
static void delete_min(pairing_ptr_heap& p)
{
	assert(p.min);
	p.delete_min();
}
static void insert(pairing_ptr_heap& heap, pairing_ptr_node* r)
{
	heap.insert(r);
}

struct splay_node
{
	typedef splay_node S;
	typedef S* Sp;

	Sp left;
	Sp right;

};
struct splay_tree
{
	typedef splay_node S;
	typedef S* Sp;

	Sp root;
	Sp min;

	splay_tree():
		root(NULL),
		min(NULL)
	{}

	operator bool()
	{
		return !!root;
	}

	void remove(Sp node)
	{
		Sp small, big;
		partition(node, root, &small, &big);
		// If new_root != node, then the node wasn't in the tree and we don't
		// have do to anything.
		if (small == node)
		{
			small = small->left;
		}
		root = merge(small, big);
		if (node == min)
		{
			min = find_min(root);
		}
	}

	void delete_min()
	{
		assert(root);
		delete_min(root, &root);
		min = find_min(root);
	}

	static void delete_min(Sp node, Sp* ret)
	{
		if (!node->left)
			*ret = node->right;
		else if (!node->left->left)
		{
			node->left = node->left->right;
			*ret = node;
		}
		else
		{
			Sp x = node->left, a = x->left, b = x->right;
			Sp y = node; //Sp c = y->right;
			x->right = y;
			y->left = b;
			// c == y->right
			//y->right = c;
			*ret = x;
			delete_min(a, &x->left);
		}
	}

	static Sp merge(Sp small, Sp big)
	{
		if (!small)
			return big;
		Sp p = small;
		while (p->right) p = p->right;
		p->right = big;
		return small;
	}

	void insert(Sp node)
	{
		Sp small, big;
		partition(node, root, &small, &big);
		// node already in tree, just stitch it back together
		if (small == node)
		{
			assert(!small->right);
			node->right = big;
		}
		else
		{
			node->left = small;
			node->right = big;
		}
		root = node;
		if (node < min || !min)
			min = node;
	}

	static Sp find_min(Sp node)
	{
		if (node)
			while (node->left)
				node = node->left;
		return node;
	}

	Sp get_min() const
	{
		return min;
	}

	// Note: No splay here(?), assuming we'll not do this very often.
	Sp get_max()
	{
		assert(root);
		Sp cur = root;
		while (cur->right) cur = cur->right;
		return cur;
	}

	// NB! Destructively updates 'node' and puts it in either the smaller or
	// bigger tree.
	// TODO Rewrite into something with two return values
	static void partition(Sp pivot, Sp node, Sp* smaller, Sp* bigger)
	{
		if (!node)
		{
			*smaller = *bigger = NULL;
			return;
		}

		Sp a = node->left, b = node->right;
		if (node <= pivot)
		{
			if (b)
			{
				Sp b1 = b->left, b2 = b->right;
				if (b <= pivot)
				{
					//a == node->left already
					//node->left = a;
					node->right = b1;
					b->left = node;
					*smaller = b;
					partition(pivot, b2, &b->right, bigger);
				}
				else
				{
					//a == x->left already
					//x->left = a;
					*smaller = node;
					// likewise, b2 == y->right
					//y->right = b2;
					*bigger = b;
					partition(pivot, b1, &node->right, &b->left);
				}
			}
			else
			{
				*smaller = node;
				*bigger = NULL;
			}
		}
		else
		{
			if (a)
			{
				Sp a1 = a->left, a2 = a->right;
				if (a <= pivot)
				{
					*smaller = a;
					// a1 == a->left
					//a->left = a1;
					*bigger = node;
					// b == node->right
					//node->right = b;

					partition(pivot, a2, &a->right, &node->left);
				}
				else
				{
					*bigger = a;
					a->right = node;
					node->left = a2;
					// b == node->right
					//node->right = b;

					partition(pivot, a1, smaller, &a->left);
				}
			}
			else
			{
				*smaller = NULL;
				*bigger = node;
			}
		}
	}
};
static splay_node* get_min(const splay_tree& p)
{
	return p.get_min();
}
static void delete_min(splay_tree& p)
{
	p.delete_min();
}
static void insert(splay_tree& t, splay_node* node)
{
	t.insert(node);
}

struct pageinfo
{
	/**
	 * Pointer to where the page starts.
	 * TODO: Pack some stuff in here. *Twelve* vacant bits!
	 */
	void* page;

	/**
	 * The heap of address-sorted pages in this category that have free pages
	 */
	pairing_ptr_node heap;

	u16 size;
	//u16 isize;
	u16 chunks;
	u16 chunks_free;
	u8 index; // index into array of pages

	/**
	 * 1-32 bytes of bitmap data. A set bit means *free* chunk.
	 */
	u8 bitmap[];

	// TODO merge bitmap into page?
};
#define pageinfo_from_heap(heap_) \
	((pageinfo*)((char*)(heap_) - offsetof(pageinfo, heap)))

static bool page_filled(pageinfo* page)
{
	return !page->chunks_free;
}
static int clear_first_set_bit(u8* bitmap, size_t maxbit)
{
	for (size_t i = 0; i < (maxbit + 7u) / 8u; i++)
	{
		const u8 found = bitmap[i];
		if (found)
		{
			u8 mask = 0x1;
			size_t n = i << 3;
			while (mask)
			{
				assert(mask == (1 << (n & 7)));
				assert(n >> 3 == i);
				if (mask & found)
				{
					bitmap[i] = found & ~mask;
					return n;
				}
				mask <<= 1;
				n++;
			}
		}
	}
	panic("No free chunks found?");
}
static void set_bit(u8* const bitmap, const size_t ix)
{
#ifdef __x86_64__
	__asm__("btsq %1, %0" : "+m" (*(u64*)bitmap) : "r" (ix));
#else
	const u8 mask = 1 << (ix & 7);
	const size_t byte = ix >> 3;

	assert(!(bitmap[byte] & mask));
	bitmap[byte] |= mask;
#endif
}
static void* page_get_chunk(pageinfo* page)
{
	assert(page->chunks_free);
	page->chunks_free--;
	size_t n = clear_first_set_bit(page->bitmap, page->chunks);
	return (u8*)page->page + (n * page->size);
}
static void page_free_chunk(pageinfo* page, void* ptr)
{
	size_t offset_in_page = (u8*)ptr - (u8*)page->page;
	size_t ix = offset_in_page;
	ix /= page->size; // FIXME store inverse or something instead
	debug("Freeing %p in %p (size %d)\n", ptr, page->page, page->size);
	set_bit(page->bitmap, ix);
	page->chunks_free++;
}

#define N_SIZES (128/16+4)
#define PAGE_SHIFT 12
#define PAGE_SIZE (1 << PAGE_SHIFT)

typedef splay_tree freepage_heap;
typedef splay_node freepage_node;

static freepage_heap g_free_pages;
static size_t g_n_free_pages;
static pairing_ptr_heap g_chunk_pages[N_SIZES];
static uintptr_t g_first_page;
static uintptr_t g_n_pages;
// Assumes mostly contiguous pages...
static pageinfo** g_pages;

static void set_pageinfo(void* page, pageinfo* info);
static pageinfo* get_pageinfo(void* ptr);

#define MAGIC_PAGE_OTHER ((pageinfo*)0)
#define MAGIC_PAGE_FIRST ((pageinfo*)1)
#define MAGIC_PAGE_FOLLO ((pageinfo*)2)
#define MAGIC_PAGE_PGINFO ((pageinfo*)3)
#define MAGIC_PAGE_FREE ((pageinfo*)4)
#define LAST_MAGIC_PAGE ((pageinfo*)5)
#define IS_MAGIC_PAGE(page) ((page) && ((page) < LAST_MAGIC_PAGE))

template <typename T>
void format_num(FILE* file, int width, bool leading_zero, bool sign, int base, bool show_base, T num)
{
	if (sign && num < 0)
	{
		// FIXME Doesn't work for the most negative value of T :/
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

const char* read_width(const char* fmt, int* width)
{
	errno = 0;
	char* endptr = NULL;
	*width = strtol(fmt, &endptr, 10);
	assert(!errno);
	return endptr;
}

static void xvfprintf(FILE* file, const char* fmt, va_list ap)
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
		for (;;)
		{
			bool is_long = false;
			bool is_size = false;
			bool leading_zero = false;
			bool sign = true;
			bool show_base = false;
			int width = 0;
			int before_point = 0;
			int base = 10;
			switch (*fmt++)
			{
			case '%':
				fputc_unlocked('%', file);
				break;
			case 'x':
				base = 16;
			case 'u':
				sign = false;
			case 'd':
				if (is_long)
					format_num(file, width, leading_zero, sign, base, show_base, va_arg(ap, long));
				else if (is_size)
					format_num(file, width, leading_zero, sign, base, show_base, va_arg(ap, size_t));
				else
					format_num(file, width, leading_zero, sign, base, show_base, va_arg(ap, int));
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
				before_point = width;
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

static void xprintf(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	xvfprintf(stdout, fmt, ap);
	va_end(ap);
}

static void panic(const char* fmt, ...)
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
	flockfile(stderr);
	xvfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	funlockfile(stderr);
	va_end(ap);
	dump_pages();
	fflush(stdout);
	fflush(stderr);
	abort();
}

static size_t size_ix(size_t size)
{
	if (likely(size <= 128))
	{
		return (size - 1) / 16;
	}
	else
	{
		size--;
		size_t ix = 8; // 256 bytes
		size >>= 8;
		while (size) { size >>= 1; ix++; }
		return ix;
	}
}

static size_t ix_size(size_t ix)
{
	return ix < 8 ? 16 * (ix + 1) : (1 << ix);
}

static void* get_page()
{
	void* ret = get_min(g_free_pages);
	if (ret)
	{
		debug("Unlinking %p from free-list\n", ret);
		delete_min(g_free_pages);
		g_n_free_pages--;
	}
	else
	{
		debug("Free-list empty, allocating fresh page\n", ret);
		uintptr_t cur = (uintptr_t)sbrk(0);
		sbrk((PAGE_SIZE - cur) & (PAGE_SIZE - 1));
		ret = sbrk(PAGE_SIZE);
	}
	debug("get_page: %p\n", ret);
	return ret;
}

static void add_to_freelist(void* page)
{
	debug("Adding %p to page free-list\n", page);
	memset(page, 0, sizeof(freepage_node));
	insert(g_free_pages, (freepage_node*)page);
	g_n_free_pages++;
	set_pageinfo(page, MAGIC_PAGE_FREE);
}

static void free_page(void* page)
{
	void* cur_break = sbrk(0);
	if (g_n_free_pages && page == (u8*)cur_break - PAGE_SIZE)
	{
		debug("Freed last page (%p info %p), shrinking heap\n", page, get_pageinfo(page));
		uintptr_t offset = ((uintptr_t)page - g_first_page) >> PAGE_SHIFT;
		size_t free_pages = 0;
		g_pages[offset] = NULL;
		do
		{
			free_pages++;
			offset--;
		}
		while (offset && g_pages[offset] == NULL);
		debug("Freeing %ld last pages (%p..%p)\n", free_pages, (u8*)cur_break - free_pages * PAGE_SIZE, cur_break);
		sbrk(-free_pages * PAGE_SIZE);
		debug("Break now %p (was %p)\n", sbrk(0), cur_break);
	}
	else
	{
		add_to_freelist(page);
	}
}

static void register_magic_pages(void* ptr_, size_t count)
{
	u8* ptr = (u8*)ptr_;
	set_pageinfo(ptr, MAGIC_PAGE_FIRST);
	while (--count) set_pageinfo(ptr += PAGE_SIZE, MAGIC_PAGE_FOLLO);
}

/**
 * Get *contiguous* pages.
 */
static void* get_pages(size_t n)
{
	void* ret;
	if (n == 1)
	{
		ret = get_page();
	}
	else
	{
		// Align. Might not be required? Depends on who calls sbrk first...
		uintptr_t cur = (uintptr_t)sbrk(0);
		sbrk((PAGE_SIZE - cur) & (PAGE_SIZE - 1));
		ret = sbrk(PAGE_SIZE * n);
		debug("get_pages: %ld pages: %p\n", n, ret);
	}
	if (unlikely(ret == (void*)-1))
	{
		return NULL;
	}
	register_magic_pages(ret, n);
	return ret;
}

static void print_pageinfo(pageinfo* page)
{
	printf("info %p: ", page);
	printf("%d x %db, %d free\n", page->chunks, page->size, page->chunks_free);
}

size_t page_allocated_space(pageinfo* page)
{
	return (page->chunks - page->chunks_free) * page->size;
}

void dump_pages_safe()
{
	SCOPELOCK();
	dump_pages();
}

static void dump_pages()
{
	printf("First, dump chunk-pages:\n");
	bool corrupt = false;
	for (size_t i = 0; i < N_SIZES; i++)
	{
		pageinfo* page = pageinfo_from_heap(get_min(g_chunk_pages[i]));
		if (page)
		{
			size_t size = ix_size(i);
			printf("%p: %ld, size %ld: ", page->page, i, size);
			print_pageinfo(page);
		}
	}
	printf("Page dump:\n");
	size_t used = 0;
	size_t allocated = 0;
	size_t freelist_pages = 0;
	size_t magic_pages = 0;
	size_t pginfo_pages = 0;
	size_t unknown_magic = 0;

	pageinfo** pagep = g_pages;
	pageinfo** end = pagep + g_n_pages;
	u8* addr = (u8*)g_first_page;
	while (pagep < end)
	{
		pageinfo* page = *pagep++;
		if (unlikely(IS_MAGIC_PAGE(page)))
		{
			if (page == MAGIC_PAGE_FIRST)
			{
				pageinfo** first = pagep;
				while (*pagep == MAGIC_PAGE_FOLLO)
					pagep++;
				size_t npages = 1 + pagep - first;
				magic_pages += npages;
				printf("%p: %ld page(s)\n", addr, npages);
			}
			else if (page == MAGIC_PAGE_FREE)
			{
				freelist_pages++;
				printf("%p: on page free-list\n", addr);
			}
			else if (page == MAGIC_PAGE_PGINFO)
			{
				pginfo_pages++;
				printf("%p: contains pageinfo\n", addr);
			}
			else
			{
				unknown_magic++;
				printf("%p: magic %ld\n", addr, (uintptr_t)page);
			}
		}
		else if (likely(page))
		{
			if (page->page != addr)
			{
				printf("!!! CLOBBERED !!! page is %p but info points to %p\n", addr, page->page);
				corrupt = true;
			}
			printf("%p: ", addr);
			print_pageinfo(page);

			allocated += PAGE_SIZE;
			used += page_allocated_space(page);
		}
		else
		{
			//printf("Not used (or pagedir info)\n");
		}
		addr += 4096;
	}
	assert(!corrupt);
	printf(":pmud egaP\n");
	size_t p = allocated ? 10000 * used / allocated : 0;
	printf("Used %lu of %lu (%d.%02d%%)\n", used, allocated, p / 100, p % 100);
	printf("Pages: %zd freelist (%zd) %zd large allocs %zd pageinfo %zd unknown\n", freelist_pages, g_n_free_pages, magic_pages, pginfo_pages, unknown_magic);
	assert(freelist_pages == g_n_free_pages);
	fflush(stdout);
}

static void set_pageinfo(void* page, pageinfo* info)
{
	uintptr_t offset;
	if (likely(g_pages))
	{
		offset = ((uintptr_t)page - g_first_page) >> PAGE_SHIFT;
	}
	else
	{
		g_first_page = (uintptr_t)page;
		offset = 0;
	}

	//debug("set_pageinfo: Page %p info %p\n", page, info);

	if (unlikely(offset >= g_n_pages))
	{
		size_t required = (sizeof(pageinfo*) * offset + PAGE_SIZE) & ~(PAGE_SIZE-1);
		pageinfo** new_pages = (pageinfo**)mmap(NULL, required, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
		assert(new_pages != MAP_FAILED);

		memcpy(new_pages, g_pages, g_n_pages * sizeof(pageinfo*));
		munmap(g_pages, g_n_pages * sizeof(pageinfo*));

		g_pages = new_pages;
		g_n_pages = required / sizeof(pageinfo*);
	}

	g_pages[offset] = info;
}

static pageinfo* new_chunkpage(size_t size)
{
	size_t ix = size_ix(size);
	assert(ix_size(ix) >= size);
	size = ix_size(ix);

	size_t nchunks = 4096/size;
	pageinfo* ret = NULL;
	size_t pisize = sizeof(pageinfo) + (nchunks+7)/8;
	if (!g_chunk_pages[size_ix(pisize)])
	{
		ret = (pageinfo*)get_page();
		set_pageinfo(ret, MAGIC_PAGE_PGINFO);
	}
	else
	{
		ret = (pageinfo*)malloc_unlocked(pisize);
	}

	memset(&ret->heap, 0, sizeof(ret->heap));
	ret->page = get_page();
	ret->size = size;
//	ret->shift = ix_shift(ix);
	ret->chunks = nchunks;
	ret->chunks_free = nchunks;
	ret->index = ix;

	memset(ret->bitmap, 0xff, (nchunks+7)/8);

	set_pageinfo(ret->page, ret);

	return ret;
}

static pageinfo* get_pageinfo(void* ptr)
{
	uintptr_t offset = ((uintptr_t)ptr - g_first_page) >> PAGE_SHIFT;
	if (unlikely(offset > g_n_pages))
	{
		return NULL;
	}
	else
	{
		pageinfo* ret = g_pages[offset];
		assert(!ret || IS_MAGIC_PAGE(ret) || !(((uintptr_t)ret->page ^ (uintptr_t)ptr) >> PAGE_SHIFT));
		return ret;
	}
}

static size_t get_magic_page_size(pageinfo* info, void* ptr)
{
	assert(info == MAGIC_PAGE_FIRST);
	uintptr_t n = ((uintptr_t)ptr - g_first_page) >> PAGE_SHIFT;
	pageinfo** start = g_pages + n;
	pageinfo** p = g_pages + n + 1;
	pageinfo** end = g_pages + g_n_pages;
	while (p < end && *p == MAGIC_PAGE_FOLLO)
	{
		p++;
	}
	return p - start;
}

static size_t get_alloc_size(void* ptr)
{
	pageinfo* info = get_pageinfo(ptr);
	if (unlikely(IS_MAGIC_PAGE(info)))
	{
		return PAGE_SIZE * get_magic_page_size(info, ptr);
	}
	if (unlikely(!info)) panic("get_alloc_size for unknown pointer %p", ptr);
	return info->size;
}

static void *malloc_unlocked(size_t size)
{
	if (unlikely(!size))
	{
#ifdef ZERO_ALLOC_RETURNS_NULL
		return NULL;
#else
		size++;
#endif
	}

	if (unlikely(size > PAGE_SIZE / 2))
	{
		size_t npages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
		debug("Allocating %ld from %ld fresh pages\n", size, npages);
		void* ret = get_pages(npages);
		debug("X ALLOC %p\n", ret);
		return ret;
	}

	pairing_ptr_heap* pagep = g_chunk_pages + size_ix(size);
	pairing_ptr_node* min = get_min(*pagep);
	pageinfo* page = min ? pageinfo_from_heap(min) : NULL;
	if (unlikely(!page))
	{
		debug("Adding new chunk page for size %lu (cat %ld, size %ld)\n", size, size_ix(size), ix_size(size_ix(size)));
		page = new_chunkpage(size);
		insert(*pagep, &page->heap);
	}
	debug("Allocating %ld from %p (info %p, %d left)\n", size, page->page, page, page->chunks_free);
	void* ret = page_get_chunk(page);
	debug("Allocated %p (%d bytes)\n", ret, page->size);
	debug("X ALLOC %p\n", ret);
	if (unlikely(page_filled(page)))
	{
		debug("Page %p (info %p) filled\n", page->page, page);
		delete_min(*pagep);
	}
	assert(ret);
#ifdef MALLOC_CLEAR_MEM
	if (likely(ret))
	{
		memset(ret, MALLOC_CLEAR_MEM, size);
		memset((u8*)ret + size, MALLOC_CLEAR_MEM_AFTER, page->size - size);
	}
#endif
	return ret;
}

void* malloc(size_t size)
{
	SCOPELOCK();
	return malloc_unlocked(size);
}

static int posix_memalign_unlocked(void** ret, size_t align, size_t size)
{
	// TODO Check that align is a power-of-two and larger than sizeof(void*)

	// Everything is 16-byte aligned in this malloc
	if (align <= 16)
	{
		*ret = malloc_unlocked(align);
		return *ret ? 0 : ENOMEM;
	}

	if (align > 4096)
	{
		if (align % 4096)
		{
			*ret = 0;
			return EINVAL;
		}
		xassert(align < 16*1024*1024);
		void* old_break = sbrk(0);
		while ((uintptr_t)old_break % align)
		{
			old_break = (u8*)sbrk(PAGE_SIZE) + PAGE_SIZE;
			set_pageinfo((u8*)old_break - PAGE_SIZE, NULL);
		}
		size_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
		if (npages == 1) npages++;
		*ret = get_pages(npages);
	}
	else
	{
		// All powers of two will be aligned on powers of two, up to 4096 anyway
		size_t aligned_size = ((size + align - 1) / align) * align;
		*ret = malloc_unlocked(aligned_size);
	}
	assert(!((uintptr_t)*ret % align));
	return *ret ? 0 : ENOMEM;
}

extern "C" int posix_memalign(void** ret, size_t align, size_t size) __attribute__((visibility("default")));
int posix_memalign(void** ret, size_t align, size_t size)
{
	SCOPELOCK();
	return posix_memalign_unlocked(ret, align, size);
}

void* calloc(size_t n, size_t sz)
{
	size_t size = n * sz;
	void* ptr = malloc(size);
	if (likely(ptr)) memset(ptr, 0, size);
	return ptr;
}

void* realloc_unlocked(void* ptr, size_t new_size)
{
	if (!ptr)
	{
		return malloc_unlocked(new_size);
	}

	// If new_size is 0, successfully free ptr. But it's not clear if you must
	// return non-null. Returning NULL normally means that allocation failed
	// and the old allocation is untouched!
	size_t old_size = get_alloc_size(ptr);
	debug("Reallocating %p from %lu to %lu\n", ptr, old_size, new_size);

	if (unlikely(!new_size))
	{
		free_unlocked(ptr);
		// Since I have no better idea, reuse the arbitrary decision made for
		// malloc(0) here (compile-time ifdef)
		void* ret = malloc_unlocked(0);
		debug("X REALLOC %p %lu %p %lu\n", ptr, old_size, ret, new_size);
		return ret;
	}

	if (new_size <= old_size)
	{
		return ptr;
	}

	void* ret = malloc_unlocked(new_size);
	assert(ret);
	if (likely(ret))
	{
		memcpy(ret, ptr, new_size < old_size ? new_size : old_size);
		free_unlocked(ptr);
	}
	debug("X REALLOC %p %lu %p %lu\n", ptr, old_size, ret, new_size);
	return ret;
}

void* realloc(void* ptr, size_t new_size)
{
	SCOPELOCK();
	return realloc_unlocked(ptr, new_size);
}

#ifdef DEBUG
static bool check_page_alignment(pageinfo* page, void* ptr)
{
	ptrdiff_t pos = (u8*)ptr - (u8*)page->page;
	return !(pos % page->size);
}
#endif

static void free_magic_page(pageinfo* magic, void* ptr)
{
	debug("Free magic page %p (magic %ld)\n", ptr, (uintptr_t)magic);
	assert(magic == MAGIC_PAGE_FIRST);
	size_t npages = get_magic_page_size(magic, ptr);
	debug("Free: Page %p (%ld pages)\n", ptr, npages);
	while (npages--)
	{
		IFDEBUG(
		void* page = (u8*)ptr + npages * PAGE_SIZE;
		pageinfo* info = get_pageinfo(page);
		assert(IS_MAGIC_PAGE(info) && (info == MAGIC_PAGE_FIRST || info == MAGIC_PAGE_FOLLO));
		)
		free_page((u8*)ptr + npages * PAGE_SIZE);
	}
}

//#define FREE_IS_NOT_FREE

void free(void* ptr)
{
	SCOPELOCK();
	return free_unlocked(ptr);
}

static void free_unlocked(void *ptr)
{
	if (unlikely(!ptr)) return;

	debug("X FREE %p\n", ptr);

	pageinfo* page = get_pageinfo(ptr);
	if (unlikely(IS_MAGIC_PAGE(page)))
	{
#ifndef FREE_IS_NOT_FREE
		free_magic_page(page, ptr);
#endif
		return;
	}
	if (unlikely(!page)) panic("free on unknown pointer %p", ptr);
	assert(check_page_alignment(page, ptr));

#ifdef FREE_CLEAR_MEM
	memset(ptr, FREE_CLEAR_MEM, page->size);
#endif

#ifndef FREE_IS_NOT_FREE
	IFDEBUG(dump_pages();)
	page_free_chunk(page, ptr);
	IFDEBUG(dump_pages();)

	if (page->chunks_free == 1)
	{
		insert(g_chunk_pages[page->index], &page->heap);
		IFDEBUG(dump_pages();)
	}
	else if (unlikely(page->chunks_free == page->chunks))
	{
		debug("Free: page %p (info %p) is now free\n", page->page, page);
		// TODO Logic for page reuse.
		// For now: keep it in the free-heap for the chunksize regardless.
		// We can't easily extradict it from the pairing heap :)

		/*
		 * maybe something like this:
		 * if first free page for chunk size, keep it
		 * else: delete from pairing heap and put on page-free list
		 */
	}
#endif
}

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
			IFDEBUG(dump_pages());
			ptrs[imalloc] = malloc(size);
			IFDEBUG(dump_pages());
		}
	}
	for (size_t i = 0; i < DELAY; i++)
	{
		free(ptrs[i]);
	}
	
	selftest_realloc();
}

pthread_t get_owner()
{
	static pthread_t g_owner = 0;
	if (!g_owner)
		g_owner = pthread_self();
	return g_owner;
}

void init() __attribute__((constructor));
void init()
{
	selftest();
}

#ifdef TEST
int main()
{
	selftest();
	printf("\"OK, dumping left-over state:\"!\n");
	dump_pages();
	printf("\"OK\"!\n");
}
#endif
