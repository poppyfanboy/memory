#include "../src/memory.c"

HeapAllocator *heap_allocator(void) {
    extern void *system_allocate(void *user_context, isize size);
    return heap_allocator_create(NULL, system_allocate, NULL, SYSTEM_ALLOCATE_IS_CONTIGUOUS);
}

HeapIterator *heap_iterator(void) {
    static HeapIterator heap_iterator;
    memset(&heap_iterator, 0, sizeof(heap_iterator));
    return &heap_iterator;
}
