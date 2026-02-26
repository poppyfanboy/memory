#include <stdio.h>

#include "../src/memory.h"

int main(void) {
    HeapAllocator *allocator = heap_allocator_create();

    void *a1 = heap_allocate(allocator, 32);
    void *a2 = heap_allocate(allocator, 64);
    void *a3 = heap_allocate(allocator, 128);
    void *a4 = heap_allocate(allocator, 256);
    void *a5 = heap_allocate(allocator, 512);
    void *a6 = heap_allocate(allocator, 1024);

    heap_deallocate(allocator, a1);
    heap_deallocate(allocator, a3);
    heap_deallocate(allocator, a6);

    HeapIterator iterator = {0};
    heap_iterate(allocator, &iterator);

    while (iterator.block_memory != NULL) {
        printf("Block of size %td at address 0x%p\n", iterator.block_size, iterator.block_memory);

        heap_iterate(allocator, &iterator);
    }

    return 0;
}
