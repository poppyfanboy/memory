#include <stdint.h>
#include <stdio.h>

typedef unsigned char u8;
typedef ptrdiff_t isize;
typedef size_t usize;

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void *system_allocate(void *user_context, isize size) {
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void system_deallocate(void *user_context, void *memory) {
    VirtualFree(memory, 0, MEM_RELEASE);
}

#define SYSTEM_HEAP_CAPACITY ((isize)64 * 1024 * 1024 * 1024)

typedef struct {
    usize page_size;

    void *base;
    isize allocated;
    isize committed;
    isize reserved;
} SystemHeap;

void *system_heap_grow(void *user_context, isize increment) {
    SystemHeap *system_heap = user_context;

    if (system_heap->base == NULL) {
        void *heap_base = VirtualAlloc(NULL, SYSTEM_HEAP_CAPACITY, MEM_RESERVE, PAGE_READWRITE);
        if (heap_base == NULL) {
            return NULL;
        }

        system_heap->base = heap_base;
        system_heap->allocated = 0;
        system_heap->committed = 0;
        system_heap->reserved = SYSTEM_HEAP_CAPACITY;

        SYSTEM_INFO system_info;
        GetSystemInfo(&system_info);
        system_heap->page_size = system_info.dwPageSize;
    }

    if (increment == 0) {
        return system_heap->base;
    }

    if (system_heap->allocated + increment > system_heap->reserved) {
        return NULL;
    }

    isize bytes_to_commit = (system_heap->allocated + increment) - system_heap->committed;
    if (bytes_to_commit > 0) {
        bytes_to_commit = (bytes_to_commit + system_heap->page_size - 1) / system_heap->page_size * system_heap->page_size;

        void *commit_result = VirtualAlloc(
            (u8 *)system_heap->base + system_heap->committed,
            bytes_to_commit,
            MEM_COMMIT,
            PAGE_READWRITE
        );
        if (commit_result == NULL) {
            return NULL;
        }

        system_heap->committed += bytes_to_commit;
    }

    void *memory = (u8 *)system_heap->base + system_heap->allocated;
    system_heap->allocated += increment;

    return memory;
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

HeapAllocator *heap_allocator(void) {
    #ifdef USE_SYSTEM_HEAP
    SystemHeap *system_heap = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(SystemHeap));
    return heap_allocator_create(
        system_heap, system_heap_grow, NULL,
        SYSTEM_ALLOCATE_IS_CONTIGUOUS | SYSTEM_ALLOCATE_HAS_BYTE_GRANULARITY
    );
    #else
    return heap_allocator_create(
        NULL, system_allocate, system_deallocate,
        0
    );
    #endif
}

void heap_dump(HeapAllocator const *allocator) {
    HeapIterator iterator = {0};
    void *previous_region = NULL;

    while (true) {
        heap_iterate(allocator, &iterator);
        if (iterator.memory == NULL) {
            break;
        }

        if (previous_region != iterator.region) {
            printf("# Region\n");
        }

        if (iterator.memory == allocator) {
            printf(
                "[0x%p] %9td bytes: Allocator metadata\n",
                iterator.memory,
                iterator.size
            );
        } else {
            printf(
                "[0x%p] %9td bytes: %s block\n",
                iterator.memory,
                iterator.size,
                iterator.is_free ? "Free" : "Occupied"
            );
        }

        previous_region = iterator.region;
    }
}
