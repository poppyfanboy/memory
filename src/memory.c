#include "memory.h"

#include <stdint.h>
#include <assert.h>

#define SYSTEM_ALLOCATE_MIN_SIZE (256 * 1024)
#define SYSTEM_ALLOCATE_GRANULARITY (64 * 1024)

// This must be at least 8, because 3 lower bits of the block sizes are used for storing bit flags.
#define ALIGNMENT 16

typedef unsigned char u8;
typedef uint64_t u64;
typedef size_t usize;
typedef ptrdiff_t isize;

#define ARRAY_COUNT(array) (isize)(sizeof(array) / sizeof((array)[0]))

#define ALIGN_UP(value, alignment) (((usize)(value) + (alignment) - 1) & (~(usize)(alignment) + 1))
#define ALIGN_DOWN(value, alignment) ((usize)(value) & (~(usize)(alignment) + 1))

#if defined(__clang__) || defined(__GNUC__)
    #define u64_trailing_zeroes __builtin_ctzll
#else
    #define u64_trailing_zeroes u64_trailing_zeroes_manual
#endif

// https://graphics.stanford.edu/~seander/bithacks.html#ZerosOnRightParallel
static inline int u64_trailing_zeroes_manual(u64 value) {
    if (value == 0) {
        return 64;
    }

    // Isolate the lowest set bit.
    value &= ~value + 1;

    // Determine which half has the set bit, then determine which quarter has the bit, and so on.
    int trailing_zeroes = 63;
    if ((value & 0x00000000ffffffff) != 0) trailing_zeroes -= 32;
    if ((value & 0x0000ffff0000ffff) != 0) trailing_zeroes -= 16;
    if ((value & 0x00ff00ff00ff00ff) != 0) trailing_zeroes -= 8;
    if ((value & 0x0f0f0f0f0f0f0f0f) != 0) trailing_zeroes -= 4;
    if ((value & 0x3333333333333333) != 0) trailing_zeroes -= 2;
    if ((value & 0x5555555555555555) != 0) trailing_zeroes -= 1;

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
#define BLOCK_FLAG_BITS (BLOCK_IS_FREE_BIT | BLOCK_PREVIOUS_IS_FREE_BIT)

#define BLOCK_SIZE(block) (isize)((block)->size & ~BLOCK_FLAG_BITS)
#define BLOCK_IS_FREE(block) (((block)->size & BLOCK_IS_FREE_BIT) != 0)
#define BLOCK_PREVIOUS_IS_FREE(block) (((block)->size & BLOCK_PREVIOUS_IS_FREE_BIT) != 0)

#define BLOCK_MEMORY(block) (void *)((u8 *)(block) + BLOCK_HEADER_SIZE)
#define BLOCK_FROM_MEMORY(memory) ((Block *)((u8 *)(memory) - BLOCK_HEADER_SIZE))

// Accessing the previous block (or its size) is allowed only when previous block is marked as free.
// When the previous block is free, it stores its size right before the current block's header.
#define BLOCK_PREVIOUS_SIZE(block) (isize *)((u8 *)(block) - sizeof(isize))
#define BLOCK_PREVIOUS(block) \
    ((Block *)((u8 *)(block) - BLOCK_HEADER_SIZE - *BLOCK_PREVIOUS_SIZE(block)))

// Next block is always accessible thanks to zero-sized guard blocks at the end of each region.
#define BLOCK_NEXT(block) ((Block *)((u8 *)(block) + BLOCK_HEADER_SIZE + BLOCK_SIZE(block)))

static inline void block_set_size(Block *block, isize new_size) {
    block->size = new_size | (block->size & BLOCK_FLAG_BITS);
}

static inline void block_mark_as_free(Block *block) {
    block->size |= BLOCK_IS_FREE_BIT;

    Block *next_block = BLOCK_NEXT(block);
    next_block->size |= BLOCK_PREVIOUS_IS_FREE_BIT;
    *BLOCK_PREVIOUS_SIZE(next_block) = BLOCK_SIZE(block);
}

static inline void block_mark_as_in_use(Block *block) {
    block->size &= ~BLOCK_IS_FREE_BIT;

    Block *next_block = BLOCK_NEXT(block);
    next_block->size &= ~BLOCK_PREVIOUS_IS_FREE_BIT;
}

// Truncates the block to the desired size by chopping a block from its end.
// The returned remainder block is automatically marked as free.
static Block *block_split(Block *block, isize new_size) {
    isize min_size_to_split = new_size + (BLOCK_HEADER_SIZE + BLOCK_MIN_SIZE);
    if (BLOCK_SIZE(block) < min_size_to_split) {
        return NULL;
    }

    isize remainder_block_size = BLOCK_SIZE(block) - (BLOCK_HEADER_SIZE + new_size);
    block_set_size(block, new_size);

    Block *remainder_block = (Block *)((u8 *)block + (BLOCK_HEADER_SIZE + new_size));
    remainder_block->size = remainder_block_size;
    block_mark_as_free(remainder_block);

    if (BLOCK_IS_FREE(block)) {
        remainder_block->size |= BLOCK_PREVIOUS_IS_FREE_BIT;
        *BLOCK_PREVIOUS_SIZE(remainder_block) = BLOCK_SIZE(block);
    }

    return remainder_block;
}

typedef Block *BlockList;

typedef struct {
    Block *previous;
    Block *next;
} BlockListNode;

#define BLOCK_LIST_NODE(block) ((BlockListNode *)BLOCK_MEMORY(block))

static void block_list_add(BlockList *list, Block *block) {
    Block *previous = NULL;
    Block *next = *list;
    while (next != NULL && BLOCK_SIZE(block) > BLOCK_SIZE(next)) {
        previous = next;
        next = BLOCK_LIST_NODE(next)->next;
    }

    if (previous == NULL) {
        *list = block;
    }
    BLOCK_LIST_NODE(block)->previous = previous;
    BLOCK_LIST_NODE(block)->next = next;

    if (previous != NULL) {
        BLOCK_LIST_NODE(previous)->next = block;
    }
    if (next != NULL) {
        BLOCK_LIST_NODE(next)->previous = block;
    }
}

static void block_list_remove(BlockList *list, Block *block) {
    if (*list == block) {
        *list = BLOCK_LIST_NODE(*list)->next;
    }

    Block *previous = BLOCK_LIST_NODE(block)->previous;
    if (previous != NULL) {
        BLOCK_LIST_NODE(previous)->next = BLOCK_LIST_NODE(block)->next;
    }

    Block *next = BLOCK_LIST_NODE(block)->next;
    if (next != NULL) {
        BLOCK_LIST_NODE(next)->previous = BLOCK_LIST_NODE(block)->previous;
    }
}

static Block *block_list_get(BlockList *list, isize min_size) {
    Block *block_iter = *list;
    while (block_iter != NULL && BLOCK_SIZE(block_iter) < min_size) {
        block_iter = BLOCK_LIST_NODE(block_iter)->next;
    }

    if (block_iter != NULL) {
        block_list_remove(list, block_iter);
        return block_iter;
    } else {
        return NULL;
    }
}

struct Region {
    // Size of the memory available inside of the region (i.e. header size is not included).
    isize size;
    Region *previous;
    Region *next;
};

#define REGION_HEADER_SIZE (isize)ALIGN_UP(sizeof(Region), ALIGNMENT)

#define REGION_FIRST_BLOCK(region) ((Block *)((u8 *)(region) + REGION_HEADER_SIZE))

// A special zero-sized block which is always marked as "in-use". Placed at the end of each region.
#define REGION_GUARD_BLOCK(region) \
    ((Block *)((u8 *)(region) + (REGION_HEADER_SIZE + (region)->size) - (BLOCK_HEADER_SIZE + 0)))

static Region *region_initialize_from_memory(void *memory, isize region_size) {
    assert((usize)memory % ALIGNMENT == 0);

    Region *new_region = memory;
    new_region->size = region_size;

    Block *guard_block = REGION_GUARD_BLOCK(new_region);
    guard_block->size = (usize)0;

    isize first_block_size = region_size - BLOCK_HEADER_SIZE - (BLOCK_HEADER_SIZE + 0);
    assert(first_block_size > BLOCK_MIN_SIZE);

    Block *first_block = REGION_FIRST_BLOCK(new_region);
    first_block->size = (usize)first_block_size;
    block_mark_as_free(first_block);

    return new_region;
}

typedef Region *RegionList;

static void region_list_prepend(RegionList *list, Region *region) {
    region->previous = NULL;
    region->next = *list;

    if (*list != NULL) {
        (*list)->previous = region;
    }
    *list = region;
}

typedef struct {
    SystemAllocate allocate;
    SystemDeallocate deallocate;
    SystemHeapGrow heap_grow;
} SystemMemoryFunctions;

struct HeapAllocator {
    RegionList regions;

    u64 free_blocks_availability;
    BlockList free_blocks[64];

    SystemMemoryFunctions system;
};

static HeapAllocator *heap_allocator_bootstrap(
    void *initial_memory,
    isize initial_memory_size,
    SystemMemoryFunctions const *system
) {
    Region *initial_region = region_initialize_from_memory(
        initial_memory,
        initial_memory_size - REGION_HEADER_SIZE
    );

    Block *initial_block = REGION_FIRST_BLOCK(initial_region);
    block_mark_as_in_use(initial_block);

    u8 *memory = BLOCK_MEMORY(initial_block);
    u8 *memory_end = memory + BLOCK_SIZE(initial_block);

    u8 *memory_iter = memory;

    HeapAllocator *allocator = (HeapAllocator *)memory_iter;
    memory_iter += ALIGN_UP(sizeof(HeapAllocator), ALIGNMENT);
    assert(memory_iter <= memory_end);

    memory_set(allocator, sizeof(HeapAllocator), 0);
    region_list_prepend(&allocator->regions, initial_region);

    allocator->system.allocate = system->allocate;
    allocator->system.deallocate = system->deallocate;
    allocator->system.heap_grow = system->heap_grow;

    // Shrink the block to recycle unused memory from the initial region.
    heap_reallocate(allocator, memory, memory_iter - memory);

    return allocator;
}

HeapAllocator *heap_allocator_create(
    SystemAllocate system_allocate,
    SystemDeallocate system_deallocate
) {
    // Allocate enough memory for the HeapAllocator struct.
    void *initial_memory = system_allocate(SYSTEM_ALLOCATE_MIN_SIZE);
    if (initial_memory == NULL) {
        return NULL;
    }

    SystemMemoryFunctions system = {.allocate = system_allocate, .deallocate = system_deallocate};
    return heap_allocator_bootstrap(initial_memory, SYSTEM_ALLOCATE_MIN_SIZE, &system);
}

HeapAllocator *heap_allocator_from_system_heap(SystemHeapGrow system_heap_grow) {
    void *system_heap_base = system_heap_grow(0);
    if (system_heap_base == NULL) {
        return NULL;
    }

    // Align the system heap base, so that all subsequent allocations are properly aligned.
    usize heap_base_alignment = ALIGN_UP(system_heap_base, ALIGNMENT) - (usize)system_heap_base;
    system_heap_grow(heap_base_alignment);

    // Allocate enough memory for the HeapAllocator struct.
    void *initial_memory = system_heap_grow(SYSTEM_ALLOCATE_MIN_SIZE);
    if (initial_memory == NULL) {
        return NULL;
    }

    SystemMemoryFunctions system = {.heap_grow = system_heap_grow};
    return heap_allocator_bootstrap(initial_memory, SYSTEM_ALLOCATE_MIN_SIZE, &system);
}

void heap_allocator_destroy(HeapAllocator *allocator) {
    if (allocator->system.deallocate != NULL) {
        SystemDeallocate system_deallocate = allocator->system.deallocate;
        Region *region_iter = allocator->regions;

        while (region_iter != NULL) {
            Region *region = region_iter;
            region_iter = region_iter->next;

            system_deallocate(region);
        }
    }
}

static inline isize heap_allocator_free_list_index(HeapAllocator const *allocator, isize size) {
    size = ALIGN_DOWN(size, ALIGNMENT);
    isize min_size = ALIGN_UP(BLOCK_MIN_SIZE, ALIGNMENT);

    isize list_index = isize_min(
        (size - min_size) / ALIGNMENT,
        ARRAY_COUNT(allocator->free_blocks) - 1
    );
    return list_index;
}

static inline void heap_allocator_free_list_add(HeapAllocator *allocator, Block *block) {
    isize list_index = heap_allocator_free_list_index(allocator, BLOCK_SIZE(block));
    block_list_add(&allocator->free_blocks[list_index], block);

    allocator->free_blocks_availability |= (u64)1 << list_index;
}

static inline void heap_allocator_free_list_remove(HeapAllocator *allocator, Block *block) {
    isize list_index = heap_allocator_free_list_index(allocator, BLOCK_SIZE(block));
    block_list_remove(&allocator->free_blocks[list_index], block);

    if (allocator->free_blocks[list_index] == NULL) {
        allocator->free_blocks_availability &= ~((u64)1 << list_index);
    }
}

// Allocates a raw piece of memory at the end of the region by growing the system heap.
// You are expected to create a Block inside of it yourself.
static void *heap_allocate_raw_from_region(HeapAllocator *allocator, Region *region, isize size) {
    assert(allocator->system.heap_grow != NULL);
    assert(size > 0 && size % ALIGNMENT == 0);

    Block *block = REGION_GUARD_BLOCK(region);
    isize heap_grow_increment = size;
    if (BLOCK_PREVIOUS_IS_FREE(block)) {
        // Use up the whole free block at the end of the region for simplicity sake.
        //
        // (It's expected from the user that they've already checked if they could've used some free
        // block to satisfy the request without asking for more memory from the system.)
        block = BLOCK_PREVIOUS(block);
        heap_grow_increment = isize_max(size - (BLOCK_HEADER_SIZE + BLOCK_SIZE(block)), 0);
    }

    void *heap_grow_result = allocator->system.heap_grow(heap_grow_increment);
    if (heap_grow_result == NULL) {
        return NULL;
    }
    region->size += heap_grow_increment;

    if (BLOCK_IS_FREE(block)) {
        heap_allocator_free_list_remove(allocator, block);
        block_mark_as_in_use(block);
    }
    Block *region_guard_block = REGION_GUARD_BLOCK(region);
    region_guard_block->size = 0;

    return block;
}

static void *heap_allocate_from_free_list(HeapAllocator *allocator, isize size) {
    if (allocator->free_blocks_availability == 0) {
        return NULL;
    }

    isize free_list_min_index = heap_allocator_free_list_index(allocator, size);
    u64 free_blocks_availability_mask = ~(((u64)1 << free_list_min_index) - 1);
    if ((allocator->free_blocks_availability & free_blocks_availability_mask) == 0) {
        return NULL;
    }

    int free_list_index = u64_trailing_zeroes(
        allocator->free_blocks_availability & free_blocks_availability_mask
    );
    Block *free_block = block_list_get(&allocator->free_blocks[free_list_index], size);
    if (free_block == NULL) {
        return NULL;
    }

    Block *remainder_block = block_split(free_block, size);
    if (remainder_block != NULL) {
        heap_allocator_free_list_add(allocator, remainder_block);
    }

    block_mark_as_in_use(free_block);
    return BLOCK_MEMORY(free_block);
}

static void *heap_allocate_from_new_region(HeapAllocator *allocator, isize size) {
    // Additionally make space for the zero-sized "guard" block placed at the end of the new region.
    isize new_memory_size = (BLOCK_HEADER_SIZE + size) + (BLOCK_HEADER_SIZE + 0);
    new_memory_size = ALIGN_UP(new_memory_size, SYSTEM_ALLOCATE_GRANULARITY);
    new_memory_size = isize_max(new_memory_size, SYSTEM_ALLOCATE_MIN_SIZE);

    void *new_region_memory;
    if (allocator->system.allocate != NULL) {
        new_region_memory = allocator->system.allocate(new_memory_size);
    } else {
        new_region_memory = allocator->system.heap_grow(new_memory_size);
    }
    if (new_region_memory == NULL) {
        return NULL;
    }

    isize new_region_size = new_memory_size - REGION_HEADER_SIZE;
    Region *new_region = region_initialize_from_memory(new_region_memory, new_region_size);
    region_list_prepend(&allocator->regions, new_region);

    Block *new_block = REGION_FIRST_BLOCK(new_region);
    Block *remainder_block = block_split(new_block, size);
    if (remainder_block != NULL) {
        heap_allocator_free_list_add(allocator, remainder_block);
    }

    block_mark_as_in_use(new_block);
    return BLOCK_MEMORY(new_block);
}

void *heap_allocate(HeapAllocator *allocator, isize size) {
    if (size == 0) {
        return NULL;
    }

    size = isize_max(size, BLOCK_MIN_SIZE);
    size = ALIGN_UP(size, ALIGNMENT);

    // Try to take a block from one of the free lists.

    void *memory_from_free_list = heap_allocate_from_free_list(allocator, size);
    if (memory_from_free_list != NULL) {
        return memory_from_free_list;
    }

    // When memory is allocated from system heap, resize an existing region (in case there is one).

    bool resize_existing_region = false;
    if (allocator->system.allocate == NULL) {
        resize_existing_region = allocator->regions != NULL;
    }

    if (resize_existing_region) {
        Region *region = allocator->regions;

        u8 *raw_memory = heap_allocate_raw_from_region(allocator, region, BLOCK_HEADER_SIZE + size);
        if (raw_memory == NULL) {
            return NULL;
        }
        isize raw_memory_size = (u8 *)REGION_GUARD_BLOCK(region) - raw_memory;

        Block *new_block = (Block *)raw_memory;
        new_block->size = raw_memory_size - BLOCK_HEADER_SIZE;
        block_mark_as_in_use(new_block);

        return BLOCK_MEMORY(new_block);
    }

    // Resort to allocating a new region to fit a block of the requested size.

    return heap_allocate_from_new_region(allocator, size);
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
    if (BLOCK_IS_FREE(BLOCK_NEXT(block))) {
        next_block = BLOCK_NEXT(block);
    }

    Block *merged_block = block;
    isize merged_block_size = BLOCK_SIZE(block);

    if (previous_block != NULL) {
        merged_block = previous_block;
        merged_block_size += BLOCK_HEADER_SIZE + BLOCK_SIZE(previous_block);
        heap_allocator_free_list_remove(allocator, previous_block);
    }

    if (next_block != NULL) {
        merged_block_size += BLOCK_HEADER_SIZE + BLOCK_SIZE(next_block);
        heap_allocator_free_list_remove(allocator, next_block);
    }

    block_set_size(merged_block, merged_block_size);
    block_mark_as_free(merged_block);
    heap_allocator_free_list_add(allocator, merged_block);
}

void *heap_reallocate(HeapAllocator *allocator, void *memory, isize new_size) {
    if (new_size == 0) {
        heap_deallocate(allocator, memory);
        return NULL;
    }

    if (memory == NULL) {
        return heap_allocate(allocator, new_size);
    }

    new_size = isize_max(new_size, BLOCK_MIN_SIZE);
    new_size = ALIGN_UP(new_size, ALIGNMENT);

    Block *block = BLOCK_FROM_MEMORY(memory);
    isize old_size = BLOCK_SIZE(block);

    if (new_size <= old_size) {
        // Merge, so that we don't get two free blocks next to each other after splitting.
        Block *next_block = BLOCK_NEXT(block);
        if (BLOCK_IS_FREE(next_block)) {
            heap_allocator_free_list_remove(allocator, next_block);
            block_set_size(block, BLOCK_SIZE(block) + (BLOCK_HEADER_SIZE + BLOCK_SIZE(next_block)));
            block_mark_as_in_use(block);
        }

        Block *remainder_block = block_split(block, new_size);
        if (remainder_block != NULL) {
            heap_allocator_free_list_add(allocator, remainder_block);
        }

        return BLOCK_MEMORY(block);
    }

    // Try to take memory from the next block.

    if (BLOCK_IS_FREE(BLOCK_NEXT(block))) {
        Block *next_block = BLOCK_NEXT(block);
        isize max_growth_amount = BLOCK_HEADER_SIZE + BLOCK_SIZE(next_block);

        if (old_size + max_growth_amount >= new_size) {
            heap_allocator_free_list_remove(allocator, next_block);

            Block *merged_block = block;
            block_set_size(merged_block, old_size + max_growth_amount);
            block_mark_as_in_use(merged_block);

            // Trim the merged block.
            Block *remainder_block = block_split(merged_block, new_size);
            if (remainder_block != NULL) {
                heap_allocator_free_list_add(allocator, remainder_block);
            }

            return BLOCK_MEMORY(merged_block);
        }
    }

    // Try to take memory from the previous (and possibly the next) block.

    if (BLOCK_IS_FREE(BLOCK_PREVIOUS(block))) {
        Block *previous_block = BLOCK_PREVIOUS(block);
        Block *next_block = BLOCK_IS_FREE(BLOCK_NEXT(block)) ? BLOCK_NEXT(block) : NULL;

        isize max_growth_amount = BLOCK_HEADER_SIZE + BLOCK_SIZE(previous_block);
        if (next_block != NULL) {
            max_growth_amount += BLOCK_HEADER_SIZE + BLOCK_SIZE(next_block);
        }

        if (old_size + max_growth_amount >= new_size) {
            heap_allocator_free_list_remove(allocator, previous_block);
            if (next_block != NULL) {
                heap_allocator_free_list_remove(allocator, next_block);
            }

            // Always move memory to the beginning of the previous block, even if the previous block
            // has plenty more memory than we need. This way subsequent reallocation requests
            // potentially could be handled without any memory moves.

            Block *merged_block = previous_block;
            block_set_size(merged_block, old_size + max_growth_amount);
            block_mark_as_in_use(merged_block);

            memory_move(memory, old_size, BLOCK_MEMORY(merged_block));

            // Trim the merged block.
            Block *remainder_block = block_split(merged_block, new_size);
            if (remainder_block != NULL) {
                heap_allocator_free_list_add(allocator, remainder_block);
            }

            return BLOCK_MEMORY(merged_block);
        }
    }

    // Resort to copying memory over to completely separate block.

    // Try to get away without having to ask system for more memory.

    void *new_memory = heap_allocate_from_free_list(allocator, new_size);
    if (new_memory != NULL) {
        memory_copy(memory, old_size, new_memory);
        heap_deallocate(allocator, memory);
        return new_memory;
    }

    // When using system heap, try to resize the block at the cost of growing the heap.

    if (allocator->system.allocate == NULL) {
        bool block_is_last_non_free_within_region;
        {
            Block *next_block = BLOCK_NEXT(block);
            block_is_last_non_free_within_region =
                BLOCK_SIZE(next_block) == 0 ||
                BLOCK_IS_FREE(next_block) && BLOCK_SIZE(BLOCK_NEXT(next_block)) == 0;
        }

        if (block_is_last_non_free_within_region) {
            Region *region = allocator->regions;

            u8 *raw_memory = heap_allocate_raw_from_region(allocator, region, new_size - old_size);
            if (raw_memory == NULL) {
                return NULL;
            }
            isize raw_memory_size = (u8 *)REGION_GUARD_BLOCK(region) - raw_memory;

            assert((void *)BLOCK_NEXT(block) == (void *)raw_memory);
            block_set_size(block, BLOCK_SIZE(block) + raw_memory_size);
            block_mark_as_in_use(block);

            return BLOCK_MEMORY(block);
        }
    }

    // Fallback to "allocate + copy + deallocate".

    new_memory = heap_allocate(allocator, new_size);
    if (new_memory == NULL) {
        return NULL;
    }
    memory_copy(memory, old_size, new_memory);
    heap_deallocate(allocator, memory);

    return new_memory;
}

void heap_iterate(HeapAllocator const *allocator, HeapIterator *iterator) {
    Block *next_block = NULL;

    if (iterator->region == NULL) {
        Region *first_region = allocator->regions;

        if (first_region != NULL) {
            next_block = REGION_FIRST_BLOCK(first_region);
            iterator->region = first_region;
        }
    } else {
        Block *current_block = BLOCK_FROM_MEMORY(iterator->memory);
        next_block = BLOCK_NEXT(current_block);

        // Go to the next region once we reach the guard block.
        if (BLOCK_SIZE(next_block) == 0) {
            Region *region = iterator->region;
            iterator->region = region->next;
            if (region->next != NULL) {
                next_block = REGION_FIRST_BLOCK(region->next);
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
