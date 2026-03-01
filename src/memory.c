#include "memory.h"

#define MEMORY_ALIGNMENT 16

typedef unsigned char u8;
typedef size_t usize;
typedef ptrdiff_t isize;

#define ALIGN_UP(size, alignment) ((usize)(size + alignment - 1) & (~(usize)alignment + 1))

static inline isize isize_min(isize left, isize right) {
    return left < right ? left : right;
}

static inline isize isize_max(isize left, isize right) {
    return left > right ? left : right;
}

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// It is assumed that this function returns addresses aligned to "MEMORY_ALIGNMENT" bytes.
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

typedef struct Block Block;
typedef struct Region Region;

#define REGION_HEADER_SIZE (isize)ALIGN_UP(sizeof(Region), MEMORY_ALIGNMENT)
#define REGION_MIN_SIZE ((isize)256 * 1024)

struct Region {
    isize size;
    Region *previous;
    Region *next;
};

typedef Region *RegionList;

static inline Block *region_first_block(Region const *region) {
    return (Block *)((u8 *)region + REGION_HEADER_SIZE);
}

static void region_list_prepend(RegionList *list, Region *region) {
    region->previous = NULL;
    region->next = *list;

    if (*list != NULL) {
        (*list)->previous = region;
    }
    *list = region;
}

#define BLOCK_HEADER_SIZE (isize)ALIGN_UP(sizeof(Block), MEMORY_ALIGNMENT)

struct Block {
    isize size;
    bool last_in_region;

    bool is_free;
    Block *previous_free;
    Block *next_free;
};

static inline void *block_memory(Block const *block) {
    return (u8 *)block + BLOCK_HEADER_SIZE;
}

static inline Block *block_next(Block const *block) {
    if (!block->last_in_region) {
        return (Block *)((u8 *)block + BLOCK_HEADER_SIZE + block->size);
    } else {
        return NULL;
    }
}

static inline Block *memory_to_block(void const *memory) {
    return (Block *)((u8 *)memory - BLOCK_HEADER_SIZE);
}

typedef Block *BlockFreeList;

static void block_free_list_add(BlockFreeList *list, Block *block) {
    block->is_free = true;

    Block *previous_block = NULL;
    Block *next_block = *list;
    while (next_block != NULL && block->size > next_block->size) {
        previous_block = next_block;
        next_block = next_block->next_free;
    }

    if (previous_block == NULL) {
        *list = block;
    }
    block->previous_free = previous_block;
    block->next_free = next_block;

    if (previous_block != NULL) {
        previous_block->next_free = block;
    }
    if (next_block != NULL) {
        next_block->previous_free = block;
    }
}

static void block_free_list_remove(BlockFreeList *list, Block *block) {
    block->is_free = false;

    if (*list == block) {
        *list = (*list)->next_free;
    }

    if (block->previous_free != NULL) {
        (block->previous_free)->next_free = block->next_free;
    }
    if (block->next_free != NULL) {
        (block->next_free)->previous_free = block->previous_free;
    }
}

static Block *block_free_list_get(BlockFreeList *list, isize min_size) {
    Block *block_iter = *list;
    while (block_iter != NULL && block_iter->size < min_size) {
        block_iter = block_iter->next_free;
    }

    if (block_iter != NULL) {
        block_free_list_remove(list, block_iter);
        return block_iter;
    } else {
        return NULL;
    }
}

struct HeapAllocator {
    RegionList regions;
    BlockFreeList free_blocks;
};

HeapAllocator *heap_allocator_create(void) {
    HeapAllocator *allocator = system_allocate(sizeof(HeapAllocator));
    if (allocator == NULL) {
        return NULL;
    }

    allocator->regions = NULL;
    allocator->free_blocks = NULL;

    return allocator;
}

void heap_allocator_destroy(HeapAllocator *allocator) {
    Region *region_iter = allocator->regions;
    while (region_iter != NULL) {
        Region *region = region_iter;
        region_iter = region_iter->next;

        system_deallocate(region);
    }

    system_deallocate(allocator);
}

void *heap_allocate(HeapAllocator *allocator, isize size) {
    size = ALIGN_UP(size, MEMORY_ALIGNMENT);
    if (size == 0) {
        return NULL;
    }

    Block *free_block = block_free_list_get(&allocator->free_blocks, size);
    if (free_block != NULL) {
        return block_memory(free_block);
    }

    isize new_region_size = isize_max(size + BLOCK_HEADER_SIZE, REGION_MIN_SIZE);
    Region *new_region = system_allocate(REGION_HEADER_SIZE + new_region_size);
    if (new_region == NULL) {
        return NULL;
    }

    new_region->size = new_region_size;
    region_list_prepend(&allocator->regions, new_region);

    Block *new_block = region_first_block(new_region);
    new_block->size = new_region_size - BLOCK_HEADER_SIZE;
    new_block->last_in_region = true;
    new_block->is_free = false;

    return block_memory(new_block);
}

void heap_deallocate(HeapAllocator *allocator, void *memory) {
    if (memory == NULL) {
        return;
    }

    Block *block = memory_to_block(memory);
    block_free_list_add(&allocator->free_blocks, block);
}

void *heap_reallocate(HeapAllocator *allocator, void *memory, isize new_size) {
    new_size = ALIGN_UP(new_size, MEMORY_ALIGNMENT);
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
    Block *next_block = NULL;

    if (iterator->region == NULL) {
        Region *first_region = allocator->regions;

        if (first_region != NULL) {
            next_block = region_first_block(first_region);
            iterator->region = first_region;
        }
    } else {
        Block *current_block = memory_to_block(iterator->memory);
        next_block = block_next(current_block);

        if (next_block == NULL) {
            Region *region = iterator->region;
            iterator->region = region->next;
            if (region->next != NULL) {
                next_block = region_first_block(region->next);
            }
        }
    }

    if (next_block != NULL) {
        iterator->memory = block_memory(next_block);
        iterator->size = next_block->size;
        iterator->is_free = next_block->is_free;
    } else {
        iterator->memory = NULL;
        iterator->size = 0;
    }
}
