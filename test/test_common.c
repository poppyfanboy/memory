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

// *Really* minimal PCG32 code / (c) 2014 M.E. O'Neill / pcg-random.org
// Licensed under Apache License 2.0 (NO WARRANTY, etc. see website)

typedef struct {
    uint64_t state;
} PCG32;

void pcg32_initialize(PCG32 *rng, uint64_t seed) {
    rng->state = seed != 0 ? seed : 1;
}

uint32_t pcg32_random(PCG32 *rng) {
    uint64_t oldstate = rng->state;
    // Advance internal state
    rng->state = oldstate * 6364136223846793005ULL + 1;
    // Calculate output function (XSH RR), uses old state for max ILP
    uint32_t xorshifted = ((oldstate >> 18) ^ oldstate) >> 27;
    uint32_t rot = oldstate >> 59;
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

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
