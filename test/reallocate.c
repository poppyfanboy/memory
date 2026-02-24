#include <assert.h>

#include "../src/memory.h"

typedef struct {
    int *values;
    int count;
    int capacity;
} Array;

void array_push(Array *array, int value, HeapAllocator *allocator) {
    if (array->count == array->capacity) {
        int new_capacity = array->capacity > 0 ? 2 * array->capacity : 16;

        array->values = heap_reallocate(
            allocator,
            array->values,
            new_capacity * sizeof(*array->values)
        );
        assert(array->values != NULL);

        array->capacity = new_capacity;
    }

    array->values[array->count] = value;
    array->count += 1;
}

int main(void) {
    HeapAllocator *allocator = heap_allocator_create();
    assert(allocator != NULL);

    Array array = {0};
    for (int i = 0; i < 1000; i += 1) {
        array_push(&array, i, allocator);
    }

    for (int i = 0; i < array.count; i += 1) {
        assert(array.values[i] == i);
    }

    heap_allocator_destroy(allocator);
    return 0;
}
