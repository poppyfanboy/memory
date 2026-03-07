#include <assert.h>

#include "test_common.c"

#define COUNT 10000

int main(void) {
    HeapAllocator *allocator = heap_allocator_create(system_allocate, system_deallocate);
    assert(allocator != NULL);

    int **values = heap_allocate(allocator, COUNT * sizeof(*values));
    assert(values != NULL);

    for (int i = 0; i < COUNT; i += 1) {
        values[i] = heap_allocate(allocator, sizeof(values[i]));
        assert(values[i] != NULL);

        *values[i] = 42;
    }

    for (int i = 0; i < COUNT; i += 10) {
        assert(*values[i] == 42);
        heap_deallocate(allocator, values[i]);
    }

    heap_allocator_destroy(allocator);
    return 0;
}
