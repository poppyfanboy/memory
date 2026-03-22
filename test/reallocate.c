#include <assert.h>
#include "test_common.c"

int main(void) {
    HeapAllocator *allocator = heap_allocator();

    void *first = heap_allocate(allocator, 256);
    assert(first != NULL);

    heap_dump(allocator);
    printf("\n");

    void *second = heap_allocate(allocator, 260992);
    assert(second != NULL);

    // Now the "second" block is placed between the two free ones.
    // The one to the right is not large enough to satisfy the reallocation request.
    heap_deallocate(allocator, first);

    heap_dump(allocator);
    printf("\n");

    second = heap_reallocate(allocator, second, 260992 + 256);
    assert(second != NULL);

    printf("After heap_reallocate(%d)\n", 260992 + 256);
    heap_dump(allocator);
    printf("\n");

    second = heap_reallocate(allocator, second, 256);

    printf("After heap_reallocate(%d)\n", 256);
    heap_dump(allocator);
    printf("\n");

    second = heap_reallocate(allocator, second, 510 * 1024);

    printf("After heap_reallocate(%d)\n", 510 * 1024);
    heap_dump(allocator);
    printf("\n");

    heap_allocator_destroy(allocator);
    return 0;
}
