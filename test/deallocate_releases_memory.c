#include "test_common.c"

#define ARRAY_COUNT(array) ((int)sizeof(array) / (int)sizeof((array)[0]))

int main(void) {
    HeapAllocator *allocator = heap_allocator();

    // Two rounds to check if the leftover cached region is getting used in the second round.
    for (int round = 0; round < 2; round += 1) {
        void *allocations[32];
        for (int i = 0; i < ARRAY_COUNT(allocations); i += 1) {
            allocations[i] = heap_allocate(allocator, 32000);
        }

        printf("After allocations\n");
        heap_dump(allocator);
        printf("\n");

        for (int i = 0; i < ARRAY_COUNT(allocations); i += 1) {
            heap_deallocate(allocator, allocations[i]);
        }

        printf("After deallocations\n");
        heap_dump(allocator);
        printf("\n");
    }

    heap_allocator_destroy(allocator);
    return 0;
}
