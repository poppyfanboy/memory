#include "memory.h"

#include <stdint.h>

typedef unsigned char u8;
typedef uint64_t u64;
typedef size_t usize;
typedef ptrdiff_t isize;

#ifdef MEMORY_LIB_ASSERT
    #define assert(expression) MEMORY_LIB_ASSERT(expression)
#else
    #include <assert.h>
#endif

#define ARRAY_COUNT(array) (isize)(sizeof(array) / sizeof((array)[0]))

// This must be at least 8, because 3 lower bits of the block sizes are used for storing bit flags.
#define ALIGNMENT 16

#define ALIGN_UP(value, alignment) (((usize)(value) + (alignment) - 1) & (~(usize)(alignment) + 1))
#define ALIGN_DOWN(value, alignment) ((usize)(value) & (~(usize)(alignment) + 1))

// https://graphics.stanford.edu/~seander/bithacks.html#ZerosOnRightParallel
static inline int u64_trailing_zeroes(u64 value) {
    if (value == 0) {
        return 64;
    }

    // Isolate the lowest set bit.
    value &= (~value + 1);

    // Determine which half has the set bit, then determine which quarter has the bit, and so on.
    int trailing_zeroes = 63;
    if ((value & 0x00000000ffffffff) != 0) {
        trailing_zeroes -= 32;
    }
    if ((value & 0x0000ffff0000ffff) != 0) {
        trailing_zeroes -= 16;
    }
    if ((value & 0x00ff00ff00ff00ff) != 0) {
        trailing_zeroes -= 8;
    }
    if ((value & 0x0f0f0f0f0f0f0f0f) != 0) {
        trailing_zeroes -= 4;
    }
    if ((value & 0x3333333333333333) != 0) {
        trailing_zeroes -= 2;
    }
    if ((value & 0x5555555555555555) != 0) {
        trailing_zeroes -= 1;
    }

    return trailing_zeroes;
}

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
    // Size of the memory available inside of the region (i.e. header size is not included).
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
    // Size of the memory available inside of the block (i.e. header size is not included).
    // You can't use this value to access size directly: lower 3 bits are used for storing flags.
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
        Block *next_block = BLOCK_NEXT(remainder_block);

        next_block->size |= BLOCK_PREVIOUS_IS_FREE_BIT;
        *BLOCK_PREVIOUS_SIZE(next_block) = BLOCK_SIZE(remainder_block);
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

    u64 free_blocks_availability;
    BlockFreeList free_blocks[64];

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
    allocator->free_blocks_availability = 0;
    memory_set(allocator->free_blocks, sizeof(allocator->free_blocks), 0);

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
    allocator->free_blocks_availability = 0;
    memory_set(allocator->free_blocks, sizeof(allocator->free_blocks), 0);

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

static inline isize heap_allocator_free_list_index(HeapAllocator const *allocator, isize size) {
    size = ALIGN_DOWN(size, ALIGNMENT);

    isize const MIN_SIZE = ALIGN_UP(BLOCK_MIN_SIZE, ALIGNMENT);
    isize list_index = isize_min(
        (size - MIN_SIZE) / ALIGNMENT,
        ARRAY_COUNT(allocator->free_blocks) - 1
    );

    return list_index;
}

static inline void heap_allocator_free_list_add(HeapAllocator *allocator, Block *block) {
    isize list_index = heap_allocator_free_list_index(allocator, BLOCK_SIZE(block));
    block_free_list_add(&allocator->free_blocks[list_index], block);

    allocator->free_blocks_availability |= ((u64)1 << list_index);
}

static inline void heap_allocator_free_list_remove(HeapAllocator *allocator, Block *block) {
    isize list_index = heap_allocator_free_list_index(allocator, BLOCK_SIZE(block));
    block_free_list_remove(&allocator->free_blocks[list_index], block);

    if (allocator->free_blocks[list_index] == NULL) {
        allocator->free_blocks_availability &= ~((u64)1 << list_index);
    }
}

void *heap_allocate(HeapAllocator *allocator, isize size) {
    if (size == 0) {
        return NULL;
    }

    size = isize_max(size, BLOCK_MIN_SIZE);
    size = ALIGN_UP(size, ALIGNMENT);

    // Try to take a block from one of the free lists.

    isize free_list_min_index = heap_allocator_free_list_index(allocator, size);
    u64 free_blocks_availability_mask = ~(((u64)1 << free_list_min_index) - 1);
    if ((allocator->free_blocks_availability & free_blocks_availability_mask) != 0) {
        int free_list_index = u64_trailing_zeroes(
            allocator->free_blocks_availability & free_blocks_availability_mask
        );

        Block *free_block = block_free_list_get(&allocator->free_blocks[free_list_index], size);
        if (free_block != NULL) {
            Block *remainder_block = block_split(free_block, size);
            if (remainder_block != NULL) {
                heap_allocator_free_list_add(allocator, remainder_block);
            }

            free_block->size &= ~BLOCK_IS_FREE_BIT;
            if (!BLOCK_LAST_IN_REGION(free_block)) {
                BLOCK_NEXT(free_block)->size &= ~BLOCK_PREVIOUS_IS_FREE_BIT;
            }

            return BLOCK_MEMORY(free_block);
        }
    }

    // When memory is allocated from system heap, resize an existing region (in case there is one).

    bool resize_existing_region = false;
    if (allocator->system_allocate == NULL) {
        assert(allocator->system_heap_grow != NULL);
        resize_existing_region = (allocator->regions != NULL);
    }
    if (resize_existing_region) {
        Block *new_block;

        // We maintain and resize a single region of memory.
        Region *region = allocator->regions;

        Block *region_guard_block = (Block *)(
            ((u8 *)region + REGION_HEADER_SIZE + region->size) - (BLOCK_HEADER_SIZE + 0)
        );

        if (BLOCK_PREVIOUS_IS_FREE(region_guard_block)) {
            // Try to resize an existing free block at the end of the region.
            new_block = BLOCK_PREVIOUS(region_guard_block);
            heap_allocator_free_list_remove(allocator, new_block);

            assert(BLOCK_SIZE(new_block) < size);
            isize heap_grow_increment = size - BLOCK_SIZE(new_block);

            void *heap_grow_result = allocator->system_heap_grow(heap_grow_increment);
            if (heap_grow_result == NULL) {
                return NULL;
            }
            region->size += heap_grow_increment;

            usize new_block_flags = (new_block->size & BLOCK_FLAG_BITS) & ~BLOCK_IS_FREE_BIT;
            new_block->size = (usize)size | new_block_flags;
        } else {
            // Allocate a new block.
            isize heap_grow_increment = BLOCK_HEADER_SIZE + size;

            void *heap_grow_result = allocator->system_heap_grow(heap_grow_increment);
            if (heap_grow_result == NULL) {
                return NULL;
            }
            region->size += heap_grow_increment;

            new_block = region_guard_block;
            new_block->size = (usize)size;
        }

        // Guard blocks are always marked as "in use", so that they don't get merged with neighbors.
        Block *guard_block = BLOCK_NEXT(new_block);
        guard_block->size = (usize)0;
        guard_block->size |= BLOCK_LAST_IN_REGION_BIT;

        return BLOCK_MEMORY(new_block);
    }

    // Resort to allocating a new region to fit a block of the requested size.

    // Additionally make space for the zero-sized "guard" block placed at the end of the new region.
    isize new_region_size = (BLOCK_HEADER_SIZE + size) + (BLOCK_HEADER_SIZE + 0);
    if (new_region_size < REGION_MIN_SIZE) {
        new_region_size = REGION_MIN_SIZE;
    }

    isize new_block_size = new_region_size - BLOCK_HEADER_SIZE - (BLOCK_HEADER_SIZE + 0);
    assert(new_block_size >= size);

    void *new_region_memory;
    if (allocator->system_allocate != NULL) {
        new_region_memory = allocator->system_allocate(REGION_HEADER_SIZE + new_region_size);
    } else {
        new_region_memory = allocator->system_heap_grow(REGION_HEADER_SIZE + new_region_size);
    }
    if (new_region_memory == NULL) {
        return NULL;
    }

    Region *new_region = new_region_memory;
    new_region->size = new_region_size;
    region_list_prepend(&allocator->regions, new_region);

    // No need to set BLOCK_PREVIOUS_SIZE() here, because this block will get used immediately.
    Block *new_block = region_first_block(new_region);
    new_block->size = (usize)new_block_size | BLOCK_IS_FREE_BIT;

    Block *guard_block = BLOCK_NEXT(new_block);
    guard_block->size = (usize)0;
    guard_block->size |= BLOCK_PREVIOUS_IS_FREE_BIT;
    guard_block->size |= BLOCK_LAST_IN_REGION_BIT;

    Block *remainder_block = block_split(new_block, size);
    if (remainder_block != NULL) {
        heap_allocator_free_list_add(allocator, remainder_block);
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
        heap_allocator_free_list_remove(allocator, previous_block);
    }
    if (next_block != NULL) {
        merged_block_size += BLOCK_HEADER_SIZE + BLOCK_SIZE(next_block);
        merged_block_last_in_region_bit = next_block->size & BLOCK_LAST_IN_REGION_BIT;
        heap_allocator_free_list_remove(allocator, next_block);
    }

    merged_block->size = merged_block_size | BLOCK_IS_FREE_BIT;
    merged_block->size |= merged_block_previous_is_free_bit;
    merged_block->size |= merged_block_last_in_region_bit;
    heap_allocator_free_list_add(allocator, merged_block);

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

        bool move_to_next_region;
        if (BLOCK_LAST_IN_REGION(current_block)) {
            move_to_next_region = true;
        } else {
            next_block = BLOCK_NEXT(current_block);
            move_to_next_region = false;

            // Skip over the zero-sized guard block. These occur only at the end of a region.
            if (BLOCK_SIZE(next_block) == 0) {
                move_to_next_region = true;
            }
        }

        if (move_to_next_region) {
            Region *region = iterator->region;
            iterator->region = region->next;
            if (region->next != NULL) {
                next_block = region_first_block(region->next);
            } else {
                next_block = NULL;
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
