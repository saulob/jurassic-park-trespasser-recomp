#include "jpt_hammond.h"

#include <stdio.h>
#include <stdlib.h>

#include "jpt_hash.h"

#define NUMBER_MAX 299
#define SUFFIXES "abcd"

static uint32_t s_ids[(NUMBER_MAX + 1) * (1 + sizeof(SUFFIXES) - 1)];
static size_t s_count;

static int compare(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

static void build(void) {
    char name[16];
    if (s_count) return;
    for (int n = 0; n <= NUMBER_MAX; n++) {
        snprintf(name, sizeof name, "VH%d", n);
        s_ids[s_count++] = jpt_sample_id(name);
        for (const char* s = SUFFIXES; *s; s++) {
            snprintf(name, sizeof name, "VH%d%c", n, *s);
            s_ids[s_count++] = jpt_sample_id(name);
        }
    }
    qsort(s_ids, s_count, sizeof s_ids[0], compare);
}

int jpt_hammond_sample(uint32_t id) {
    build();
    return bsearch(&id, s_ids, s_count, sizeof s_ids[0], compare) != NULL;
}
