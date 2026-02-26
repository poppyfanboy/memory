#include "memory.h"

typedef unsigned char u8;
typedef size_t usize;
typedef ptrdiff_t isize;

#define ALIGNMENT 16
#define ALIGN_UP(x) ((usize)(x + ALIGNMENT - 1) & (~(usize)ALIGNMENT + 1))

static inline isize isize_min(isize left, isize right) {
    return left < right ? left : right;
}

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// It is assumed that this function returns addresses aligned to "ALIGNMENT" bytes.
void *system_allocate(isize size) {
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void system_deallocate(void *memory) {
    VirtualFree(memory, 0, MEM_RELEASE);
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

#define BLOCK_HEADER_SIZE (isize)ALIGN_UP(sizeof(Block))

typedef struct Block Block;

struct Block {
    isize size;
    Block *previous;
    Block *next;
};

static inline void *block_memory(Block *block) {
    return (u8 *)block + BLOCK_HEADER_SIZE;
}

static inline Block *memory_to_block(void *memory) {
    return (Block *)((u8 *)memory - BLOCK_HEADER_SIZE);
}

struct HeapAllocator {
    Block *blocks;
};

HeapAllocator *heap_allocator_create(void) {
    HeapAllocator *allocator = system_allocate(sizeof(HeapAllocator));
    if (allocator == NULL) {
        return NULL;
    }

    allocator->blocks = NULL;

    return allocator;
}

void heap_allocator_destroy(HeapAllocator *allocator) {
    Block *block_iter = allocator->blocks;
    while (block_iter != NULL) {
        Block *block = block_iter;
        block_iter = block_iter->next;

        system_deallocate(block);
    }

    system_deallocate(allocator);
}

void *heap_allocate(HeapAllocator *allocator, isize size) {
    size = ALIGN_UP(size);
    if (size == 0) {
        return NULL;
    }

    Block *new_block = system_allocate(size + BLOCK_HEADER_SIZE);
    if (new_block == NULL) {
        return NULL;
    }

    new_block->size = size;

    new_block->previous = NULL;
    new_block->next = allocator->blocks;

    if (allocator->blocks != NULL) {
        (allocator->blocks)->previous = new_block;
    }
    allocator->blocks = new_block;

    return block_memory(new_block);
}

void heap_deallocate(HeapAllocator *allocator, void *memory) {
    if (memory == NULL) {
        return;
    }

    Block *block = memory_to_block(memory);

    if (allocator->blocks == block) {
        allocator->blocks = (allocator->blocks)->next;
    }

    if (block->previous != NULL) {
        (block->previous)->next = block->next;
    }
    if (block->next != NULL) {
        (block->next)->previous = block->previous;
    }

    system_deallocate(block);
}

void *heap_reallocate(HeapAllocator *allocator, void *memory, isize new_size) {
    new_size = ALIGN_UP(new_size);
    if (new_size == 0) {
        heap_deallocate(allocator, memory);
        return NULL;
    }

    void *new_memory = heap_allocate(allocator, new_size);
    if (new_memory == NULL) {
        return NULL;
    }

    if (memory != NULL) {
        Block *block = memory_to_block(memory);
        memory_copy(memory, isize_min(block->size, new_size), new_memory);

        heap_deallocate(allocator, memory);
    }

    return new_memory;
}

void heap_iterate(HeapAllocator *allocator, HeapIterator *iterator) {
    Block *current_block;
    if (iterator->block_memory == NULL) {
        current_block = allocator->blocks;
    } else {
        current_block = memory_to_block(iterator->block_memory)->next;
    }

    if (current_block == NULL) {
        iterator->block_memory = NULL;
        iterator->block_size = 0;
    } else {
        iterator->block_memory = block_memory(current_block);
        iterator->block_size = current_block->size;
    }
}
