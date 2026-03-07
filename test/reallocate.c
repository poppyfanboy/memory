#include <assert.h>

#include "test_common.c"

#define COUNT 25000

typedef struct {
    int *values;
    int count;
    int capacity;
} Array;

void array_push(Array *array, int value, HeapAllocator *allocator) {
    if (array->count == array->capacity) {
        int new_capacity = array->capacity > 0 ? 3 * array->capacity / 2 : 16;

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
    HeapAllocator *allocator = heap_allocator_create(system_allocate, system_deallocate);
    assert(allocator != NULL);

    Array array = {0};
    for (int i = 0; i < COUNT; i += 1) {
        array_push(&array, i, allocator);
    }

    heap_dump(allocator);

    for (int i = 0; i < array.count; i += 1) {
        assert(array.values[i] == i);
    }

    array.values = heap_reallocate(allocator, array.values, 1 * sizeof(*array.values));
    array.capacity = 1;
    array.count = 1;
    assert(array.values[0] == 0);

    array.values = heap_reallocate(allocator, array.values, 0);
    assert(array.values == NULL);

    heap_allocator_destroy(allocator);
    return 0;
}
