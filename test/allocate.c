#include <assert.h>

#include "../src/memory.h"

#define COUNT 10000

int main(void) {
    HeapAllocator *allocator = heap_allocator_create();
    assert(allocator != NULL);

    int **values = heap_allocate(allocator, COUNT * sizeof(*values));

    for (int i = 0; i < COUNT; i += 1) {
        values[i] = heap_allocate(allocator, sizeof(values[i]));
        assert(values[i] != NULL);

        *values[i] = 42;
    }

    for (int i = 0; i < COUNT; i += 10) {
        heap_deallocate(allocator, values[i]);
    }

    heap_allocator_destroy(allocator);
    return 0;
}
