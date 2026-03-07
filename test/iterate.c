#include <assert.h>

#include "test_common.c"

int main(void) {
    HeapAllocator *allocator = heap_allocator_create(system_allocate, system_deallocate);
    assert(allocator != NULL);

    void *a6 = heap_allocate(allocator, 1024);
    assert(a6 != NULL);
    heap_deallocate(allocator, a6);

    void *a5 = heap_allocate(allocator, 512);
    assert(a5 != NULL);

    void *a4 = heap_allocate(allocator, 256);
    assert(a4 != NULL);

    void *a3 = heap_allocate(allocator, 128);
    assert(a3 != NULL);
    heap_deallocate(allocator, a3);

    void *a2 = heap_allocate(allocator, 64);
    assert(a2 != NULL);

    void *a1 = heap_allocate(allocator, 32);
    assert(a1 != NULL);
    heap_deallocate(allocator, a1);

    heap_dump(allocator);

    heap_allocator_destroy(allocator);
    return 0;
}
