#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdbool.h>

#define SYSTEM_ALLOCATE_IS_CONTIGUOUS 0x1
#define SYSTEM_ALLOCATE_ZEROES_MEMORY 0x2

typedef struct HeapAllocator HeapAllocator;

typedef void *(*SystemAllocate)(void *user_context, ptrdiff_t size);
typedef void (*SystemDeallocate)(void *user_context, void *memory);

HeapAllocator *heap_allocator_create(
    void *user_context,
    SystemAllocate system_allocate,
    SystemDeallocate system_deallocate,
    unsigned int flags
);

void *heap_allocate(HeapAllocator *allocator, ptrdiff_t size);
void *heap_allocate_zeroed(HeapAllocator *allocator, ptrdiff_t size);
void *heap_reallocate(HeapAllocator *allocator, void *memory, ptrdiff_t new_size);
void heap_deallocate(HeapAllocator *allocator, void *memory);

typedef struct {
    void *region;
    void *memory;
    ptrdiff_t size;
    bool is_free;
} HeapIterator;

void heap_iterate(HeapAllocator const *allocator, HeapIterator *iterator);

void heap_allocator_destroy(HeapAllocator *allocator);

#endif
