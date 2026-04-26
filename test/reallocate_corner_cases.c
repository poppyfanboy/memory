#include "test_common.c"

void *heap_reallocate_logged(HeapAllocator *allocator, void *memory, isize new_size) {
    void *result = heap_reallocate(allocator, memory, new_size);
    printf("heap.reallocate(%td): [0x%p] -> [0x%p]\n", new_size, memory, result);
    heap_dump(allocator);
    printf("\n");
    return result;
}

int main(void) {
    HeapAllocator *allocator = heap_allocator();

    printf("Initial state\n");
    heap_dump(allocator);
    printf("\n");

    void *first = heap_reallocate_logged(allocator, NULL, 256 + 8);
    void *second = heap_reallocate_logged(allocator, NULL, 64000 + 8);

    // Now the "second" block is placed between the two free ones.
    // The one to the right is not large enough to satisfy the reallocation request.
    heap_reallocate_logged(allocator, first, 0);

    second = heap_reallocate_logged(allocator, second, (64000 + 8) + 256);
    second = heap_reallocate_logged(allocator, second, 256 + 8);
    second = heap_reallocate_logged(allocator, second, 512000 + 8);

    heap_allocator_destroy(allocator);
    return 0;
}
