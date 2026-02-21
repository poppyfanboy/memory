#include <stdlib.h>
#include <assert.h>

#include "../src/memory.h"

int main(void) {
    HeapAllocator *allocator = heap_allocator_create();
    assert(allocator != NULL);

    int *value = heap_allocate(allocator, sizeof(*value));
    assert(value != NULL);

    *value = 42;

    heap_deallocate(allocator, value);

    heap_allocator_destroy(allocator);
    return 0;
}
