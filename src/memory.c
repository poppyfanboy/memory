#include "memory.h"

// This must be at least 8, because 3 lower bits of block storage size are used to store bit flags.
// Storage size is the total amount of memory occupied by a block, i.e. block size including header.
#define ALIGNMENT (2 * sizeof(usize))

// Least common multiple of page sizes (alloc granularities to be exact) on Linux / Windows / WASM.
#define PAGE_SIZE (64 * 1024)

// Memory requests this large are allocated directly with SystemAllocate. Freed immediately later.
#define USE_SYSTEM_ALLOCATE_DIRECTLY_THRESHOLD (8 * 1024 * 1024)

#include <stdint.h>

#define ISIZE_MAX PTRDIFF_MAX

typedef unsigned char u8;
typedef uint64_t u64;
typedef size_t usize;
typedef ptrdiff_t isize;

#define ARRAY_COUNT(array) (isize)(sizeof(array) / sizeof((array)[0]))

#define ALIGN_UP(value, alignment) (((usize)(value) + (alignment) - 1) & (~(usize)(alignment) + 1))
#define ALIGN_DOWN(value, alignment) ((usize)(value) & (~(usize)(alignment) + 1))

static inline isize isize_min(isize left, isize right) {
    return left < right ? left : right;
}

static inline isize isize_max(isize left, isize right) {
    return left > right ? left : right;
}

#ifdef __wasm__
    #define assert(predicate)

    void *memset(void *dest, int filler, usize size) {
        #ifdef __wasm_bulk_memory__
        if (size > 16) {
            return __builtin_memset(dest, filler, size);
        }
        #endif

        u8 *dest_iter = dest;
        u8 *dest_end = (u8 *)dest + size;

        while (dest_iter < dest_end) {
            *dest_iter = filler;
            dest_iter += 1;
        }

        return dest;
    }

    void *memcpy(void *dest, const void *source, usize size) {
        #ifdef __wasm_bulk_memory__
        if (size > 16) {
            return __builtin_memcpy(dest, source, size);
        }
        #endif

        u8 const *source_iter = source;
        u8 const *source_end = (u8 const *)source + size;
        u8 *dest_iter = dest;

        while (source_iter < source_end) {
            *dest_iter = *source_iter;
            source_iter += 1;
            dest_iter += 1;
        }

        return dest;
    }
#else
    #include <assert.h>
    #include <string.h>
#endif

#if defined(__clang__) || defined(__GNUC__)
    #define u64_trailing_zeroes __builtin_ctzll
#elif defined(_MSC_VER) && defined(_WIN64)
    #define u64_trailing_zeroes u64_trailing_zeroes_msvc

    #include <intrin.h>

    static inline int u64_trailing_zeroes_msvc(u64 value) {
        unsigned long trailing_zeroes;
        _BitScanForward64(&trailing_zeroes, value);
        return trailing_zeroes;
    }
#else
    #define u64_trailing_zeroes u64_trailing_zeroes_manual

    // https://graphics.stanford.edu/~seander/bithacks.html#ZerosOnRightParallel
    static inline int u64_trailing_zeroes_manual(u64 value) {
        assert(value != 0);

        // Isolate the lowest set bit.
        value &= ~value + 1;

        // Check if the lower half is all zeroes. If it is not, then the other half cannot possibly
        // contribute any of its zeroes into the result. Repeat for both halves in parallel.
        int trailing_zeroes = 63;
        if ((value & 0x00000000ffffffff) != 0) trailing_zeroes -= 32;
        if ((value & 0x0000ffff0000ffff) != 0) trailing_zeroes -= 16;
        if ((value & 0x00ff00ff00ff00ff) != 0) trailing_zeroes -= 8;
        if ((value & 0x0f0f0f0f0f0f0f0f) != 0) trailing_zeroes -= 4;
        if ((value & 0x3333333333333333) != 0) trailing_zeroes -= 2;
        if ((value & 0x5555555555555555) != 0) trailing_zeroes -= 1;

        return trailing_zeroes;
    }
#endif

typedef struct Block Block;
typedef struct Region Region;

struct Block {
    // You cannot use this field directly: lower bits are used for storing bit flags.
    usize storage_size;
};

#define BLOCK_PREVIOUS_IS_FREE_BIT (usize)0x1
#define BLOCK_PREVIOUS_IS_FREE(block) (((block)->storage_size & BLOCK_PREVIOUS_IS_FREE_BIT) != 0)

#define BLOCK_IS_FREE_BIT (usize)0x2
#define BLOCK_IS_FREE(block) (((block)->storage_size & BLOCK_IS_FREE_BIT) != 0)

#define BLOCK_IS_DIRECTLY_SYSTEM_ALLOCATED_BIT (usize)0x4
#define BLOCK_IS_DIRECTLY_SYSTEM_ALLOCATED(block) \
    (((block)->storage_size & BLOCK_IS_DIRECTLY_SYSTEM_ALLOCATED_BIT) != 0)

#define BLOCK_FLAG_BITS \
    (BLOCK_PREVIOUS_IS_FREE_BIT | BLOCK_IS_FREE_BIT | BLOCK_IS_DIRECTLY_SYSTEM_ALLOCATED_BIT)

#define BLOCK_HEADER_SIZE (isize)sizeof(Block)

// Total amount memory occupied by a block, including its header.
// Always a multiple of "ALIGNMENT" to ensure alignment of usable memory stored inside.
#define BLOCK_STORAGE_SIZE(block) (isize)((block)->storage_size & ~BLOCK_FLAG_BITS)

// This must be at least (3 * sizeof(usize)), because free blocks store additional information:
// - Two pointers for the intrusive free-lists right after the header.
// - Storage size of the block at the very end of block's usable memory.
#define BLOCK_MIN_SIZE (3 * sizeof(usize))

// Just the size of the usable memory stored inside of the block.
#define BLOCK_SIZE(block) (BLOCK_STORAGE_SIZE(block) - BLOCK_HEADER_SIZE)

#define BLOCK_MEMORY(block) (void *)((u8 *)(block) + BLOCK_HEADER_SIZE)
#define BLOCK_FROM_MEMORY(memory) ((Block *)((u8 *)(memory) - BLOCK_HEADER_SIZE))

// Accessing these is only allowed when the previous block is marked as free.
// When the previous block is free, it stores its storage size at the end of usable free memory.
#define BLOCK_PREVIOUS_STORAGE_SIZE(block) (*(isize *)((u8 *)(block) - sizeof(isize)))
#define BLOCK_PREVIOUS(block) ((Block *)((u8 *)(block) - BLOCK_PREVIOUS_STORAGE_SIZE(block)))

// Next block is always accessible thanks to the guard block at the end of each region.
#define BLOCK_NEXT(block) ((Block *)((u8 *)(block) + BLOCK_HEADER_SIZE + BLOCK_SIZE(block)))

// Returns a negative value in case of integer overflow.
static inline isize block_size_for_memory_request(isize memory_request) {
    if (memory_request <= BLOCK_MIN_SIZE) {
        return BLOCK_MIN_SIZE;
    }

    isize max_overhead = BLOCK_HEADER_SIZE + (ALIGNMENT - 1);
    if (memory_request > ISIZE_MAX - max_overhead) {
        return -1;
    }

    return ALIGN_UP(BLOCK_HEADER_SIZE + memory_request, ALIGNMENT) - BLOCK_HEADER_SIZE;
}

static inline void block_set_size(Block *block, isize new_size) {
    assert((BLOCK_HEADER_SIZE + new_size) % ALIGNMENT == 0);
    block->storage_size =
        (usize)(BLOCK_HEADER_SIZE + new_size) | (block->storage_size & BLOCK_FLAG_BITS);
}

static inline void block_mark_as_free(Block *block) {
    block->storage_size |= BLOCK_IS_FREE_BIT;

    Block *next_block = BLOCK_NEXT(block);
    next_block->storage_size |= BLOCK_PREVIOUS_IS_FREE_BIT;
    BLOCK_PREVIOUS_STORAGE_SIZE(next_block) = BLOCK_STORAGE_SIZE(block);
}

static inline void block_mark_as_in_use(Block *block) {
    block->storage_size &= ~BLOCK_IS_FREE_BIT;

    Block *next_block = BLOCK_NEXT(block);
    next_block->storage_size &= ~BLOCK_PREVIOUS_IS_FREE_BIT;
}

// Truncates the block to the desired size by chopping a block from its end.
// The returned remainder block is automatically marked as free.
static Block *block_split(Block *block, isize new_size) {
    isize min_storage_size_to_split =
        ALIGN_UP(BLOCK_HEADER_SIZE + new_size, ALIGNMENT) +
        ALIGN_UP(BLOCK_HEADER_SIZE + BLOCK_MIN_SIZE, ALIGNMENT);

    if (BLOCK_STORAGE_SIZE(block) < min_storage_size_to_split) {
        return NULL;
    }

    isize remainder_block_storage_size = BLOCK_STORAGE_SIZE(block) - (BLOCK_HEADER_SIZE + new_size);
    block_set_size(block, new_size);

    Block *remainder_block = (Block *)((u8 *)block + (BLOCK_HEADER_SIZE + new_size));
    remainder_block->storage_size = (usize)remainder_block_storage_size;
    block_mark_as_free(remainder_block);

    if (BLOCK_IS_FREE(block)) {
        remainder_block->storage_size |= BLOCK_PREVIOUS_IS_FREE_BIT;
        BLOCK_PREVIOUS_STORAGE_SIZE(remainder_block) = BLOCK_STORAGE_SIZE(block);
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

    TreeNode null_node_storage;
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
    // Memory available for storing blocks. Guard block is not included.
    u8 *begin;
    u8 *end;

    Region *previous;
    Region *next;
};

// Choose header size such that address of memory inside of the first block is properly aligned.
#define REGION_HEADER_SIZE \
    (isize)(ALIGN_UP(sizeof(Region) + BLOCK_HEADER_SIZE, ALIGNMENT) - BLOCK_HEADER_SIZE)

#define REGION_FIRST_BLOCK(region) ((Block *)((region)->begin))

// A special block stored at the end of each region. Always marked as "in-use".
// This makes looking up the next neighboring block for any other block always safe.
//
// Stores a pointer to the beginning of the region.
// Given the last block within the region, this allows us to tell if it spans the entire region.
#define REGION_GUARD_BLOCK(region) ((Block *)((region)->end))
#define REGION_FROM_GUARD_BLOCK(block) (*(Region **)BLOCK_MEMORY(block))

// Guard block's storage size is marked as 0 to differentiate it from normal blocks.
#define IS_GUARD_BLOCK(block) (BLOCK_STORAGE_SIZE(block) == 0)

// Actual guard block storage includes block header and a pointer to the beginning of the region.
#define GUARD_BLOCK_STORAGE_SIZE (isize)ALIGN_UP(BLOCK_HEADER_SIZE + sizeof(usize), ALIGNMENT)

static Region *region_create_for_memory_request(
    void *user_context,
    SystemAllocate allocate,
    isize memory_request
) {
    memory_request = block_size_for_memory_request(memory_request);
    if (memory_request < 0) {
        return NULL;
    }

    isize max_overhead = REGION_HEADER_SIZE + GUARD_BLOCK_STORAGE_SIZE + (PAGE_SIZE - 1);
    if ((BLOCK_HEADER_SIZE + memory_request) > ISIZE_MAX - max_overhead) {
        return NULL;
    }
    isize region_storage_size = ALIGN_UP(
        REGION_HEADER_SIZE + (BLOCK_HEADER_SIZE + memory_request) + GUARD_BLOCK_STORAGE_SIZE,
        PAGE_SIZE
    );

    Region *new_region = allocate(user_context, region_storage_size);
    if (new_region == NULL) {
        return NULL;
    }

    new_region->begin = (u8 *)new_region + REGION_HEADER_SIZE;
    new_region->end =
        new_region->begin +
        ALIGN_DOWN(region_storage_size - REGION_HEADER_SIZE - GUARD_BLOCK_STORAGE_SIZE, ALIGNMENT);

    new_region->previous = NULL;
    new_region->next = NULL;

    Block *guard_block = REGION_GUARD_BLOCK(new_region);
    guard_block->storage_size = (usize)0;
    REGION_FROM_GUARD_BLOCK(guard_block) = new_region;

    isize first_block_storage_size = new_region->end - new_region->begin;
    Block *first_block = REGION_FIRST_BLOCK(new_region);
    first_block->storage_size = (usize)first_block_storage_size;
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

static void region_list_remove(RegionList *list, Region *region) {
    if (*list == region) {
        *list = region->next;
    }

    if (region->previous != NULL) {
        region->previous->next = region->next;
    }
    if (region->next != NULL) {
        region->next->previous = region->previous;
    }
}

// Must be pinned in memory, so that sentinels stored inside never change their addresses.
struct HeapAllocator {
    RegionList regions;

    // The last region which got emptied. Free block inside is not tracked by any of the free lists.
    // Keep it around just in case the next memory request will need a new region.
    Region *cached_free_region;

    // Circular linked lists represented with sentinel head elements.
    // "free_blocks[N].next" points to the first list element, ".previous" points to the last one.
    ListNode free_blocks[64];
    u64 free_blocks_availability;

    Tree free_block_tree;

    void *user_context;
    SystemAllocate system_allocate;
    SystemDeallocate system_deallocate;
    unsigned int flags;
};

HeapAllocator *heap_allocator_create(
    void *user_context,
    SystemAllocate system_allocate,
    SystemDeallocate system_deallocate,
    unsigned int flags
) {
    if ((flags & SYSTEM_ALLOCATE_IS_CONTIGUOUS) != 0) {
        void *system_heap_base = system_allocate(user_context, 0);
        if (system_heap_base == NULL) {
            return NULL;
        }

        // Align the system heap base, so that all subsequent allocations are properly aligned.
        usize base_alignment = ALIGN_UP(system_heap_base, ALIGNMENT) - (usize)system_heap_base;
        system_allocate(user_context, base_alignment);
    }

    Region *initial_region = region_create_for_memory_request(
        user_context,
        system_allocate,
        sizeof(HeapAllocator)
    );

    Block *initial_block = REGION_FIRST_BLOCK(initial_region);
    block_mark_as_in_use(initial_block);

    HeapAllocator *allocator = BLOCK_MEMORY(initial_block);
    memset(allocator, 0, sizeof(HeapAllocator));

    region_list_prepend(&allocator->regions, initial_region);

    for (int i = 0; i < ARRAY_COUNT(allocator->free_blocks); i += 1) {
        allocator->free_blocks[i].next = &allocator->free_blocks[i];
        allocator->free_blocks[i].previous = &allocator->free_blocks[i];
    }

    TreeNode *tree_null_node = &allocator->free_block_tree.null_node_storage;
    tree_null_node->color = TREE_NODE_BLACK;
    allocator->free_block_tree.null = tree_null_node;
    allocator->free_block_tree.root = tree_null_node;

    allocator->user_context = user_context;
    allocator->system_allocate = system_allocate;
    allocator->system_deallocate = system_deallocate;
    allocator->flags = flags;

    // Trim the allocation used to store allocator struct.
    heap_reallocate(allocator, allocator, sizeof(HeapAllocator));

    return allocator;
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
    assert((BLOCK_HEADER_SIZE + size) % ALIGNMENT == 0 && size >= BLOCK_MIN_SIZE);

    isize min_size = ALIGN_UP(BLOCK_HEADER_SIZE + BLOCK_MIN_SIZE, ALIGNMENT) - BLOCK_HEADER_SIZE;
    return isize_min((size - min_size) / ALIGNMENT, ARRAY_COUNT(allocator->free_blocks));
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
            assert(list != list->next);

            free_block = BLOCK_FROM_LIST_NODE(list->next);

            list_delete(list->next);
            if (list->next == list) {
                allocator->free_blocks_availability &= ~((u64)1 << free_list_index);
            }
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

// Tries to grow a block located at the end of a region by resizing the region.
// Only works when system allocates memory contiguously and we maintain a single growable region.
static Block *heap_allocator_grow_last_block(
    HeapAllocator *allocator,
    Block *block,
    isize block_increment
) {
    assert(IS_GUARD_BLOCK(block) || IS_GUARD_BLOCK(BLOCK_NEXT(block)));

    Region *region = allocator->regions;
    assert(region->next == NULL);
    assert((allocator->flags & SYSTEM_ALLOCATE_IS_CONTIGUOUS) != 0);

    isize region_increment = block_increment;
    if (IS_GUARD_BLOCK(block)) {
        // If we are growing the guard block, make space for the replacement guard block.
        region_increment += GUARD_BLOCK_STORAGE_SIZE;
    }
    region_increment = ALIGN_UP(region_increment, ALIGNMENT);

    void *heap_grow_result = allocator->system_allocate(allocator->user_context, region_increment);
    if (heap_grow_result == NULL) {
        return NULL;
    }

    region->end = region->end + region_increment;

    Block *guard_block = REGION_GUARD_BLOCK(region);
    guard_block->storage_size = (usize)0;
    REGION_FROM_GUARD_BLOCK(guard_block) = region;

    block_set_size(block, region->end - (u8 *)BLOCK_MEMORY(block));
    return block;
}

static inline bool should_use_system_allocate_directly(HeapAllocator const *allocator, isize size) {
    if (size < USE_SYSTEM_ALLOCATE_DIRECTLY_THRESHOLD) {
        return false;
    }

    // Check if the system allocator is capable of releasing memory back to the system.
    return
        (allocator->flags & SYSTEM_ALLOCATE_IS_CONTIGUOUS) == 0 &&
        allocator->system_deallocate != NULL;
}

void *heap_allocate(HeapAllocator *allocator, isize size) {
    if (size == 0) {
        return NULL;
    }

    if (should_use_system_allocate_directly(allocator, size)) {
        Region *new_region = region_create_for_memory_request(
            allocator->user_context,
            allocator->system_allocate,
            size
        );
        if (new_region == NULL) {
            return NULL;
        }

        region_list_prepend(&allocator->regions, new_region);

        Block *new_block = REGION_FIRST_BLOCK(new_region);
        block_mark_as_in_use(new_block);
        new_block->storage_size |= BLOCK_IS_DIRECTLY_SYSTEM_ALLOCATED_BIT;

        return BLOCK_MEMORY(new_block);
    }

    size = block_size_for_memory_request(size);
    if (size < 0) {
        return NULL;
    }

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

    // Try to use a cached free region.

    if (allocator->cached_free_region != NULL) {
        Block *cached_block = REGION_FIRST_BLOCK(allocator->cached_free_region);
        if (size < BLOCK_SIZE(cached_block)) {
            allocator->cached_free_region = NULL;

            Block *remainder_block = block_split(cached_block, size);
            if (remainder_block != NULL) {
                heap_allocator_free_list_add(allocator, remainder_block);
            }

            block_mark_as_in_use(cached_block);
            return BLOCK_MEMORY(cached_block);
        }
    }

    // Allocate a new region otherwise.

    Region *new_region = region_create_for_memory_request(
        allocator->user_context,
        allocator->system_allocate,
        size
    );
    if (new_region == NULL) {
        return NULL;
    }
    region_list_prepend(&allocator->regions, new_region);

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

    if (BLOCK_IS_DIRECTLY_SYSTEM_ALLOCATED(block)) {
        Region *region = (Region *)((u8 *)block - REGION_HEADER_SIZE);
        region_list_remove(&allocator->regions, region);
        allocator->system_deallocate(allocator->user_context, region);
        return;
    }

    Block *merged_block = block;
    isize merged_block_size = BLOCK_SIZE(block);

    if (BLOCK_PREVIOUS_IS_FREE(block)) {
        Block *previous_block = BLOCK_PREVIOUS(block);
        merged_block = previous_block;
        merged_block_size += BLOCK_HEADER_SIZE + BLOCK_SIZE(previous_block);
        heap_allocator_free_list_remove(allocator, previous_block);
    }

    if (BLOCK_IS_FREE(BLOCK_NEXT(block))) {
        Block *next_block = BLOCK_NEXT(block);
        merged_block_size += BLOCK_HEADER_SIZE + BLOCK_SIZE(next_block);
        heap_allocator_free_list_remove(allocator, next_block);
    }

    block_set_size(merged_block, merged_block_size);
    block_mark_as_free(merged_block);

    // Check if the (free) block we ended up with spans the entire region.
    if (allocator->system_deallocate != NULL && IS_GUARD_BLOCK(BLOCK_NEXT(merged_block))) {
        Block *guard_block = BLOCK_NEXT(merged_block);
        Region *region = REGION_FROM_GUARD_BLOCK(guard_block);

        if (REGION_FIRST_BLOCK(region) == merged_block) {
            Region *old_cached_region = allocator->cached_free_region;
            allocator->cached_free_region = region;

            if (old_cached_region != NULL) {
                region_list_remove(&allocator->regions, old_cached_region);
                allocator->system_deallocate(allocator->user_context, old_cached_region);
            }

            return;
        }
    }

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

    Block *block = BLOCK_FROM_MEMORY(memory);
    isize old_size = BLOCK_SIZE(block);

    if (
        BLOCK_IS_DIRECTLY_SYSTEM_ALLOCATED(block) ||
        should_use_system_allocate_directly(allocator, new_size)
    ) {
        void *new_memory = heap_allocate(allocator, new_size);
        if (new_memory == NULL) {
            return NULL;
        }

        memcpy(new_memory, memory, isize_min(old_size, new_size));
        heap_deallocate(allocator, memory);

        return new_memory;
    }

    new_size = block_size_for_memory_request(new_size);
    if (new_size < 0) {
        return NULL;
    }

    if (new_size == old_size) {
        return BLOCK_MEMORY(block);
    }

    // Handle shrinking the block.

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

            // "memcpy" will handle copying backwards even for overlaping regions.
            memcpy(BLOCK_MEMORY(merged_block), memory, old_size);

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
        memcpy(new_memory, memory, old_size);
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
    memcpy(new_memory, memory, old_size);
    heap_deallocate(allocator, memory);

    return new_memory;
}

void *heap_allocate_zeroed(HeapAllocator *allocator, isize size) {
    void *memory = heap_allocate(allocator, size);
    if (memory != NULL) {
        bool is_already_zeroed =
            should_use_system_allocate_directly(allocator, size) &&
            (allocator->flags & SYSTEM_ALLOCATE_ZEROES_MEMORY) != 0;

        if (!is_already_zeroed) {
            isize block_size = BLOCK_SIZE(BLOCK_FROM_MEMORY(memory));
            memset(memory, 0, block_size);
        }
    }

    return memory;
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
        if (IS_GUARD_BLOCK(next_block)) {
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
