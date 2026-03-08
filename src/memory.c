#include "memory.h"

typedef unsigned char u8;
typedef size_t usize;
typedef ptrdiff_t isize;

// This must be at least 8, because 3 lower bits of the block sizes are used for storing bit flags.
#define ALIGNMENT 16

#define ALIGN_UP(value, alignment) (((usize)(value) + (alignment) - 1) & (~(usize)(alignment) + 1))

static inline isize isize_min(isize left, isize right) {
    return left < right ? left : right;
}

static inline isize isize_max(isize left, isize right) {
    return left > right ? left : right;
}

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

#define REGION_HEADER_SIZE (isize)ALIGN_UP(sizeof(Region), ALIGNMENT)
#define REGION_MIN_SIZE ((isize)256 * 1024)

struct Region {
    // Size of the memory available inside of the region (excluding the header size).
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

struct Block {
    // Size of the memory available inside of the block (excluding the header size).
    // Lower 3 bits are used for storing flags.
    usize size;
};

#define BLOCK_HEADER_SIZE (isize)ALIGN_UP(sizeof(Block), ALIGNMENT)

// This must be at least (3 * sizeof(usize)), because free blocks store additional information:
// - Two pointers for the intrusive free-lists right after the header.
// - Size of the block at the very end of the block.
#define BLOCK_MIN_SIZE (isize)32

// Since block sizes are always multiples of "ALIGNMENT", lower bits of block sizes are naturally
// zero and thus can be used to store additional information.
#define BLOCK_IS_FREE_BIT (usize)0x1
#define BLOCK_PREVIOUS_IS_FREE_BIT (usize)0x2
#define BLOCK_LAST_IN_REGION_BIT (usize)0x4
#define BLOCK_FLAG_BITS (BLOCK_IS_FREE_BIT | BLOCK_PREVIOUS_IS_FREE_BIT | BLOCK_LAST_IN_REGION_BIT)

#define BLOCK_SIZE(block) (isize)((block)->size & ~BLOCK_FLAG_BITS)
#define BLOCK_IS_FREE(block) (((block)->size & BLOCK_IS_FREE_BIT) != 0)
#define BLOCK_PREVIOUS_IS_FREE(block) (((block)->size & BLOCK_PREVIOUS_IS_FREE_BIT) != 0)
#define BLOCK_LAST_IN_REGION(block) (((block)->size & BLOCK_LAST_IN_REGION_BIT) != 0)

#define BLOCK_MEMORY(block) (void *)((u8 *)(block) + BLOCK_HEADER_SIZE)
#define BLOCK_FROM_MEMORY(memory) ((Block *)((u8 *)(memory) - BLOCK_HEADER_SIZE))

// Accessing the previous block (or its size) is allowed only when previous block is marked as free.
// When the previous block is free, it stores its size right before the current block's header.
#define BLOCK_PREVIOUS_SIZE(block) (isize *)((u8 *)(block) - sizeof(isize))
#define BLOCK_PREVIOUS(block) \
    ((Block *)((u8 *)(block) - BLOCK_HEADER_SIZE - *BLOCK_PREVIOUS_SIZE(block)))

// Always check for BLOCK_LAST_IN_REGION() before accessing this.
#define BLOCK_NEXT(block) ((Block *)((u8 *)(block) + BLOCK_HEADER_SIZE + BLOCK_SIZE(block)))

#define BLOCK_PREVIOUS_FREE(block) ((Block **)((u8 *)block + BLOCK_HEADER_SIZE))
#define BLOCK_NEXT_FREE(block) ((Block **)((u8 *)block + BLOCK_HEADER_SIZE + sizeof(Block *)))

static Block *block_split(Block *block, isize new_size) {
    isize min_size_to_split = new_size + (BLOCK_HEADER_SIZE + BLOCK_MIN_SIZE);
    if (BLOCK_SIZE(block) < min_size_to_split) {
        return NULL;
    }
    isize remainder_block_size = BLOCK_SIZE(block) - (BLOCK_HEADER_SIZE + new_size);

    Block *remainder_block = (Block *)((u8 *)block + (BLOCK_HEADER_SIZE + new_size));
    remainder_block->size = remainder_block_size | BLOCK_IS_FREE_BIT;
    remainder_block->size |= (block->size & BLOCK_PREVIOUS_IS_FREE_BIT);
    remainder_block->size |= (block->size & BLOCK_LAST_IN_REGION_BIT);
    if (BLOCK_IS_FREE(block)) {
        *BLOCK_PREVIOUS_SIZE(remainder_block) = BLOCK_SIZE(block);
    }

    usize block_flags = (block->size & BLOCK_FLAG_BITS) & ~BLOCK_LAST_IN_REGION_BIT;
    block->size = new_size | block_flags;
    if (!BLOCK_LAST_IN_REGION(remainder_block)) {
        *BLOCK_PREVIOUS_SIZE(BLOCK_NEXT(remainder_block)) = BLOCK_SIZE(remainder_block);
    }

    return remainder_block;
}

typedef Block *BlockFreeList;

static void block_free_list_add(BlockFreeList *list, Block *block) {
    Block *previous_block = NULL;
    Block *next_block = *list;
    while (next_block != NULL && BLOCK_SIZE(block) > BLOCK_SIZE(next_block)) {
        previous_block = next_block;
        next_block = *BLOCK_NEXT_FREE(next_block);
    }

    if (previous_block == NULL) {
        *list = block;
    }
    *BLOCK_PREVIOUS_FREE(block) = previous_block;
    *BLOCK_NEXT_FREE(block) = next_block;

    if (previous_block != NULL) {
        *BLOCK_NEXT_FREE(previous_block) = block;
    }
    if (next_block != NULL) {
        *BLOCK_PREVIOUS_FREE(next_block) = block;
    }
}

static void block_free_list_remove(BlockFreeList *list, Block *block) {
    if (*list == block) {
        *list = *BLOCK_NEXT_FREE(*list);
    }

    Block *previous_free = *BLOCK_PREVIOUS_FREE(block);
    if (previous_free != NULL) {
        *BLOCK_NEXT_FREE(previous_free) = *BLOCK_NEXT_FREE(block);
    }

    Block *next_free = *BLOCK_NEXT_FREE(block);
    if (next_free != NULL) {
        *BLOCK_PREVIOUS_FREE(next_free) = *BLOCK_PREVIOUS_FREE(block);
    }
}

static Block *block_free_list_get(BlockFreeList *list, isize min_size) {
    Block *block_iter = *list;
    while (block_iter != NULL && BLOCK_SIZE(block_iter) < min_size) {
        block_iter = *BLOCK_NEXT_FREE(block_iter);
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

    SystemAllocate system_allocate;
    SystemDeallocate system_deallocate;
    SystemHeapGrow system_heap_grow;
};

HeapAllocator *heap_allocator_create(
    SystemAllocate system_allocate,
    SystemDeallocate system_deallocate
) {
    HeapAllocator *allocator = system_allocate(sizeof(HeapAllocator));
    if (allocator == NULL) {
        return NULL;
    }

    allocator->regions = NULL;
    allocator->free_blocks = NULL;

    allocator->system_allocate = system_allocate;
    allocator->system_deallocate = system_deallocate;
    allocator->system_heap_grow = NULL;

    return allocator;
}

HeapAllocator *heap_allocator_from_system_heap(SystemHeapGrow system_heap_grow) {
    // Align the system heap base, so that all subsequent allocations are properly aligned.
    void *system_heap_base = system_heap_grow(0);
    if (system_heap_base == NULL) {
        return NULL;
    }
    usize heap_base_alignment = ALIGN_UP(system_heap_base, ALIGNMENT) - (usize)system_heap_base;
    system_heap_grow(heap_base_alignment);

    HeapAllocator *allocator = system_heap_grow(ALIGN_UP(sizeof(HeapAllocator), ALIGNMENT));
    if (allocator == NULL) {
        return NULL;
    }

    allocator->regions = NULL;
    allocator->free_blocks = NULL;

    allocator->system_allocate = NULL;
    allocator->system_deallocate = NULL;
    allocator->system_heap_grow = system_heap_grow;

    return allocator;
}

void heap_allocator_destroy(HeapAllocator *allocator) {
    if (allocator->system_deallocate != NULL) {
        Region *region_iter = allocator->regions;
        while (region_iter != NULL) {
            Region *region = region_iter;
            region_iter = region_iter->next;

            allocator->system_deallocate(region);
        }

        allocator->system_deallocate(allocator);
    }
}

void *heap_allocate(HeapAllocator *allocator, isize size) {
    if (size == 0) {
        return NULL;
    }

    size = isize_max(size, BLOCK_MIN_SIZE);
    size = ALIGN_UP(size, ALIGNMENT);

    Block *free_block = block_free_list_get(&allocator->free_blocks, size);
    if (free_block != NULL) {
        Block *remainder_block = block_split(free_block, size);
        if (remainder_block != NULL) {
            block_free_list_add(&allocator->free_blocks, remainder_block);
        }

        free_block->size &= ~BLOCK_IS_FREE_BIT;
        if (!BLOCK_LAST_IN_REGION(free_block)) {
            BLOCK_NEXT(free_block)->size &= ~BLOCK_PREVIOUS_IS_FREE_BIT;
        }

        return BLOCK_MEMORY(free_block);
    }

    isize new_region_size = isize_max(size + BLOCK_HEADER_SIZE, REGION_MIN_SIZE);

    Region *new_region;
    if (allocator->system_allocate != NULL) {
        new_region = allocator->system_allocate(REGION_HEADER_SIZE + new_region_size);
    } else {
        new_region = allocator->system_heap_grow(REGION_HEADER_SIZE + new_region_size);
    }
    if (new_region == NULL) {
        return NULL;
    }

    new_region->size = new_region_size;
    region_list_prepend(&allocator->regions, new_region);

    Block *new_block = region_first_block(new_region);
    new_block->size = (new_region_size - BLOCK_HEADER_SIZE) | BLOCK_IS_FREE_BIT;
    new_block->size |= BLOCK_LAST_IN_REGION_BIT;

    Block *remainder_block = block_split(new_block, size);
    if (remainder_block != NULL) {
        block_free_list_add(&allocator->free_blocks, remainder_block);
    }

    new_block->size &= ~BLOCK_IS_FREE_BIT;
    if (!BLOCK_LAST_IN_REGION(new_block)) {
        BLOCK_NEXT(new_block)->size &= ~BLOCK_PREVIOUS_IS_FREE_BIT;
    }

    return BLOCK_MEMORY(new_block);
}

void heap_deallocate(HeapAllocator *allocator, void *memory) {
    if (memory == NULL) {
        return;
    }
    Block *block = BLOCK_FROM_MEMORY(memory);

    Block *previous_block = NULL;
    if (BLOCK_PREVIOUS_IS_FREE(block)) {
        previous_block = BLOCK_PREVIOUS(block);
    }

    Block *next_block = NULL;
    if (!BLOCK_LAST_IN_REGION(block) && BLOCK_IS_FREE(BLOCK_NEXT(block))) {
        next_block = BLOCK_NEXT(block);
    }

    Block *merged_block = block;
    isize merged_block_size = BLOCK_SIZE(block);
    usize merged_block_previous_is_free_bit = merged_block->size & BLOCK_PREVIOUS_IS_FREE_BIT;
    usize merged_block_last_in_region_bit = merged_block->size & BLOCK_LAST_IN_REGION_BIT;

    if (previous_block != NULL) {
        merged_block = previous_block;
        merged_block_size += BLOCK_HEADER_SIZE + BLOCK_SIZE(previous_block);
        merged_block_previous_is_free_bit = previous_block->size & BLOCK_PREVIOUS_IS_FREE_BIT;
        block_free_list_remove(&allocator->free_blocks, previous_block);
    }
    if (next_block != NULL) {
        merged_block_size += BLOCK_HEADER_SIZE + BLOCK_SIZE(next_block);
        merged_block_last_in_region_bit = next_block->size & BLOCK_LAST_IN_REGION_BIT;
        block_free_list_remove(&allocator->free_blocks, next_block);
    }

    merged_block->size = merged_block_size | BLOCK_IS_FREE_BIT;
    merged_block->size |= merged_block_previous_is_free_bit;
    merged_block->size |= merged_block_last_in_region_bit;
    block_free_list_add(&allocator->free_blocks, merged_block);

    if (!BLOCK_LAST_IN_REGION(merged_block)) {
        Block *next_block_after_merged = BLOCK_NEXT(merged_block);
        next_block_after_merged->size |= BLOCK_PREVIOUS_IS_FREE_BIT;
        *BLOCK_PREVIOUS_SIZE(next_block_after_merged) = merged_block_size;
    }
}

void *heap_reallocate(HeapAllocator *allocator, void *memory, isize new_size) {
    new_size = ALIGN_UP(new_size, ALIGNMENT);
    if (new_size == 0) {
        heap_deallocate(allocator, memory);
        return NULL;
    }

    void *new_memory = heap_allocate(allocator, new_size);
    if (new_memory == NULL) {
        return NULL;
    }

    if (memory != NULL) {
        Block *block = BLOCK_FROM_MEMORY(memory);
        memory_copy(memory, isize_min(BLOCK_SIZE(block), new_size), new_memory);

        heap_deallocate(allocator, memory);
    }

    return new_memory;
}

void heap_iterate(HeapAllocator const *allocator, HeapIterator *iterator) {
    Block *next_block = NULL;

    if (iterator->region == NULL) {
        Region *first_region = allocator->regions;

        if (first_region != NULL) {
            next_block = region_first_block(first_region);
            iterator->region = first_region;
        }
    } else {
        Block *current_block = BLOCK_FROM_MEMORY(iterator->memory);

        if (!BLOCK_LAST_IN_REGION(current_block)) {
            next_block = BLOCK_NEXT(current_block);
        } else {
            Region *region = iterator->region;
            iterator->region = region->next;
            if (region->next != NULL) {
                next_block = region_first_block(region->next);
            }
        }
    }

    if (next_block != NULL) {
        iterator->memory = BLOCK_MEMORY(next_block);
        iterator->size = BLOCK_SIZE(next_block);
        iterator->is_free = BLOCK_IS_FREE(next_block);
    } else {
        iterator->memory = NULL;
        iterator->size = 0;
    }
}
