#include "indexed_heap.h"
#include "frame_stream.h"
#include <stddef.h>

void *indexed_heap_alloc(uint32_t size)
{
    uint32_t needed;
    uint32_t current;
    uint32_t node;
    uint32_t count = 0;
    uint32_t fits = 0;
    void *result = NULL;
    if (size > 0x7FFFFFF4u)
        return NULL;
    needed = ((size + 3u) & ~3u) + 8u;
    if (needed < 12u)
        needed = 12u;
    indexed_heap_lock();
    current = indexed_heap_word(0x20000324u);
    if (current < 0x2000F100u || current > 0x2001FB00u || (current & 3u))
        goto done;
    node = indexed_heap_word(0x2000F0CCu);
    while (node) {
        uint32_t length;
        uint32_t next;
        if (++count > 5674u || (node & 3u) || node < 0x2000F100u || node > current - 8u)
            goto done;
        length = indexed_heap_word(node);
        next = indexed_heap_word(node + 4u);
        if (length < 12u || (length & 3u) || length > current - node)
            goto done;
        if (next && (next <= node || next < node + length))
            goto done;
        if (length >= needed)
            fits = 1;
        node = next;
    }
    if (fits || needed <= 0x2001FB00u - current)
        result = fs_alloc(size);
done:
    indexed_heap_unlock();
    return result;
}
