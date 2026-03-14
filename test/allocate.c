#include <assert.h>

#include "test_common.c"

#define STRING_MIN_SIZE 1
#define STRING_MAX_SIZE 8192
#define STRING_COUNT 4096

static inline char char_advance(char current_char, int increment) {
    int next_char_index = (current_char - 'a') + increment;
    next_char_index %= ('z' - 'a');
    return 'a' + next_char_index;
}

typedef struct {
    char starting_char;
    int count;
    char *data;
} String;

void string_fill(String *string) {
    int increment = (uintptr_t)string->data & 0x7fffffff;

    char current_char = string->starting_char;
    for (int i = 0; i < string->count; i += 1) {
        string->data[i] = current_char;
        current_char = char_advance(current_char, increment);
    }
}

void string_validate(String const *string) {
    int increment = (uintptr_t)string->data & 0x7fffffff;

    char current_char = string->starting_char;
    for (int i = 0; i < string->count; i += 1) {
        assert(string->data[i] == current_char);
        current_char = char_advance(current_char, increment);
    }
}

static inline double float_random(PCG32 *rng) {
    uint32_t random_int = pcg32_random(rng);
    return random_int * 0x1p-32;
}

static inline int int_random(PCG32 *rng, int min, int max) {
    double t = float_random(rng);
    return min + t * (max - min + 1);
}

static inline char char_random(PCG32 *rng) {
    return int_random(rng, 'a', 'z');
}

int main(void) {
    PCG32 rng;
    pcg32_initialize(&rng, 42);

    #ifdef USE_SYSTEM_HEAP
    HeapAllocator *allocator = heap_allocator_from_system_heap(system_heap_grow);
    #else
    HeapAllocator *allocator = heap_allocator_create(system_allocate, system_deallocate);
    #endif
    assert(allocator != NULL);

    String *strings = heap_allocate(allocator, STRING_COUNT * sizeof(String));
    assert(strings != NULL);

    String *string_iter = strings;
    while (string_iter < strings + STRING_COUNT) {
        string_iter->starting_char = char_random(&rng);
        string_iter->count = int_random(&rng, STRING_MIN_SIZE, STRING_MAX_SIZE);

        string_iter->data = heap_allocate(allocator, string_iter->count);
        assert(string_iter->data != NULL);
        string_fill(string_iter);

        string_iter += 1;
    }

    for (int i = 0; i < STRING_COUNT; i += 1) {
        string_validate(&strings[i]);
    }

    // Deallocate every second string and re-validate.
    for (int i = 0; i < STRING_COUNT; i += 2) {
        heap_deallocate(allocator, strings[i].data);
    }
    for (int i = 1; i < STRING_COUNT; i += 2) {
        string_validate(&strings[i]);
    }

    heap_allocator_destroy(allocator);
    return 0;
}
