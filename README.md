sbmalloc - a simple malloc
==========================

I wrote this mostly to see how it could be done. See "design" and
"implementation" sections below for some thoughts on how it works.

## License ##

This software is licensed under the MIT license. See the LICENSE file.

## Building ##

Running 'make' should be enough. This works on Linux, there is no reason to
suspect it would work on other operating systems, but feel free to try. GNU
make is probably required.

## Design ##

The goals for this malloc are simplicity and low memory use, together with at
least acceptable performance. As much memory "as possible" should be returned
to the system "eventually", and the external memory use (e.g. RSS and VSIZE)
should not be much more than the actual memory usage for very long.

Those are all very vague terms, but among other things it means that large
internal holes in the heap should be returned to the OS if unused, but at the
same time malloc should not need to return all unused memory immediately since
that will cause unnecessary calls to mmap and munmap.

I got some of the ideas from phkmalloc.

## Implementation details ##

The basic principle is segregated storage, with bins for 16..256 bytes in
16 byte increments and 256..2048 bytes in powers of two. Larger allocations,
`PAGE_SIZE` and up will go straight to mmap.

For each class, one page is allocated and divided into N-byte chunks, with
metadata for the page allocated separately. Metadata includes the number of
free/allocated chunks, the size of each chunk, and a bitmap of used chunks.

Each size class has its own list of non-full pages, sorted by the address of
the page. malloc picks the first free chunk from the lowest-address page on the
free-list.

Completely empty pages get added to a free-page list when freeing the last
chunk on the page, and may then be reused for another size class. The free-page
list is also sorted by address. When running out of pages for any size class a
new page is taken from the free-page list or allocated from the system.

The free-page list is allowed to grow to a fixed size (1024 pages, at this
time), if that number of free pages already exists in the free list additional
pages are freed rather than added to the free-list. There are plans to replace
this with a better policy, since it causes excessive unmap traffic.

## Multi-threading ##

There is currently no attempt at all to support per-thread pools or any clever
threading optimizations like that. This malloc is, however, thread safe and
protects all public functions with a pthread mutex.
