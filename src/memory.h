#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdbool.h>

void memory_set(void *memory, ptrdiff_t size, unsigned char filler);
void memory_copy(void const *source, ptrdiff_t size, void *dest);
void memory_move(void const *source, ptrdiff_t size, void *dest);

typedef struct HeapAllocator HeapAllocator;

typedef void *(*SystemAllocate)(ptrdiff_t size);
typedef void (*SystemDeallocate)(void *memory);
HeapAllocator *heap_allocator_create(SystemAllocate allocate, SystemDeallocate deallocate);

typedef void *(*SystemHeapGrow)(ptrdiff_t increment);
HeapAllocator *heap_allocator_from_system_heap(SystemHeapGrow heap_grow);

void *heap_allocate(HeapAllocator *allocator, ptrdiff_t size);
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
