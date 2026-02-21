#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>

void memory_set(void *memory, ptrdiff_t size, unsigned char filler);
void memory_copy(void const *source, ptrdiff_t size, void *dest);
void memory_move(void const *source, ptrdiff_t size, void *dest);

typedef struct HeapAllocator HeapAllocator;

HeapAllocator *heap_allocator_create(void);
void heap_allocator_destroy(HeapAllocator *allocator);

void *heap_allocate(HeapAllocator *allocator, ptrdiff_t size);
void heap_deallocate(HeapAllocator *allocator, void *memory);

void *heap_reallocate(
    HeapAllocator *allocator,
    void *memory,
    ptrdiff_t old_size,
    ptrdiff_t new_size
);

#endif
