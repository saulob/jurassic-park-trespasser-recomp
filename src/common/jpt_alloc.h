/* Memory whose size comes from a level's files: counts are multiplied without overflow and one piece is capped. free() releases it. */
#ifndef JPT_ALLOC_H
#define JPT_ALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JPT_ALLOC_MAX ((size_t)512 << 20)

void* jpt_alloc(size_t count, size_t size);
void* jpt_alloc_zero(size_t count, size_t size);
void* jpt_alloc_again(void* p, size_t count, size_t size);
/* Tests: the n-th call from now fails, and every one after it; 0: none does. */
void jpt_alloc_fail_at(long n);

#ifdef __cplusplus
}
#endif

#endif
