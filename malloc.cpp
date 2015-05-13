#define _POSIX_C_SOURCE 200112L

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <malloc.h>
#include <signal.h>
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

#define EXPORT __attribute__((visibility("default")))

extern "C" {
	// Exported only by us
	void dump_pages() EXPORT;
	// Not marked as visible by malloc.h.
	struct mallinfo mallinfo() EXPORT;
	int malloc_trim(size_t pad) EXPORT;
}

#include "utils.h"

//#define LOG_MMAP
#if defined(DEBUG) || defined(LOG_MMAP)
static void* mmap_wrap(void* ptr, size_t length, int prot, int flags, int fd, off_t offset)
{
	xprintf("mmap(%lu)\n", (unsigned long)length);
	return mmap(ptr, length, prot, flags, fd, offset);
}
static int munmap_wrap(void* ptr, size_t length)
{
	xprintf("munmap(%lu)\n", (unsigned long)length);
	return munmap(ptr, length);
}
#define munmap munmap_wrap
#define mmap mmap_wrap
#endif

static void free_unlocked(void* ptr);
static void* malloc_unlocked(size_t size);
static void* realloc_unlocked(void* ptr, size_t new_size);

#ifdef DEBUG
#define MALLOC_CLEAR_MEM 0xcc
#define MALLOC_CLEAR_MEM_AFTER 0xfd
#define FREE_CLEAR_MEM 0xdd
#endif

#define USE_SPINLOCKS

#if defined(USE_SPINLOCKS)
#define NEED_INIT_LOCK
static void init_lock();
namespace mallock
{
	pthread_spinlock_t spinlock;

	static void init()
	{
		int res = pthread_spin_init(&spinlock, PTHREAD_PROCESS_PRIVATE);
		xassert(res == 0);
	}
	static void lock()
	{
		init_lock();
		int res = pthread_spin_lock(&spinlock);
		xassert(res == 0);
	}
	static void unlock()
	{
		init_lock();
		int res = pthread_spin_unlock(&spinlock);
		xassert(res == 0);
	}
};
#elif defined(USE_MUTEX)
namespace mallock
{
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

	static void lock()
	{
		int res = pthread_mutex_lock(&mutex);
		xassert(res == 0);
	}
	static void unlock()
	{
		int res = pthread_mutex_unlock(&mutex);
		xassert(res == 0);
	}
};
#else
#error Choose your poison
#endif
#ifdef NEED_INIT_LOCK
void init_lock()
{
	static pthread_once_t mallock_init = PTHREAD_ONCE_INIT;
	pthread_once(&mallock_init, mallock::init);
}
#endif

class scopelock
{
	scopelock(const scopelock&) = delete;
	scopelock& operator=(const scopelock&) = delete;
public:
	scopelock()
	{
		mallock::lock();
	}
	~scopelock()
	{
		mallock::unlock();
	}
};

#define SCOPELOCK(e) scopelock scopelock_ ## e

#if 1
#include "splay_tree.cpp"

#define NODE splay_node
#define TREE(node) splay_tree<node>
#else
#include "pairing_heap.cpp"

#define NODE pairing_ptr_node
#define TREE(node) pairing_ptr_heap
#endif

typedef NODE chunkpage_node;
typedef TREE(chunkpage_node) chunkpage_heap;

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
	chunkpage_node heap;

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
static size_t clear_first_set_bit(u8* bitmap, size_t maxbit)
{
	unsigned maxbyte = 8 * ((maxbit + 63u) / 64u);
	u8* start = bitmap;
	u64 found;

	while (maxbyte)
	{
		if (likely(found = *(u64*)bitmap))
		{
			goto found64;
		}
		bitmap += 8;
		maxbyte -= 8;
	}
	panic("No free chunks found?");

found64:
#ifdef __x86_64__
	u64 ix;
	__asm__("bsf %1, %0" : "=r" (ix) : "r" (found));
	*(u64*)bitmap = found ^ ((u64)1 << ix);
	return ix + ((bitmap - start) << 3);
#else
	size_t n = bitmap - start;

	u32 temp = found;
	if (!likely((u32)found))
		n += 4, temp = found >> 32;
	if (!likely((u16)temp))
		n += 2, temp >>= 16;
	if (!likely((u8)temp))
		n++;

	n <<= 3;
	u64 mask = (u64)1 << (n & (7 * 8));
	u64 maskmask = (u64)0xff << (n & (7 * 8));
	do
	{
		assert(mask == ((u64)1 << (n & 63)));
		if (mask & found)
		{
			*(u64*)bitmap = found & ~mask;
			return n;
		}
		mask <<= 1;
		n++;
	}
	while (mask & maskmask);
	panic("No bits set in word?");
#endif
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
	assert(n * page->size < PAGE_SIZE);
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

struct chunk_stats
{
	size_t allocated;
	size_t freed;
	size_t total_waste;
};

#define N_SIZES (128/16+4)
#define MAX_CHUNK_PAGE_SIZE (2048)

typedef NODE freepage_node;
typedef TREE(freepage_node) freepage_heap;

static freepage_heap g_free_pages;
static size_t g_n_free_pages;
static size_t g_spare_pages_wanted;
static size_t g_max_pages, g_added_pages, g_used_pages;
static size_t g_used_bytes;

static chunkpage_heap g_chunk_pages[N_SIZES];
static chunk_stats g_chunk_stats[N_SIZES];
static uintptr_t g_first_page;
static uintptr_t g_n_pages;
// Assumes mostly contiguous pages...
static pageinfo** g_pages;
static timer_t g_timerid;

static void set_timer(int sec);
static void set_timer_on_free();

static void set_pageinfo(void* page, pageinfo* info);
static pageinfo* get_pageinfo(void* ptr);
static size_t get_magic_page_size(pageinfo* info, void* ptr);

#define MAGIC_PAGE_OTHER ((pageinfo*)0)
#define MAGIC_PAGE_FIRST ((pageinfo*)1)
#define MAGIC_PAGE_FOLLO ((pageinfo*)2)
#define MAGIC_PAGE_PGINFO ((pageinfo*)3)
#define MAGIC_PAGE_FREE ((pageinfo*)4)
#define LAST_MAGIC_PAGE ((pageinfo*)5)
#define IS_MAGIC_PAGE(page) ((page) && ((page) < LAST_MAGIC_PAGE))

static size_t large_size_ix(size_t size_)
{
	assert(size_ <= 2048);
	uint16_t size = (uint16_t)size_;
	if (likely(size <= 256))
		return 8;
	else if (likely(size <= 512))
		return 9;
	else if (likely(size <= 1024))
		return 10;
	else
		return 11;
}

static size_t size_ix(size_t size)
{
	if (likely(size <= 128))
	{
		return (size - 1) / 16;
	}
	else
	{
		return large_size_ix(size);
	}
}

static size_t ix_size(size_t ix)
{
	return ix < 8 ? 16 * (ix + 1) : (1 << ix);
}

static void add_used_pages(size_t n)
{
	g_used_pages += n;
	if (g_used_pages > g_max_pages) {
		assert((ssize_t)g_used_pages > 0);
		g_max_pages = g_used_pages;
	}
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
		ret = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
		g_added_pages++;
	}
	add_used_pages(1);
	debug("get_page: %p\n", ret);
	return ret;
}

static void add_to_freelist(void* page)
{
	debug("Adding %p to page free-list\n", page);
	memset(page, 0, sizeof(freepage_node));
	insert(g_free_pages, (freepage_node*)page);
	//dump_heap(g_free_pages);
	g_n_free_pages++;
	g_used_pages--;
	set_pageinfo(page, MAGIC_PAGE_FREE);
}

static void free_page(void* page)
{
	add_to_freelist(page);
	// TODO Start trim timer (if necessary)
	//dump_pages();
}

static void free_magic_page(pageinfo* magic, void* ptr)
{
	debug("Free magic page %p (magic %ld)\n", ptr, (uintptr_t)magic);
	assert(magic == MAGIC_PAGE_FIRST);
	size_t npages = get_magic_page_size(magic, ptr);
	size_t reusepages = 0;
	debug("Free: Page %p (%ld pages)\n", ptr, npages);
	g_used_bytes -= npages * PAGE_SIZE;
	reusepages = npages;
	npages -= reusepages;
	u8* page = (u8*)ptr;
	while (reusepages--)
	{
		IFDEBUG(
		pageinfo* info = get_pageinfo(page);
		assert(info == MAGIC_PAGE_FIRST || info == MAGIC_PAGE_FOLLO);
		)
		free_page(page);
		page += PAGE_SIZE;
	}
	if (npages)
	{
		u8* start = page;
		while (npages--)
		{
			set_pageinfo(page, NULL);
			page += PAGE_SIZE;
		}
		munmap(start, page - start);
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
		ret = mmap(0, n * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
		g_added_pages += n;
		add_used_pages(n);
	}
	debug("get_pages: %ld pages: %p\n", n, ret);
	if (unlikely(ret == (void*)-1))
	{
		xprintf("get_pages: %ld pages: %p\n", n, ret);
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

static size_t page_allocated_space(pageinfo* page)
{
	return (page->chunks - page->chunks_free) * page->size;
}

static void dump_chunk_stats() {
	size_t total_size = 0, total_waste = 0;
	for (size_t i = 0; i < N_SIZES; i++) {
		chunk_stats& stats = g_chunk_stats[i];
		const size_t size = ix_size(i);
		printf("Chunk %3zu [size %4zu]: %zu allocs, %zu frees, %zu of %zu bytes wasted\n",
			i, size, stats.allocated, stats.freed, stats.total_waste, stats.allocated * size);
		const size_t waste_pm =
			stats.allocated ? (1000 * stats.total_waste) / (stats.allocated * size) : 0;
		const size_t ext_waste_pm = 1000 * (PAGE_SIZE % size) / PAGE_SIZE;
		printf("Chunk %3zu [size %4zu]: waste %d.%d%% (int) %d.%d%% (ext per page)\n",
			i, size, waste_pm / 10, waste_pm % 10, ext_waste_pm / 10, ext_waste_pm % 10);

		total_size += stats.allocated * size;
		total_waste += stats.total_waste;
		// total_ext_waste += // needs to know the number of (currently) used
		// pages to multiply the waste by...
	}
	const size_t waste_pm = total_size ? (1000 * total_waste) / total_size : 0;
	printf("All chunks: %zu of %zu bytes wasted, %d.%d%%\n",
			total_waste, total_size, waste_pm / 10, waste_pm % 10);
}

void dump_pages()
{
	printf("Size-class statistics:\n");
	dump_chunk_stats();
	printf("Current (head-of-heap) chunk-pages:\n");
	bool corrupt = false;
	for (size_t i = 0; i < N_SIZES; i++)
	{
		chunkpage_node* min = get_min(g_chunk_pages[i]);
		if (min)
		{
			pageinfo* page = pageinfo_from_heap(min);
			size_t size = ix_size(i);
			printf("%p: %zu, size %zu: ", page->page, i, size);
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
	size_t chunk_pages = 0;
	size_t used_pages = 0;

	pageinfo** pagep = g_pages;
	pageinfo** end = pagep + g_n_pages;
	u8* addr = (u8*)g_first_page;
	u8* last_non_free = NULL;
	while (pagep < end)
	{
		pageinfo* page = *pagep++;
		bool free = false;
		if (unlikely(IS_MAGIC_PAGE(page)))
		{
			if (page == MAGIC_PAGE_FIRST)
			{
				pageinfo** first = pagep;
				while (*pagep == MAGIC_PAGE_FOLLO)
					pagep++;
				size_t npages = 1 + pagep - first;
				magic_pages += npages;
				used_pages += npages;
				printf("%p: %zu page(s) large alloc\n", addr, npages);
				addr += (npages - 1) * PAGE_SIZE;
			}
			else if (page == MAGIC_PAGE_FREE)
			{
				size_t n = 1;
				while (pagep < end && *pagep == MAGIC_PAGE_FREE)
				{
					pagep++;
					n++;
				}
				printf("%p: %zu page(s) free-list\n", addr, n);
				addr += (n - 1) *PAGE_SIZE;
				freelist_pages += n;
				free = true;
			}
			else if (page == MAGIC_PAGE_PGINFO)
			{
				pginfo_pages++;
				used_pages++;
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
			used_pages++;
			chunk_pages++;
		}
		else
		{
			//printf("%p: Not used (%p)\n", addr, page);
			free = true;
		}
		if (!free)
			last_non_free = addr;
		addr += 4096;
	}
	printf(":pmud egaP\n");
	size_t p = allocated ? 10000 * used / allocated : 0;
	printf("Used %zu of %zu (%d.%02d%%)\n", used, allocated, p / 100, p % 100);
	printf("Pages: %zu used (%zu) %zu freelist (%zu) %zu large allocs %zu pageinfo %zu chunkpages %zu unknown\n", used_pages, g_used_pages, freelist_pages, g_n_free_pages, magic_pages, pginfo_pages, chunk_pages, unknown_magic);
	size_t total_pages = freelist_pages + magic_pages + pginfo_pages + chunk_pages;
	printf( "last non-free page %p\n"
			"first page         %p\n", last_non_free, g_first_page);
	printf("%ld bytes in known pages\n", total_pages * PAGE_SIZE);
	printf("%ld bytes covered by page table (%ld bytes)\n", g_n_pages * PAGE_SIZE, g_n_pages * sizeof(pageinfo*));
	fflush(stdout);
	assert(!corrupt);
	(void)corrupt; // Silence unused-var warning
	assert(freelist_pages == g_n_free_pages);
	assert(used == g_used_pages);
	assert(!unknown_magic);
}

#undef COMPACT_PAGEINFO
#ifdef COMPACT_PAGEINFO
static void compact_pageinfo()
{
	intptr_t first_non_zero = -1;
	intptr_t last_non_zero = -1;
	for (size_t i = 0; i < g_n_pages; i++)
	{
		if (g_pages[i])
		{
			first_non_zero = i;
			break;
		}
	}
	for (size_t i = g_n_pages; i; i--)
	{
		if (g_pages[i - 1])
		{
			last_non_zero = i;
			break;
		}
	}
	// Whole thing is zero, free it
	if (first_non_zero < 0)
	{
		printf("Page table empty: freeing everything");
		munmap(g_pages, g_n_pages * sizeof(pageinfo*));
		g_n_pages = 0;
		g_pages = NULL;
		g_first_page = 0;
		return;
	}

	intptr_t tail_unused = last_non_zero >= 0 ? g_n_pages - last_non_zero : 0;
	debug("Could save %ld + %ld pagetable entries\n", first_non_zero, tail_unused);

	intptr_t entries_per_page = PAGE_SIZE / sizeof(pageinfo*);
	if (tail_unused >= entries_per_page)
	{
		size_t remove_entries = (tail_unused / entries_per_page) * entries_per_page;
		printf("Removing %ld unused entries at end\n", remove_entries);
		munmap(g_pages + g_n_pages - remove_entries, remove_entries * sizeof(pageinfo*));
		g_n_pages -= remove_entries;
	}

	if (first_non_zero >= entries_per_page)
	{
		size_t adjust_entries = (first_non_zero / entries_per_page) * entries_per_page;
		printf("Removing %ld unused entries at start, new table size %ld\n", adjust_entries, g_n_pages - adjust_entries);
		munmap(g_pages, adjust_entries * sizeof(pageinfo*));
		g_n_pages -= adjust_entries;
		g_first_page += adjust_entries * PAGE_SIZE;
		g_pages += adjust_entries;

		//dump_pages();
	}
}
#endif

static void set_pageinfo(void* page, pageinfo* info)
{
	if (unlikely(!g_pages))
	{
		g_first_page = (uintptr_t)page;
	}
	intptr_t offset = ((intptr_t)page - (intptr_t)g_first_page) >> PAGE_SHIFT;

	debug("set_pageinfo: Page %p info %p\n", page, info);

#ifdef DEBUG
	uintptr_t old_page = g_first_page;
	pageinfo* old_pageinfo = NULL;
	if (g_n_pages)
	{
		for (size_t i = 0; i < g_n_pages; i++, old_page += 4096)
		{
			if ((old_pageinfo = g_pages[i]))
			{
				break;
			}
		}
	}
#endif

	if (unlikely((uintptr_t)offset >= g_n_pages || offset < 0))
	{
		size_t required = offset < 0 ? g_n_pages - offset : offset;
		required = (sizeof(pageinfo*) * required + PAGE_SIZE) & ~(PAGE_SIZE-1);
		debug("Resizing page table from %ld to %ld\n", g_n_pages, required / sizeof(pageinfo*));
		pageinfo** new_pages = (pageinfo**)mmap(NULL, required, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
		assert(new_pages != MAP_FAILED);

		IFDEBUG(uintptr_t old_first_page = g_first_page;)
		if (offset < 0)
		{
			size_t adjustment = (-offset * sizeof(pageinfo*) + PAGE_SIZE) & ~(PAGE_SIZE - 1);
			memcpy((u8*)new_pages + adjustment, g_pages, g_n_pages * sizeof(pageinfo*));
			g_first_page -= adjustment * PAGE_SIZE / sizeof(pageinfo*);
			offset += adjustment / sizeof(pageinfo*);
		}
		else
		{
			memcpy(new_pages, g_pages, g_n_pages * sizeof(pageinfo*));
		}
		munmap(g_pages, g_n_pages * sizeof(pageinfo*));
		debug("Moved page table from %p (%ld) %p to %p (%ld) %p\n", g_pages, g_n_pages, old_first_page, new_pages, required / sizeof(pageinfo*), g_first_page);

#ifdef DEBUG
		if (g_n_pages)
		{
			uintptr_t new_offset = ((intptr_t)old_page - (intptr_t)g_first_page) >> PAGE_SHIFT;
			assert(new_offset < required / sizeof(pageinfo*) && new_offset >= 0);
			assert(new_pages[new_offset] == old_pageinfo);
		}
#endif

		g_pages = new_pages;
		g_n_pages = required / sizeof(pageinfo*);
	}

	g_pages[offset] = info;
	debug("set_pageinfo: Page %p info %p\n", page, g_pages[offset]);

#ifdef COMPACT_PAGEINFO
	compact_pageinfo();
#endif
}

static pageinfo* new_chunkpage(size_t size)
{
	size_t ix = size_ix(size);
	assert(ix_size(ix) >= size);
	size = ix_size(ix);

	size_t nchunks = 4096/size;
	size_t bitmapwords = (nchunks + 63) / 64;
	pageinfo* ret = NULL;
	size_t pisize = sizeof(pageinfo) + 8 * bitmapwords;
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

	memset(ret->bitmap, 0, 8 * bitmapwords);
	memset(ret->bitmap, 0xff, nchunks / 8);
	ret->bitmap[nchunks/8] = (1 << (nchunks & 7)) - 1;

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

	if (unlikely(size > MAX_CHUNK_PAGE_SIZE))
	{
		size_t npages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
		debug("Allocating %ld from %ld fresh pages\n", size, npages);
		void* ret = get_pages(npages);
		xassert(ret);
		g_used_bytes += npages * PAGE_SIZE;
		debug("X ALLOC %p\n", ret);
		return ret;
	}

	const size_t ix = size_ix(size);
	chunkpage_heap* pagep = g_chunk_pages + ix;
	chunkpage_node* min = get_min(*pagep);
	pageinfo* page = pageinfo_from_heap(min);
	if (unlikely(!min))
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
	xassert(ret);
#ifdef MALLOC_CLEAR_MEM
	if (likely(ret))
	{
		memset(ret, MALLOC_CLEAR_MEM, size);
		memset((u8*)ret + size, MALLOC_CLEAR_MEM_AFTER, page->size - size);
	}
#endif
	const size_t used_size = ix_size(ix);
	g_chunk_stats[ix].allocated++;
	g_chunk_stats[ix].total_waste += used_size - size;
	g_used_bytes += used_size;
	return ret;
}

void* malloc(size_t size)
{
	SCOPELOCK();
	return malloc_unlocked(size);
}

static int posix_memalign_unlocked(void** ret, size_t align, size_t size)
{
	*ret = NULL;

	if (align & (align - 1)) // x is not a power of two
	{
		return EINVAL;
	}

	// Everything is 16-byte aligned in this malloc
	if (align <= 16)
	{
		*ret = malloc_unlocked(align);
		return *ret ? 0 : ENOMEM;
	}

	if (align > PAGE_SIZE)
	{
		if (align % PAGE_SIZE)
		{
			return EINVAL;
		}
		xassert(align < 16*1024*1024);
		size_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

		// Map some extra space, then unmap it afterwards :)
		u8* page = (u8*)mmap(0, align + npages * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
		if (page != MAP_FAILED)
		{
			u8* orig_page = page;
			u8* allocated_end = page + align + npages * PAGE_SIZE;
			uintptr_t misalign = (uintptr_t)page % align;
			if (misalign)
			{
				page += align - misalign;
				// Unmap header
				munmap(orig_page, page - orig_page);
			}
			u8* used_end = page + npages * PAGE_SIZE;
			// Unmap footer
			munmap(used_end, allocated_end - used_end);
			*ret = page;
		}
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
	xassert(ret);
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

//#define FREE_IS_NOT_FREE

void free(void* ptr)
{
	SCOPELOCK();
	return free_unlocked(ptr);
}

static void free_unlocked(void *ptr)
{
	if (unlikely(!ptr)) return;

	set_timer_on_free();

	debug("X FREE %p\n", ptr);

	pageinfo* page = get_pageinfo(ptr);
	if (unlikely(IS_MAGIC_PAGE(page)))
	{
#ifndef FREE_IS_NOT_FREE
		if (page == MAGIC_PAGE_FIRST)
			free_magic_page(page, ptr);
		else if (page == MAGIC_PAGE_PGINFO)
			free_page(ptr);
		else
			xassert(false);
#endif
		return;
	}
	if (unlikely(!page)) panic("free on unknown pointer %p", ptr);
	assert(check_page_alignment(page, ptr));

#ifdef FREE_CLEAR_MEM
	memset(ptr, FREE_CLEAR_MEM, page->size);
#endif

	g_used_bytes -= page->size;
	g_chunk_stats[page->index].freed++;

#ifndef FREE_IS_NOT_FREE
	page_free_chunk(page, ptr);

	if (page->chunks_free == 1)
	{
		insert(g_chunk_pages[page->index], &page->heap);
	}
	else if (unlikely(page->chunks_free == page->chunks))
	{
		debug("Free: page %p (info %p) is now free\n", page->page, page);
		chunkpage_heap& heap = g_chunk_pages[page->index];
		if (&page->heap != get_min(heap) /* && heap.size() > N */)
		{
			//debug("PRE RM %p\n", &page->heap);
			//dump_heap(heap);
			remove(heap, &page->heap);
			//debug("POST RM %p\n", &page->heap);
			//dump_heap(heap);
			free_page(page->page);
			page->page = 0;
			free_unlocked(page);
		}
	}
#endif
}

// 4MB is not a lot to keep laying around. But there should rather be some kind
// of hysteresis mechanism.
static const size_t SPARE_PAGES = 1024;

#if 0
#define trim_debug(fmt, ...) xfprintf(stderr, "%d: " fmt, getpid(), ## __VA_ARGS__)
#else
#define trim_debug(...) (void)0
#endif

// Some of the things we want here:
// 1. Collect the amount of unused memorytime between two measurements. The
//    maximum actual usage in this time represents the minimum amount we should
//    have had allocated. Any excess is unused. (The maximum usage may have
//    been at any time between the measurements and this is not accounted for.)
// 2. Collect the amount of extra memory allocated in the time period.
//
// 1. Gives us an indication of how much unneeded memory we have, try to
//    reduce this to a small amount. e.g.
//    max-use + spare pages = target value,
//    d = actual - target,
//    free up to d/2 each iteration, at least d/4.
// 2. Gives us an indication of how much we've over-freed? But it will also
//    happen naturally as a change in the working set size.
//    Every time we hit the ceiling, increase spare_pages to try to have more
//    memory ready for next time the working set increases.
int malloc_trim(size_t pad)
{
	// Should be a small enough value that it "never matters" if we waste this
	// amount of memory. 1MB chosen somewhat arbitrarily.
	// This is supposed to prevent churn just trimming handfuls of pages over
	// and over.
	const size_t MIN_SPARES = 256;
	const size_t MIN_TRIM = 256;
	size_t free_pages, spare_pages_wanted, added_pages, used_pages, max_pages;
	{
		SCOPELOCK();
		free_pages = g_n_free_pages;
		spare_pages_wanted = g_spare_pages_wanted;
		added_pages = g_added_pages;
		max_pages = g_max_pages;
		used_pages = g_used_pages;
		g_added_pages = 0;
		g_max_pages = g_used_pages;

		size_t upper_spare = max_pages - used_pages;
		size_t lower_spare = added_pages;
		size_t orig_spare = spare_pages_wanted;
		spare_pages_wanted = lower_spare + orig_spare + upper_spare;
		spare_pages_wanted /= 3;
		if (spare_pages_wanted < added_pages) spare_pages_wanted = added_pages;
		if (spare_pages_wanted < MIN_SPARES) spare_pages_wanted = MIN_SPARES;
		trim_debug("Trim: spare adjustment %zu .. %zu .. %zu => %zu\n",
				lower_spare, orig_spare, upper_spare, spare_pages_wanted);
		g_spare_pages_wanted = spare_pages_wanted;
	}

	if (!free_pages)
	{
		// Nothing to do.
		return 0;
	}
	int pages_trimmed = 0;

	trim_debug("Trim: %zu free pages, %zu spare wanted.\n",
			free_pages, spare_pages_wanted);
	trim_debug("Trim: %zu added, %zu used now, %zd max since last, %zu used+free\n",
			added_pages, used_pages, max_pages, used_pages + free_pages);

	size_t spare_pages_now = (free_pages + spare_pages_wanted) >> 1;
	trim_debug("Trim: Aiming for %zu spares (freeing %zd).\n",
			spare_pages_now, free_pages - spare_pages_now);
	if (free_pages - spare_pages_now < MIN_TRIM)
	{
		trim_debug("Trim: Less than MIN_TRIM, skipping.\n");
		return 0;
	}

	while (free_pages > spare_pages_now)
	{
		u8* block_start;
		u8* block_end;
		{
			SCOPELOCK();
			// Might have changed if another free happened.
			free_pages = g_n_free_pages;

			u8* last_page = (u8*)get_max(g_free_pages);
			block_end = last_page + PAGE_SIZE;

			do
			{
				remove(g_free_pages, (freepage_node*)last_page);
				free_pages--;
				set_pageinfo(last_page, 0);
				pages_trimmed++;

				block_start = last_page;
				last_page = (u8*)get_max(g_free_pages);
			}
			while (last_page + 4096 == block_start);

			g_n_free_pages = free_pages;
			// End of lock scope
		}

		trim_debug("Trim: Freeing %zu pages.\n", (block_end - block_start) / PAGE_SIZE);
		int res = munmap(block_start, block_end - block_start);
		if (res != 0) perror("munmap");
	}

	return pages_trimmed;
}

static volatile sig_atomic_t timer_is_scheduled = 0;
static void timer_function(union sigval)
{
	timer_is_scheduled = false;
	trim_debug("Trim: timer triggered\n");
	int trimmed = malloc_trim(0);
	// Set trim interval to 0 to disable the timer.
	// Some ideas on interval:
	// - 0 if there was nothing to do. free() will arm the timer later.
	//   (But perhaps it's better to keep the timer but increase the interval
	//   every time nothing happens, to avoid free() doing anything expensive.
	//   It would "just" need to poke a flag to see if the timer is already
	//   scheduled.)
	// - 1 otherwise. Assume we did only a little work (freed 10% of free pages
	//   or whatever).
	int trim_timer_interval = 1;
	trim_debug("Trim: %d pages trimmed\n", trimmed);
	(void)trimmed;
	set_timer(trim_timer_interval);
}

static void init_timer() __attribute__((constructor));
static void init_timer()
{
	sigevent sigevt;
	memset(&sigevt, 0, sizeof(sigevt));
	sigevt.sigev_notify = SIGEV_THREAD;
	sigevt.sigev_notify_function = timer_function;
	int res = timer_create(CLOCK_MONOTONIC, &sigevt, &g_timerid);
	if (res != 0) {
		perror("malloc timer_create");
		exit(1);
	}
	timer_is_scheduled = false;
	// Re-init the timer in the child.
	pthread_atfork(NULL, NULL, init_timer);
}
// Only has an effect if the timer is previously unscheduled.
static void set_timer(int sec)
{
	if (timer_is_scheduled) {
		return;
	}
	// TODO Set these so that all malloc timers (at the same interval) sync up.
	itimerspec timerspec;
	memset(&timerspec, 0, sizeof(timerspec));
	timerspec.it_value.tv_sec = sec;
	int res = timer_settime(g_timerid, 0 /* flags */, &timerspec, NULL);
	if (res != 0) {
		perror("timer_settime");
	}
	timer_is_scheduled = true;
}
static void set_timer_on_free() {
	set_timer(1);
}

#if defined(DEBUG) || defined(TEST)
static void ix_test() __attribute__((constructor));
void ix_test()
{
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
}
#endif

struct mallinfo mallinfo() {
	struct mallinfo res;
	memset(&res, 0, sizeof(res));

	SCOPELOCK();

	dump_chunk_stats();
//	dump_pages();

	// Get some almost, but not completely, incorrect information about the
	// heap, trying to emulate glibc's output.
	//
	// arena: bytes of "main" non-mmap heap space (including free)
	// hblks: number of mmapped blocks in heap (including free)
	// hblkhd: number of bytes in hblks
	//
	// ordblks: number of ordinary blocks (including free)
	// uordblks: used bytes in ordinary blocks
	// fordblks: free bytes in ordinary blocks
	//
	// smblks: number of fastbin blocks
	// usmblks: -used bytes in fastbin blocks- "highwater mark" for allocated space.
	// fsmblks: free bytes in fastbin blocks
	//
	// Fastbin blocks are not used by sbmalloc, and the corresponding fields
	// are all 0. There is no "arena" in sbmalloc, so that will also be 0.
	//
	// Each used heap page is "one" ordinary block.

	res.hblks = g_n_free_pages + g_used_pages;
	res.hblkhd = res.hblks * PAGE_SIZE;

	// Perhaps a more useful figure here would be the total count/size/free of
	// *chunks* rather than their pages. Takes more counting though :)

	res.ordblks = g_used_pages;
	res.uordblks = g_used_bytes;
	// Free space in allocated chunk pages. Does not include pages that are on
	// the free-list. To get freelist size (in pages), take hblks - ordblks.
	res.fordblks = res.ordblks * PAGE_SIZE - res.uordblks;

	return res;
}
