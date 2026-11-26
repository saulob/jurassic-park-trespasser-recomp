#include "jpt_alloc.h"

#include <stdlib.h>

static long s_fail_at;

static int allowed(size_t count, size_t size, size_t* bytes) {
    if (s_fail_at > 0 && --s_fail_at == 0) s_fail_at = 1;
    if (s_fail_at == 1) return 0;
    if (size && count > JPT_ALLOC_MAX / size) return 0;
    *bytes = count && size ? count * size : 1;
    return 1;
}

void* jpt_alloc(size_t count, size_t size) {
    size_t bytes;
    return allowed(count, size, &bytes) ? malloc(bytes) : NULL;
}

void* jpt_alloc_zero(size_t count, size_t size) {
    size_t bytes;
    return allowed(count, size, &bytes) ? calloc(1, bytes) : NULL;
}

void* jpt_alloc_again(void* p, size_t count, size_t size) {
    size_t bytes;
    return allowed(count, size, &bytes) ? realloc(p, bytes) : NULL;
}

void jpt_alloc_fail_at(long n) { s_fail_at = n > 0 ? n + 1 : 0; }
