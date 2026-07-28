/*
 * One heap for the nRF52840 target.
 *
 * ESP-IDF has a single allocator: malloc/calloc and heap_caps_malloc are the
 * same pool, so shared code can call either and esp_get_free_heap_size()
 * describes all of it. A bare newlib port does not work that way. Left alone
 * it has TWO heaps: heap_caps_* goes to FreeRTOS's ucHeap while plain
 * malloc/calloc goes to newlib's sbrk heap, and the two are provisioned
 * separately out of the same 256KB.
 *
 * That split was actively dangerous here, because the stock newlib _sbrk does
 * not bound the heap: it hands out whatever address comes next and only the
 * hardware complains. mesh_task.c allocates its DM session table (44160
 * bytes) and delivery ring (28692 bytes) with plain calloc when there is no
 * PSRAM to put them in, so the sbrk heap ran ~69KB past its 4KB limit,
 * through the unallocated gap, and into the interrupt stack at the top of
 * RAM. It survived only because nothing had yet pushed the total past
 * 0x20040000; adding two kilobytes of mbuf pool did, and the next boot hard
 * faulted in memset with r2 = 0x20040154. The RAM budget gate could not see
 * any of this: it sums sections, and none of those 73KB is a section.
 *
 * So newlib's allocator is routed to pvPortMalloc here. There is now one
 * pool, it is a .bss array the budget gate can count, and an over-allocation
 * returns NULL like it does on ESP instead of quietly scribbling on the
 * stack.
 */
#include <errno.h>
#include <reent.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <FreeRTOS.h>
#include <portable.h>

/*
 * pvPortMalloc guarantees portBYTE_ALIGNMENT (8) alignment. Prefixing the
 * payload with an 8-byte header keeps that guarantee for the caller while
 * recording the size, which realloc needs and heap_4 does not expose.
 */
typedef struct {
    size_t size;
    size_t reserved;
} alloc_hdr_t;

_Static_assert(sizeof(alloc_hdr_t) % 8 == 0, "header must preserve 8-byte alignment");

static void* alloc_with_header(size_t size) {
    if (size > SIZE_MAX - sizeof(alloc_hdr_t)) {
        return NULL;
    }
    alloc_hdr_t* hdr = pvPortMalloc(size + sizeof(alloc_hdr_t));
    if (hdr == NULL) {
        return NULL;
    }
    hdr->size = size;
    return hdr + 1;
}

void* _malloc_r(struct _reent* r, size_t size) {
    void* p = alloc_with_header(size);
    if (p == NULL) {
        r->_errno = ENOMEM;
    }
    return p;
}

void _free_r(struct _reent* r, void* ptr) {
    (void)r;
    if (ptr == NULL) {
        return;
    }
    vPortFree((alloc_hdr_t*)ptr - 1);
}

void* _calloc_r(struct _reent* r, size_t n, size_t size) {
    size_t total = n * size;
    if (size != 0 && total / size != n) {
        r->_errno = ENOMEM;
        return NULL;
    }
    void* p = alloc_with_header(total);
    if (p == NULL) {
        r->_errno = ENOMEM;
        return NULL;
    }
    memset(p, 0, total);
    return p;
}

void* _realloc_r(struct _reent* r, void* ptr, size_t size) {
    if (ptr == NULL) {
        return _malloc_r(r, size);
    }
    if (size == 0) {
        _free_r(r, ptr);
        return NULL;
    }

    alloc_hdr_t* hdr = (alloc_hdr_t*)ptr - 1;
    if (hdr->size >= size) {
        /* heap_4 cannot shrink a block in place, and copying to a smaller
         * one would only fragment the pool. Keep the block as it is. */
        return ptr;
    }

    void* fresh = alloc_with_header(size);
    if (fresh == NULL) {
        r->_errno = ENOMEM;
        return NULL; /* the original block stays valid, as realloc requires */
    }
    memcpy(fresh, ptr, hdr->size);
    vPortFree(hdr);
    return fresh;
}

size_t _malloc_usable_size_r(struct _reent* r, void* ptr) {
    (void)r;
    return ptr ? ((alloc_hdr_t*)ptr - 1)->size : 0;
}

/*
 * Nothing should reach sbrk now that the allocator above owns every
 * allocation, and __HEAP_SIZE is 0 so there is no region for it to hand out
 * anyway. Fail loudly rather than leaving the unbounded newlib version
 * linked, which is what walked off the end of RAM in the first place.
 */
void* _sbrk(ptrdiff_t incr) {
    (void)incr;
    errno = ENOMEM;
    return (void*)-1;
}
