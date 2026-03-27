#include <assert.h>
#include <stdlib.h>

#include "test_common.c"

#define SEED 42
#define OPERATION_COUNT 10000

#define STRING_MIN_SIZE 1
#define STRING_MAX_SIZE 500000

// Each "VALIDATION_PERIOD" operations the test program will go through all of the strings allocated
// at the moment and check if they contain what we've put into them initially.
#define VALIDATION_PERIOD 1000

typedef struct {
    char *chars;
    int char_count;

    char starting_char;
    int char_increment;
} String;

static inline char char_advance(char current_char, int increment) {
    int next_char_index = (current_char - 'a') + increment;
    next_char_index %= ('z' - 'a');
    return 'a' + next_char_index;
}

void string_fill(String *string, char starting_char, int char_increment) {
    string->starting_char = starting_char;
    string->char_increment = char_increment;

    char current_char = starting_char;
    for (int i = 0; i < string->char_count; i += 1) {
        string->chars[i] = current_char;
        current_char = char_advance(current_char, char_increment);
    }
}

void string_validate(String const *string) {
    char current_char = string->starting_char;
    for (int i = 0; i < string->char_count; i += 1) {
        assert(string->chars[i] == current_char);
        current_char = char_advance(current_char, string->char_increment);
    }
}

typedef struct {
    String *data;
    int count;
    int capacity;
} StringList;

void string_list_append(StringList *strings, String new_string) {
    if (strings->count == strings->capacity) {
        int new_capacity = 2 * strings->capacity;
        if (new_capacity < 16) {
            new_capacity = 16;
        }

        strings->data = realloc(strings->data, new_capacity * sizeof(*strings->data));
        strings->capacity = new_capacity;
    }

    strings->data[strings->count] = new_string;
    strings->count += 1;
}

void string_list_remove_at(StringList *strings, int index) {
    strings->data[index] = strings->data[strings->count - 1];
    strings->count -= 1;
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
    pcg32_initialize(&rng, SEED);

    HeapAllocator *allocator = heap_allocator();
    assert(allocator != NULL);

    StringList strings = {0};

    for (int operation_index = 0; operation_index < OPERATION_COUNT; operation_index += 1) {
        enum {
            ALLOCATE,
            REALLOCATE,
            DEALLOCATE,
        };

        int operation;
        float operation_dice_roll = float_random(&rng);
        if (operation_dice_roll < 0.5F) {
            operation = ALLOCATE;
        } else if (operation_dice_roll < 0.75F) {
            operation = REALLOCATE;
        } else {
            operation = DEALLOCATE;
        }

        if (strings.count == 0 && (operation == REALLOCATE || operation == DEALLOCATE)) {
            operation = ALLOCATE;
        }

        switch (operation) {
            case ALLOCATE: {
                String string;
                string.char_count = int_random(&rng, STRING_MIN_SIZE, STRING_MAX_SIZE);
                string.chars = heap_allocate(allocator, string.char_count);
                assert(string.chars != NULL);

                char starting_char = char_random(&rng);
                int char_increment = int_random(&rng, 0, 25);
                string_fill(&string, starting_char, char_increment);

                string_list_append(&strings, string);
            } break;

            case REALLOCATE: {
                assert(strings.count > 0);
                int string_index = int_random(&rng, 0, strings.count - 1);
                String *string = &strings.data[string_index];

                int new_char_count = int_random(&rng, STRING_MIN_SIZE, STRING_MAX_SIZE);
                string->char_count = new_char_count;
                string->chars = heap_reallocate(allocator, string->chars, new_char_count);
                assert(string->chars != NULL);

                string_fill(string, string->starting_char, string->char_increment);
            } break;

            case DEALLOCATE: {
                assert(strings.count > 0);
                int string_index = int_random(&rng, 0, strings.count - 1);
                String *string = &strings.data[string_index];

                string_validate(string);
                heap_deallocate(allocator, string->chars);

                string_list_remove_at(&strings, string_index);
            } break;
        }

        if (operation_index + 1 == VALIDATION_PERIOD) {
            for (int i = 0; i < strings.count; i += 1) {
                string_validate(&strings.data[i]);
            }
        }
    }

    for (int i = 0; i < strings.count; i += 1) {
        string_validate(&strings.data[i]);
    }
    heap_allocator_destroy(allocator);
    return 0;
}
