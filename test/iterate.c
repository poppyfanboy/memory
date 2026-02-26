#include <stdio.h>

#include "../src/memory.h"

int main(void) {
    HeapAllocator *allocator = heap_allocator_create();

    void *a6 = heap_allocate(allocator, 1024);
    heap_deallocate(allocator, a6);

    void *a5 = heap_allocate(allocator, 512);

    void *a4 = heap_allocate(allocator, 256);

    void *a3 = heap_allocate(allocator, 128);
    heap_deallocate(allocator, a3);

    void *a2 = heap_allocate(allocator, 64);

    void *a1 = heap_allocate(allocator, 32);
    heap_deallocate(allocator, a1);

    HeapIterator iterator = {0};
    heap_iterate(allocator, &iterator);

    while (iterator.memory != NULL) {
        printf(
            "%s block of size %td at address 0x%p\n",
            iterator.is_free ? "Free" : "Occupied",
            iterator.size,
            iterator.memory
        );

        heap_iterate(allocator, &iterator);
    }

    return 0;
}
