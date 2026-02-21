#include "memory.h"

typedef unsigned char u8;
typedef ptrdiff_t isize;

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void *system_allocate(isize size) {
    HANDLE heap = GetProcessHeap();
    return HeapAlloc(heap, 0, size);
}

void system_deallocate(void *memory) {
    HANDLE heap = GetProcessHeap();
    HeapFree(heap, 0, memory);
}

#endif // _WIN32

void memory_set(void *memory, isize size, u8 filler) {
    u8 *memory_bytes = memory;

    u8 *memory_iter = memory_bytes;
    while (memory_iter < memory_bytes + size) {
        *memory_iter = filler;
        memory_iter += 1;
    }
}

void memory_copy(void const *source, isize size, void *dest) {
    u8 const *source_bytes = source;
    u8 *dest_bytes = dest;

    u8 const *source_iter = source_bytes;
    u8 *dest_iter = dest_bytes;
    while (source_iter < source_bytes + size) {
        *dest_iter = *source_iter;
        source_iter += 1;
        dest_iter += 1;
    }
}

void memory_move(void const *source, isize size, void *dest) {
    u8 const *source_bytes = source;
    u8 *dest_bytes = dest;

    if (source_bytes < dest_bytes) {
        u8 const *source_iter = source_bytes + size - 1;
        u8 *dest_iter = dest_bytes + size - 1;
        while (source_iter >= source_bytes) {
            *dest_iter = *source_iter;
            source_iter -= 1;
            dest_iter -= 1;
        }
    } else {
        u8 const *source_iter = source_bytes;
        u8 *dest_iter = dest_bytes;
        while (source_iter < source_bytes + size) {
            *dest_iter = *source_iter;
            source_iter += 1;
            dest_iter += 1;
        }
    }
}

struct HeapAllocator {
    void *dummy_data;
};

HeapAllocator *heap_allocator_create(void) {
    HeapAllocator *allocator = system_allocate(sizeof(HeapAllocator));
    if (allocator == NULL) {
        return NULL;
    }

    allocator->dummy_data = NULL;
    return allocator;
}

void heap_allocator_destroy(HeapAllocator *allocator) {
    system_deallocate(allocator);
}

void *heap_allocate(HeapAllocator *allocator, isize size) {
    if (size == 0) {
        return NULL;
    }

    return system_allocate(size);
}

void heap_deallocate(HeapAllocator *allocator, void *memory) {
    if (memory == NULL) {
        return;
    }

    system_deallocate(memory);
}

void *heap_reallocate(HeapAllocator *allocator, void *memory, isize old_size, isize new_size) {
    if (new_size == 0) {
        heap_deallocate(allocator, memory);
        return NULL;
    }

    void *new_memory = heap_allocate(allocator, new_size);
    if (new_memory == NULL) {
        return NULL;
    }

    if (memory != NULL) {
        memory_copy(memory, old_size, new_memory);
        heap_deallocate(allocator, memory);
    }

    return new_memory;
}
