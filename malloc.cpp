#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void init() __attribute__((constructor));
void fini() __attribute__((destructor));

static void panic(...) __attribute__((noreturn));

#define assert xassert
#define xassert(e) if (e); else panic(#e)

/**
 * These are adapted to be stored internally in whatever data we have.
 */
struct pairing_ptr_heap
{
	typedef pairing_ptr_heap T;
	typedef pairing_ptr_heap* Tp;

	/**
	 * down,right: first child and right sibling of this page in pairing heap
	 * of pages with free space.
	 */
	pairing_ptr_heap* down;
	pairing_ptr_heap* right;

	Tp delete_min()
	{
		assert(!right);
		Tp l = down;
		down = NULL;
		return mergePairs(l);
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
pairing_ptr_heap* delete_min(pairing_ptr_heap* p)
{
	assert(p);
	return p->delete_min();
}
pairing_ptr_heap* insert(pairing_ptr_heap* l, pairing_ptr_heap* r)
{
	return pairing_ptr_heap::merge(l, r);
}

struct pageinfo
{
	/**
	 * Pointer to where the page starts.
	 * TODO: Pack some stuff in here. *Twelve* vacant bits!
	 */
	void* page;

	uint16_t size;
	//uint16_t isize;
	uint16_t chunks;
	uint16_t chunks_free;
	uint8_t index; // index into array of pages

	/**
	 * The heap of address-sorted pages in this category that have free pages
	 */
	pairing_ptr_heap heap;
	/**
	 * 1-32 bytes of bitmap data. A set bit means *free* chunk.
	 */
	uint8_t bitmap[];

	// TODO merge bitmap into page?
};
#define pageinfo_from_heap(heap_) \
	((pageinfo*)((char*)(heap_) - offsetof(pageinfo, heap)))

bool page_filled(pageinfo* page)
{
	return !page->chunks_free;
}
void* page_get_chunk(pageinfo* page)
{
	page->chunks_free--;
	uint8_t* bitmap = page->bitmap;
	for (size_t i = 0; i < page->chunks / 8; i++)
	{
		const uint8_t found = bitmap[i];
		if (found)
		{
			uint8_t mask = 0x80;
			size_t n = i << 3;
			while (mask)
			{
				if (mask & found)
				{
					bitmap[i] = found & ~mask;
					return (uint8_t*)page->page + (n * page->size);
				}
				mask >>= 1;
				n++;
			}
		}
	}
	panic("No free chunks found?");
}
void page_free_chunk(pageinfo* page, void* ptr)
{
	size_t ix = (uint8_t*)ptr - (uint8_t*)page->page;
	ix /= page->size; // FIXME store inverse or something instead
	page->bitmap[ix >> 3] |= 1 << (7 - (ix & 7));
	page->chunks_free++;
}

#define N_SIZES (128/16+5)
#define PAGE_SHIFT 12
#define PAGE_SIZE (1 << PAGE_SHIFT)

static pairing_ptr_heap* g_free_pages;
static pageinfo* g_chunk_pages[N_SIZES];
static uintptr_t g_first_page;
static uintptr_t g_n_pages;
// Assumes mostly contiguous pages...
static pageinfo** g_pages;

void init()
{
	g_first_page = (uintptr_t)sbrk(0);
}

void fini()
{
	// TODO Unmap everything?
}

void panic(...)
{
	abort();
}

size_t size_ix(size_t size)
{
	if (size <= 128)
		return (size + 15) / 16 - 1;

	size_t ix = 8; // 256 bytes
	size >>= 8;
	while (size) { size >>= 1; ix++; }
	return ix;
}

size_t ix_size(size_t ix)
{
	return ix < 8 ? 16 * (ix + 1) : (1 << ix);
}

void* get_page()
{
	if (void* ret = g_free_pages)
	{
		g_free_pages = delete_min(g_free_pages);
		return ret;
	}
	else
	{
		uintptr_t cur = (uintptr_t)sbrk(0);
		sbrk((PAGE_SIZE - cur) & (PAGE_SIZE - 1));
		return sbrk(PAGE_SIZE);
	}
}

void set_pageinfo(void* page, pageinfo* info)
{
	uintptr_t offset;
	if (g_pages)
	{
		offset = ((uintptr_t)page - g_first_page) >> PAGE_SHIFT;
	}
	else
	{
		g_first_page = (uintptr_t)page;
		offset = 0;
	}

	if (offset >= g_n_pages)
	{
		size_t required = (offset + PAGE_SIZE) & ~(PAGE_SIZE-1);
		pageinfo** new_pages = (pageinfo**)mmap(NULL, required, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
		assert(new_pages != MAP_FAILED);

		memcpy(new_pages, g_pages, g_n_pages * sizeof(pageinfo*));
		munmap(g_pages, g_n_pages * sizeof(pageinfo*));

		g_pages = new_pages;
		g_n_pages = required / sizeof(pageinfo*);
	}

	g_pages[offset] = info;
}

pageinfo* new_chunkpage(size_t size)
{
	size_t ix = size_ix(size);
	size = ix_size(ix);

	size_t nchunks = 4096/size;
	pageinfo* ret = NULL;
	size_t pisize = sizeof(pageinfo) + nchunks/8 - 1;
	if (!g_chunk_pages[size_ix(pisize)])
	{
		ret = (pageinfo*)get_page();
		set_pageinfo(ret, NULL);
	}
	else
	{
		ret = (pageinfo*)malloc(pisize);
	}

	memset(&ret->heap, 0, sizeof(ret->heap));
	ret->page = get_page();
	ret->size = size;
//	ret->shift = ix_shift(ix);
	ret->chunks = nchunks;
	ret->chunks_free = nchunks;
	ret->index = ix;

	memset(ret->bitmap, 0xff, nchunks/8);

	set_pageinfo(ret->page, ret);

	return ret;
}

pageinfo* ptr_pageinfo(void* ptr)
{
	uintptr_t offset = ((uintptr_t)ptr - g_first_page) >> PAGE_SHIFT;
	if (offset > g_n_pages) return NULL;
	pageinfo* ret = g_pages[offset];
	assert(!ret || !(((uintptr_t)ret->page ^ (uintptr_t)ptr) >> PAGE_SHIFT));
	return ret;
}

size_t get_alloc_size(void* ptr)
{
	pageinfo* info = ptr_pageinfo(ptr);
	if (!info) panic("get_alloc_size for unknown pointer %p", ptr);
	return info->size;
}

void *malloc(size_t size)
{
	if (!size) return NULL;

	pageinfo** pagep = g_chunk_pages + size_ix(size);
	pageinfo* page = *pagep;
	if (!page)
	{
		printf("Adding new chunk page for size %u (cat %d)\n", size, size_ix(size));
		page = new_chunkpage(size);
		*pagep = page;
	}
	printf("Allocating chunk from %p (info %p, %d left)\n", page->page, page, page->chunks_free);
	void* ret = page_get_chunk(page);
	if (page_filled(page))
	{
		printf("Page %p (info %p) filled\n", page->page, page);
		pairing_ptr_heap* newpage = delete_min(&page->heap);
		*pagep = newpage ? pageinfo_from_heap(newpage) : NULL;
	}
	return ret;
}

void* calloc(size_t n, size_t sz)
{
	size_t size = n * sz;
	void* ptr = malloc(size);
	if (ptr) memset(ptr, 0, size);
	return ptr;
}

void* realloc(void* ptr, size_t new_size)
{
	size_t old_size = get_alloc_size(ptr);
	void* ret = malloc(new_size);
	if (ret)
	{
		memcpy(ret, ptr, new_size < old_size ? new_size : old_size);
	}
	return ret;
}

void free(void *ptr)
{
	if (!ptr) return;

	pageinfo* page = ptr_pageinfo(ptr);
	if (!page) panic("free on unknown pointer %p", ptr);

	bool was_filled = page_filled(page);
	page_free_chunk(page, ptr);
	if (was_filled)
	{
		pageinfo** pagep = g_chunk_pages + page->index;
		pageinfo* free_page = *pagep;
		if (!free_page)
			*pagep = page;
		else
			*pagep = pageinfo_from_heap(insert(&free_page->heap, &page->heap));
	}
	else if (page->chunks_free == page->chunks)
	{
		printf("Free: page %p (info %p) is now free\n", page->page, page);
	}
}

#ifdef TEST

int32_t xrand()
{
	static int32_t m_w = 1246987127, m_z = 789456123;
	m_z = 36969 * (m_z & 65535) + (m_z >> 16);
	m_w = 18000 * (m_w & 65535) + (m_w >> 16);
	return (m_z << 16) + m_w;  /* 32-bit result */
}

const size_t DELAY = 10;
const size_t NTESTS = 100000;
const size_t MAXALLOC = 512;

int main()
{
	void* ptrs[DELAY] = {0};
	for (size_t i = 0; i < NTESTS; i++)
	{
		size_t size = i % MAXALLOC;
		void** p = ptrs + (i % DELAY);
		printf("free(%p)\n", *p);
		free(*p);
		printf("malloc(%lu)\n", (unsigned long)size);
		*p = malloc(size);
		printf("malloc(%lu): %p\n", (unsigned long)size, *p);
		for (size_t j = 0; j < DELAY; j++)
		{
			assert(ptrs + j == p || !p[0] || ptrs[j] != p[0]);
		}
	}
}
#endif
