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

extern "C" void dump_pages() __attribute__((visibility("default")));

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

#ifndef TEST
#define THREAD_SAFE
#define USE_SPINLOCKS
#endif

#ifdef THREAD_SAFE
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
#endif

class scopelock
{
	scopelock(const scopelock&);
	scopelock& operator=(const scopelock&);
public:
#ifdef THREAD_SAFE
	scopelock()
	{
		mallock::lock();
	}
	~scopelock()
	{
		mallock::unlock();
	}
#endif
};

#ifdef THREAD_SAFE
#define SCOPELOCK(e) scopelock scopelock_ ## e
#elif defined(TEST)
#define SCOPELOCK(e) (void)0
#else
static pthread_t get_owner()
{
	static pthread_t g_owner = 0;
	if (!g_owner)
		g_owner = pthread_self();
	return g_owner;
}
#define SCOPELOCK(e) xassert_abort(pthread_equal(get_owner(), pthread_self()))
#endif

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

	// Index of first free chunk. If chunks_free is non-zero, otherwise invalid.
	// Each chunk contains one byte to link to the next free chunk.
	u8 first_free;
};
#define pageinfo_from_heap(heap_) \
	((pageinfo*)((char*)(heap_) - offsetof(pageinfo, heap)))

static bool page_filled(pageinfo* page)
{
	return !page->chunks_free;
}
static void* page_get_chunk(pageinfo* page)
{
	assert(page->chunks_free);
	page->chunks_free--;
	size_t n = page->first_free;
	assert(n * page->size < PAGE_SIZE);
	u8 *p = (u8*)page->page + n * page->size;
	page->first_free = page->chunks_free ? *p : 0;
	assert(page->first_free < page->chunks);
	return p;
}
static void page_free_chunk(pageinfo* page, void* ptr)
{
	size_t offset_in_page = (u8*)ptr - (u8*)page->page;
	size_t ix = offset_in_page;
	ix /= page->size; // FIXME store inverse or something instead
	assert(ix < page->chunks);
	assert(!(offset_in_page % page->size));
	debug("Freeing %p in %p (size %d)\n", ptr, page->page, page->size);
	*(u8 *)ptr = page->first_free;
	page->first_free = ix;
	assert(page->first_free < page->chunks);
	page->chunks_free++;
}

#define N_SIZES (128/16+4)

typedef NODE freepage_node;
typedef TREE(freepage_node) freepage_heap;

static freepage_heap g_free_pages;
static size_t g_n_free_pages;
static chunkpage_heap g_chunk_pages[N_SIZES];
static uintptr_t g_first_page;
static uintptr_t g_n_pages;
// Assumes mostly contiguous pages...
static pageinfo** g_pages;

static void set_pageinfo(void* page, pageinfo* info);
static pageinfo* get_pageinfo(void* ptr);
static size_t get_magic_page_size(pageinfo* info, void* ptr);

#define MAGIC_PAGE_OTHER ((pageinfo*)0)
#define MAGIC_PAGE_FIRST ((pageinfo*)1)
#define MAGIC_PAGE_FOLLO ((pageinfo*)2)
#define MAGIC_PAGE_FREE ((pageinfo*)3)
#define LAST_MAGIC_PAGE ((pageinfo*)4)
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
		debug("Free-list empty, allocating fresh page\n");
		ret = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	}
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
	set_pageinfo(page, MAGIC_PAGE_FREE);
}

// 4MB is not a lot to keep laying around. But there should rather be some kind
// of hysteresis mechanism.
static const size_t SPARE_PAGES = 1024;

static void free_page(void* page)
{
	if (g_n_free_pages > SPARE_PAGES)
	{
		debug("Free page %p: Have spares - unmapping.\n", page);
		set_pageinfo(page, NULL);
		munmap(page, PAGE_SIZE);
	}
	else
	{
		debug("Free page %p: Not enough spares, caching.\n", page);
		add_to_freelist(page);
	}
	//dump_pages();
}

static void free_magic_page(pageinfo* magic, void* ptr)
{
	debug("Free magic page %p (magic %ld)\n", ptr, (uintptr_t)magic);
	assert(magic == MAGIC_PAGE_FIRST);
	size_t npages = get_magic_page_size(magic, ptr);
	size_t reusepages = 0;
	debug("Free: Page %p (%ld pages)\n", ptr, npages);
	if (g_n_free_pages < SPARE_PAGES)
	{
		reusepages = SPARE_PAGES - g_n_free_pages;
		if (reusepages < npages)
		{
			npages -= reusepages;
		}
		else
		{
			reusepages = npages;
			npages = 0;
		}
	}
	u8* page = (u8*)ptr;
	while (reusepages--)
	{
		IFDEBUG(
		pageinfo* info = get_pageinfo(page);
		assert(IS_MAGIC_PAGE(info) && (info == MAGIC_PAGE_FIRST || info == MAGIC_PAGE_FOLLO));
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
	}
	debug("get_pages: %zu pages: %p\n", n, ret);
	if (unlikely(ret == (void*)-1))
	{
		xprintf("get_pages: %zu pages: %p\n", n, ret);
		return NULL;
	}
	register_magic_pages(ret, n);
	return ret;
}

static void print_pageinfo(pageinfo* page)
{
	printf("info %p: %d x %db, %d free\n", page,
			page->chunks, page->size, page->chunks_free);
}

static size_t page_allocated_space(pageinfo* page)
{
	return (page->chunks - page->chunks_free) * page->size;
}

void dump_pages()
{
	printf("First, dump chunk-pages:\n");
	bool corrupt = false;
	for (size_t i = 0; i < N_SIZES; i++)
	{
		chunkpage_node* min = get_min(g_chunk_pages[i]);
		if (min)
		{
			pageinfo* page = pageinfo_from_heap(min);
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
	size_t unknown_magic = 0;
	size_t chunk_pages = 0;

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
				printf("%p: %ld page(s) large alloc\n", addr, npages);
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
			chunk_pages++;
		}
		else
		{
			size_t n = 1;
			while (pagep < end && !*pagep)
			{
				pagep++;
				n++;
			}
			printf("%p: %zu page(s) hole\n", addr, n);
			addr += (n - 1) *PAGE_SIZE;
			free = true;
		}
		if (!free)
			last_non_free = addr;
		addr += 4096;
	}
	printf(":pmud egaP\n");
	size_t p = allocated ? 10000 * used / allocated : 0;
	printf("Used %zu of %zu (%zu.%02zu%%)\n", used, allocated, p / 100, p % 100);
	printf("Pages: %zd freelist (%zd) %zd large allocs %zd chunkpages %zd unknown\n", freelist_pages, g_n_free_pages, magic_pages, chunk_pages, unknown_magic);
	size_t total_pages = freelist_pages + magic_pages + chunk_pages;
	printf( "last non-free page %p\n"
			"first page         %p\n", last_non_free, (void *)g_first_page);
	printf("%zu bytes in known pages\n", total_pages * PAGE_SIZE);
	printf("%zu bytes covered by page table (%zu entries / %zu bytes)\n", g_n_pages * PAGE_SIZE, g_n_pages, g_n_pages * sizeof(pageinfo*));
	fflush(stdout);
	assert(!corrupt);
	(void)corrupt; // Silence unused-var warning
	assert(freelist_pages == g_n_free_pages);
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
		debug("Moved page table from %p (%zu) %zu to %p (%zu) %zu\n", g_pages, g_n_pages, old_first_page, new_pages, required / sizeof(pageinfo*), g_first_page);

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

static void init_free_list(pageinfo* page)
{
	size_t n = page->chunks;
	u8 *p = (u8 *)page->page;
	size_t size = page->size;
	if (p == (u8 *)page) {
		n--;
		p += size;
	}
	while (n--) {
		page_free_chunk(page, p);
		p += size;
	}
}

static pageinfo* new_chunkpage(size_t size)
{
	size_t ix = size_ix(size);
	assert(ix_size(ix) >= size);
	size = ix_size(ix);

	size_t nchunks = 4096/size;
	pageinfo* ret = NULL;
	size_t pisize = sizeof(pageinfo);
	size_t pisize_ix = size_ix(pisize);
	// If allocating the pageinfo would itself require a new chunk page, *and*
	// we're currently making a new chunk page for the same size class, do
	// something a bit special.
	// The first block of the chunk page will contain its own pageinfo. This
	// pageinfo will never be freed since the page never ends up empty.
	if (!g_chunk_pages[pisize_ix] && pisize_ix == ix)
	{
		ret = (pageinfo*)get_page();
		ret->page = ret;
	}
	else
	{
		ret = (pageinfo*)malloc_unlocked(pisize);
		ret->page = get_page();
	}

	memset(&ret->heap, 0, sizeof(ret->heap));
	ret->size = size;
	ret->index = ix;
	ret->chunks = nchunks;
	// Start out with all chunks allocated, then free them one by one. This
	// is a bit inefficient (we could build the structure all at once rather
	// than execute the free operation many times), but it's obviously correct.
	ret->chunks_free = 0;
	ret->first_free = 0;
	init_free_list(ret);

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
		xassert(ret);
		debug("X ALLOC %p\n", ret);
		return ret;
	}

	chunkpage_heap* pagep = g_chunk_pages + size_ix(size);
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
	if (!mul_safe(n, sz)) {
		errno = ENOMEM;
		return NULL;
	}
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

	debug("X FREE %p\n", ptr);

	pageinfo* page = get_pageinfo(ptr);
	if (unlikely(IS_MAGIC_PAGE(page)))
	{
#ifndef FREE_IS_NOT_FREE
		if (page == MAGIC_PAGE_FIRST)
			free_magic_page(page, ptr);
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

#ifndef FREE_IS_NOT_FREE
	page_free_chunk(page, ptr);

	if (page->chunks_free == 1)
	{
		insert(g_chunk_pages[page->index], &page->heap);
	}
	else if (unlikely(page->chunks_free == page->chunks
		|| (page->page == page && page->chunks_free == page->chunks - 1)))
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
			if (page->page == page) {
				free_page(page);
			} else {
				free_page(page->page);
				page->page = 0;
				free_unlocked(page);
			}
		}
	}
#endif
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
