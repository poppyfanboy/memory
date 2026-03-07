#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void *system_allocate(ptrdiff_t size) {
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void system_deallocate(void *memory) {
    VirtualFree(memory, 0, MEM_RELEASE);
}

#endif // _WIN32

#include "../src/memory.h"

void heap_dump(HeapAllocator const *allocator) {
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
}

