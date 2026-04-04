#include "memory.h"

#include <stdint.h>
#include <assert.h>

// This must be at least 8, because 3 lower bits of the block sizes are used for storing bit flags.
#define ALIGNMENT 16

// Least common multiple of page sizes (alloc granularities to be exact) on Linux / Windows / WASM.
#define PAGE_SIZE (64 * 1024)

// This must be large enough to fit allocator state + initial region.
#define INITIAL_MEMORY_SIZE (256 * 1024)

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
#define BLOCK_MIN_SIZE 32

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
    min_size_to_split = ALIGN_UP(min_size_to_split, ALIGNMENT);
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

typedef struct ListNode ListNode;

struct ListNode {
    ListNode *previous;
    ListNode *next;
};

#define BLOCK_LIST_NODE(block) ((ListNode *)BLOCK_MEMORY(block))
#define BLOCK_FROM_LIST_NODE(node) ((Block *)BLOCK_FROM_MEMORY(node))

static void list_insert(ListNode *list, ListNode *node) {
    node->next = list->next;
    node->previous = list;

    list->next->previous = node;
    list->next = node;
}

static void list_delete(ListNode *node) {
    node->previous->next = node->next;
    node->next->previous = node->previous;
}

typedef enum {
    TREE_NODE_BLACK,
    TREE_NODE_RED,
} TreeNodeColor;

typedef struct TreeNode TreeNode;

struct TreeNode {
    TreeNodeColor color;
    TreeNode *parent;
    TreeNode *left;
    TreeNode *right;
};

#define BLOCK_TREE_NODE(block) ((TreeNode *)BLOCK_MEMORY(block))
#define BLOCK_FROM_TREE_NODE(node) ((Block *)BLOCK_FROM_MEMORY(node))

#define TREE_NODE_KEY(node) BLOCK_SIZE(BLOCK_FROM_TREE_NODE(node))

// A red-black tree as it is described in "Introduction to Algorithms" book (a.k.a. CLRS).
typedef struct {
    TreeNode *null;
    TreeNode *root;
} Tree;

static void tree_insert(Tree *tree, TreeNode *node);
static void tree_delete(Tree *tree, TreeNode *node);

static TreeNode *tree_lower_bound_search(Tree const *tree, isize min_key) {
    TreeNode *result = NULL;

    TreeNode *node_iter = tree->root;
    while (node_iter != tree->null) {
        if (TREE_NODE_KEY(node_iter) >= min_key) {
            result = node_iter;
            node_iter = node_iter->left;
        } else {
            node_iter = node_iter->right;
        }
    }

    return result;
}

struct Region {
    // Size of the memory available inside of the region (i.e. header size is not included).
    isize size;
    Region *previous;
    Region *next;
};

#define REGION_HEADER_SIZE (isize)ALIGN_UP(sizeof(Region), ALIGNMENT)
#define REGION_MIN_SIZE (256 * 1024)

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

// This structure must be pinned in memory, so that addresses of linked list sentinels never change.
struct HeapAllocator {
    RegionList regions;

    // Sentinel elements representing circular linked lists.
    // "free_blocks[N].next" points to the first list element, ".previous" points to the last one.
    ListNode free_blocks[64];
    u64 free_blocks_availability;

    Tree free_block_tree;

    void *user_context;
    SystemAllocate system_allocate;
    SystemDeallocate system_deallocate;
    unsigned int flags;
};

static HeapAllocator *heap_allocator_bootstrap(
    void *initial_memory,
    isize initial_memory_size,
    void *user_context,
    SystemAllocate system_allocate,
    SystemDeallocate system_deallocate,
    unsigned int flags
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

    TreeNode *tree_null_node = (TreeNode *)memory_iter;
    memory_iter += ALIGN_UP(sizeof(TreeNode), ALIGNMENT);
    assert(memory_iter <= memory_end);

    memory_set(allocator, sizeof(HeapAllocator), 0);
    region_list_prepend(&allocator->regions, initial_region);

    for (int i = 0; i < ARRAY_COUNT(allocator->free_blocks); i += 1) {
        allocator->free_blocks[i].next = &allocator->free_blocks[i];
        allocator->free_blocks[i].previous = &allocator->free_blocks[i];
    }

    tree_null_node->color = TREE_NODE_BLACK;
    allocator->free_block_tree.null = tree_null_node;
    allocator->free_block_tree.root = tree_null_node;

    allocator->user_context = user_context;
    allocator->system_allocate = system_allocate;
    allocator->system_deallocate = system_deallocate;
    allocator->flags = flags;

    // Shrink the block to recycle unused memory from the initial region.
    heap_reallocate(allocator, memory, memory_iter - memory);

    return allocator;
}

HeapAllocator *heap_allocator_create(
    void *user_context,
    SystemAllocate system_allocate,
    SystemDeallocate system_deallocate,
    unsigned int flags
) {
    if ((flags & SYSTEM_ALLOCATE_IS_CONTIGUOUS) != 0) {
        // We were provided with an sbrk-like interface.

        if ((flags & SYSTEM_ALLOCATE_HAS_BYTE_GRANULARITY) != 0) {
            void *system_heap_base = system_allocate(user_context, 0);
            if (system_heap_base == NULL) {
                return NULL;
            }

            // Align the system heap base, so that all subsequent allocations are properly aligned.
            usize base_alignment = ALIGN_UP(system_heap_base, ALIGNMENT) - (usize)system_heap_base;
            system_allocate(user_context, base_alignment);
        }

        isize initial_memory_size = ALIGN_UP(INITIAL_MEMORY_SIZE, PAGE_SIZE);
        void *initial_memory = system_allocate(user_context, initial_memory_size);
        if (initial_memory == NULL) {
            return NULL;
        }

        return heap_allocator_bootstrap(
            initial_memory, initial_memory_size,
            user_context, system_allocate, system_deallocate, flags
        );
    }

    // We were provided with a mmap/munmap-like interface.

    isize initial_memory_size = ALIGN_UP(INITIAL_MEMORY_SIZE, PAGE_SIZE);
    void *initial_memory = system_allocate(user_context, INITIAL_MEMORY_SIZE);
    if (initial_memory == NULL) {
        return NULL;
    }

    return heap_allocator_bootstrap(
        initial_memory, initial_memory_size,
        user_context, system_allocate, system_deallocate, flags
    );
}

void heap_allocator_destroy(HeapAllocator *allocator) {
    if (allocator->system_deallocate != NULL) {
        SystemDeallocate system_deallocate = allocator->system_deallocate;
        Region *region_iter = allocator->regions;

        while (region_iter != NULL) {
            Region *region = region_iter;
            region_iter = region_iter->next;

            system_deallocate(allocator->user_context, region);
        }
    }
}

// Returns ARRAY_COUNT(...) index in case the size is too large for any of fixed-sized free lists.
static isize heap_allocator_free_list_index(HeapAllocator const *allocator, isize size) {
    // We "downplay" the size of a free block, so that an allocation of the corresponding size would
    // definitely fit inside of the block.
    size = ALIGN_DOWN(size, ALIGNMENT);

    isize min_size = ALIGN_UP(BLOCK_MIN_SIZE, ALIGNMENT);
    assert(size >= min_size);

    isize list_index = isize_min(
        (size - min_size) / ALIGNMENT,
        ARRAY_COUNT(allocator->free_blocks)
    );
    return list_index;
}

static void heap_allocator_free_list_add(HeapAllocator *allocator, Block *block) {
    isize list_index = heap_allocator_free_list_index(allocator, BLOCK_SIZE(block));

    if (list_index < ARRAY_COUNT(allocator->free_blocks)) {
        list_insert(&allocator->free_blocks[list_index], BLOCK_LIST_NODE(block));
        allocator->free_blocks_availability |= (u64)1 << list_index;
    } else {
        tree_insert(&allocator->free_block_tree, BLOCK_TREE_NODE(block));
    }
}

static void heap_allocator_free_list_remove(HeapAllocator *allocator, Block *block) {
    isize list_index = heap_allocator_free_list_index(allocator, BLOCK_SIZE(block));

    if (list_index < ARRAY_COUNT(allocator->free_blocks)) {
        list_delete(BLOCK_LIST_NODE(block));

        ListNode *list = &allocator->free_blocks[list_index];
        if (list->next == list) {
            allocator->free_blocks_availability &= ~((u64)1 << list_index);
        }
    } else {
        tree_delete(&allocator->free_block_tree, BLOCK_TREE_NODE(block));
    }
}

static void *heap_allocate_from_free_list(HeapAllocator *allocator, isize size) {
    Block *free_block = NULL;

    // Try to find a free block in one of the fixed-size free lists.
    isize free_list_min_index = heap_allocator_free_list_index(allocator, size);
    if (free_list_min_index < ARRAY_COUNT(allocator->free_blocks)) {
        u64 free_blocks_availability_mask = ~(((u64)1 << free_list_min_index) - 1);

        if ((allocator->free_blocks_availability & free_blocks_availability_mask) != 0) {
            int free_list_index = u64_trailing_zeroes(
                allocator->free_blocks_availability & free_blocks_availability_mask
            );

            ListNode *list = &allocator->free_blocks[free_list_index];
            free_block = BLOCK_FROM_LIST_NODE(list->next);
            list_delete(list->next);
        }
    }

    // Fallback to searching in the tree.
    if (free_block == NULL) {
        TreeNode *free_block_node = tree_lower_bound_search(&allocator->free_block_tree, size);
        if (free_block_node != NULL) {
            tree_delete(&allocator->free_block_tree, free_block_node);
            free_block = BLOCK_FROM_TREE_NODE(free_block_node);
        }
    }

    if (free_block != NULL) {
        Block *remainder_block = block_split(free_block, size);
        if (remainder_block != NULL) {
            heap_allocator_free_list_add(allocator, remainder_block);
        }

        block_mark_as_in_use(free_block);
    }

    if (free_block != NULL) {
        return BLOCK_MEMORY(free_block);
    } else {
        return NULL;
    }
}

static Region *heap_allocator_new_region(HeapAllocator *allocator, isize size) {
    isize new_memory_size = REGION_HEADER_SIZE + isize_max(size, REGION_MIN_SIZE);
    new_memory_size = ALIGN_UP(new_memory_size, PAGE_SIZE);

    void *new_region_memory = allocator->system_allocate(allocator->user_context, new_memory_size);
    if (new_region_memory == NULL) {
        return NULL;
    }

    isize new_region_size = new_memory_size - REGION_HEADER_SIZE;
    Region *new_region = region_initialize_from_memory(new_region_memory, new_region_size);
    region_list_prepend(&allocator->regions, new_region);

    return new_region;
}

// Tries to grow a block located at the end of a region by resizing the region.
// Only works when system allocates memory contiguously and we maintain a single growable region.
static Block *heap_allocator_grow_last_block(
    HeapAllocator *allocator,
    Block *block,
    isize block_increment
) {
    assert(BLOCK_SIZE(block) == 0 || BLOCK_SIZE(BLOCK_NEXT(block)) == 0);

    Region *region = allocator->regions;
    assert(region->next == NULL);
    assert((allocator->flags & SYSTEM_ALLOCATE_IS_CONTIGUOUS) != 0);

    isize region_increment = block_increment;
    if (BLOCK_SIZE(block) == 0) {
        // If we are growing the guard block, make space for the new guard block.
        region_increment += (BLOCK_HEADER_SIZE + 0);
    }
    if ((allocator->flags & SYSTEM_ALLOCATE_HAS_BYTE_GRANULARITY) != 0) {
        region_increment = ALIGN_UP(region_increment, ALIGNMENT);
    } else {
        region_increment = ALIGN_UP(region_increment, PAGE_SIZE);
    }

    void *heap_grow_result = allocator->system_allocate(allocator->user_context, region_increment);
    if (heap_grow_result == NULL) {
        return NULL;
    }
    region->size += region_increment;

    Block *region_guard_block = REGION_GUARD_BLOCK(region);
    region_guard_block->size = 0;

    block_set_size(block, (u8 *)region_guard_block - (u8 *)BLOCK_MEMORY(block));
    return block;
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

    // When memory is allocated from system heap, resize an existing region.

    if ((allocator->flags & SYSTEM_ALLOCATE_IS_CONTIGUOUS) != 0) {
        Region *region = allocator->regions;

        Block *last_block = REGION_GUARD_BLOCK(region);
        if (BLOCK_PREVIOUS_IS_FREE(last_block)) {
            last_block = BLOCK_PREVIOUS(last_block);
            heap_allocator_free_list_remove(allocator, last_block);
        }

        last_block = heap_allocator_grow_last_block(
            allocator,
            last_block,
            size - BLOCK_SIZE(last_block)
        );
        if (last_block == NULL) {
            return NULL;
        }

        Block *remainder_block = block_split(last_block, size);
        if (remainder_block != NULL) {
            heap_allocator_free_list_add(allocator, remainder_block);
        }

        block_mark_as_in_use(last_block);
        return BLOCK_MEMORY(last_block);
    }

    // Allocate a new region otherwise.

    // Aside from requested memory, make space for the zero-sized guard block.
    isize new_region_min_size = (BLOCK_HEADER_SIZE + size) + (BLOCK_HEADER_SIZE + 0);
    Region *new_region = heap_allocator_new_region(allocator, new_region_min_size);

    Block *new_block = REGION_FIRST_BLOCK(new_region);
    Block *remainder_block = block_split(new_block, size);
    if (remainder_block != NULL) {
        heap_allocator_free_list_add(allocator, remainder_block);
    }

    block_mark_as_in_use(new_block);
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

    // Handle shrinking the block.

    if (new_size == old_size) {
        return BLOCK_MEMORY(block);
    }

    if (new_size < old_size) {
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

    if (BLOCK_PREVIOUS_IS_FREE(block)) {
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

            // "memory_copy" will handle copying backwards even for overlaping regions.
            memory_copy(memory, old_size, BLOCK_MEMORY(merged_block));

            // Trim the merged block.
            Block *remainder_block = block_split(merged_block, new_size);
            if (remainder_block != NULL) {
                heap_allocator_free_list_add(allocator, remainder_block);
            }

            return BLOCK_MEMORY(merged_block);
        }
    }

    // Do the memory copy, but still try to get away without having to ask system for more memory.

    void *new_memory = heap_allocate_from_free_list(allocator, new_size);
    if (new_memory != NULL) {
        memory_copy(memory, old_size, new_memory);
        heap_deallocate(allocator, memory);
        return new_memory;
    }

    // When using system heap, try to resize the block at the cost of growing the heap.

    if ((allocator->flags & SYSTEM_ALLOCATE_IS_CONTIGUOUS) != 0) {
        Region *region = allocator->regions;

        Block *last_free_block = REGION_GUARD_BLOCK(region);
        if (BLOCK_PREVIOUS_IS_FREE(last_free_block)) {
            last_free_block = BLOCK_PREVIOUS(last_free_block);
        }

        if (BLOCK_NEXT(block) == last_free_block) {
            // This block wasn't big enough to satisfy reallocation earlier, so consume it entirely.
            if (BLOCK_IS_FREE(last_free_block)) {
                heap_allocator_free_list_remove(allocator, last_free_block);
                block_set_size(
                    block,
                    BLOCK_SIZE(block) + (BLOCK_HEADER_SIZE + BLOCK_SIZE(last_free_block))
                );
            }

            // There could be some reusable memory in the previous block. Ignore it for simplicity.
            Block *last_block = block;

            last_block = heap_allocator_grow_last_block(
                allocator,
                last_block,
                new_size - BLOCK_SIZE(last_block)
            );
            if (last_block == NULL) {
                return NULL;
            }

            Block *remainder_block = block_split(last_block, new_size);
            if (remainder_block != NULL) {
                heap_allocator_free_list_add(allocator, remainder_block);
            }

            block_mark_as_in_use(last_block);
            return BLOCK_MEMORY(last_block);
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

// Operations on red-black trees. Direct translation from pseudocode presented in CLRS.

static void tree_left_rotate(Tree *tree, TreeNode *x) {
    TreeNode *y = x->right;
    x->right = y->left;
    if (y->left != tree->null) {
        y->left->parent = x;
    }
    y->parent = x->parent;
    if (x->parent == tree->null) {
        tree->root = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    y->left = x;
    x->parent = y;
}

static void tree_right_rotate(Tree *tree, TreeNode *x) {
    TreeNode *y = x->left;
    x->left = y->right;
    if (y->right != tree->null) {
        y->right->parent = x;
    }
    y->parent = x->parent;
    if (x->parent == tree->null) {
        tree->root = y;
    } else if (x == x->parent->right) {
        x->parent->right = y;
    } else {
        x->parent->left = y;
    }
    y->right = x;
    x->parent = y;
}

static void tree_insert_fixup(Tree *tree, TreeNode *z) {
    while (z->parent->color == TREE_NODE_RED) {
        if (z->parent == z->parent->parent->left) {
            TreeNode *y = z->parent->parent->right;
            if (y->color == TREE_NODE_RED) {
                z->parent->color = TREE_NODE_BLACK;
                y->color = TREE_NODE_BLACK;
                z->parent->parent->color = TREE_NODE_RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    tree_left_rotate(tree, z);
                }
                z->parent->color = TREE_NODE_BLACK;
                z->parent->parent->color = TREE_NODE_RED;
                tree_right_rotate(tree, z->parent->parent);
            }
        } else {
            TreeNode *y = z->parent->parent->left;
            if (y->color == TREE_NODE_RED) {
                z->parent->color = TREE_NODE_BLACK;
                y->color = TREE_NODE_BLACK;
                z->parent->parent->color = TREE_NODE_RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    tree_right_rotate(tree, z);
                }
                z->parent->color = TREE_NODE_BLACK;
                z->parent->parent->color = TREE_NODE_RED;
                tree_left_rotate(tree, z->parent->parent);
            }
        }
    }
    tree->root->color = TREE_NODE_BLACK;
}

static void tree_insert(Tree *tree, TreeNode *z) {
    TreeNode *x = tree->root;
    TreeNode *y = tree->null;
    while (x != tree->null) {
        y = x;
        if (TREE_NODE_KEY(z) < TREE_NODE_KEY(x)) {
            x = x->left;
        } else {
            x = x->right;
        }
    }
    z->parent = y;
    if (y == tree->null) {
        tree->root = z;
    } else if (TREE_NODE_KEY(z) < TREE_NODE_KEY(y)) {
        y->left = z;
    } else {
        y->right = z;
    }
    z->left = tree->null;
    z->right = tree->null;
    z->color = TREE_NODE_RED;
    tree_insert_fixup(tree, z);
}

static TreeNode *tree_minimum(Tree *tree, TreeNode *node) {
    while (node->left != tree->null) {
        node = node->left;
    }
    return node;
}

static void tree_node_transplant(Tree *tree, TreeNode *u, TreeNode *v) {
    if (u->parent == tree->null) {
        tree->root = v;
    } else if (u == u->parent->left) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }
    v->parent = u->parent;
}

static void tree_delete_fixup(Tree *tree, TreeNode *x) {
    while (x != tree->root && x->color == TREE_NODE_BLACK) {
        if (x == x->parent->left) {
            TreeNode *w = x->parent->right;
            if (w->color == TREE_NODE_RED) {
                w->color = TREE_NODE_BLACK;
                x->parent->color = TREE_NODE_RED;
                tree_left_rotate(tree, x->parent);
                w = x->parent->right;
            }
            if (w->left->color == TREE_NODE_BLACK && w->right->color == TREE_NODE_BLACK) {
                w->color = TREE_NODE_RED;
                x = x->parent;
            } else {
                if (w->right->color == TREE_NODE_BLACK) {
                    w->left->color = TREE_NODE_BLACK;
                    w->color = TREE_NODE_RED;
                    tree_right_rotate(tree, w);
                    w = x->parent->right;
                }
                w->color = x->parent->color;
                x->parent->color = TREE_NODE_BLACK;
                w->right->color = TREE_NODE_BLACK;
                tree_left_rotate(tree, x->parent);
                x = tree->root;
            }
        } else {
            TreeNode *w = x->parent->left;
            if (w->color == TREE_NODE_RED) {
                w->color = TREE_NODE_BLACK;
                x->parent->color = TREE_NODE_RED;
                tree_right_rotate(tree, x->parent);
                w = x->parent->left;
            }
            if (w->right->color == TREE_NODE_BLACK && w->left->color == TREE_NODE_BLACK) {
                w->color = TREE_NODE_RED;
                x = x->parent;
            } else {
                if (w->left->color == TREE_NODE_BLACK) {
                    w->right->color = TREE_NODE_BLACK;
                    w->color = TREE_NODE_RED;
                    tree_left_rotate(tree, w);
                    w = x->parent->left;
                }
                w->color = x->parent->color;
                x->parent->color = TREE_NODE_BLACK;
                w->left->color = TREE_NODE_BLACK;
                tree_right_rotate(tree, x->parent);
                x = tree->root;
            }
        }
    }
    x->color = TREE_NODE_BLACK;
}

static void tree_delete(Tree *tree, TreeNode *z) {
    TreeNode *x;
    TreeNode *y = z;
    TreeNodeColor y_original_color = y->color;
    if (z->left == tree->null) {
        x = z->right;
        tree_node_transplant(tree, z, z->right);
    } else if (z->right == tree->null) {
        x = z->left;
        tree_node_transplant(tree, z, z->left);
    } else {
        y = tree_minimum(tree, z->right);
        y_original_color = y->color;
        x = y->right;
        if (y != z->right) {
            tree_node_transplant(tree, y, y->right);
            y->right = z->right;
            y->right->parent = y;
        } else {
            x->parent = y;
        }
        tree_node_transplant(tree, z, y);
        y->left = z->left;
        y->left->parent = y;
        y->color = z->color;
    }
    if (y_original_color == TREE_NODE_BLACK) {
        tree_delete_fixup(tree, x);
    }
}
