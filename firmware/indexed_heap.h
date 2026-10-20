#ifndef INDEXED_HEAP_H
#define INDEXED_HEAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *indexed_heap_alloc(uint32_t size);
uint32_t indexed_heap_word(uint32_t address);
void indexed_heap_lock(void);
void indexed_heap_unlock(void);

#ifdef __cplusplus
}
#endif

#endif
