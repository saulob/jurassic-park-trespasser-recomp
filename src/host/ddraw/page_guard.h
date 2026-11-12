#ifndef TRESPASSER_PAGE_GUARD_H
#define TRESPASSER_PAGE_GUARD_H

#include <stddef.h>

/* One buffer at a time made inaccessible until its first access. The access faults, the pages become
 * accessible again and touched(context) runs on the faulting thread before the access is retried. */
typedef void (*page_guard_touched_t)(void* context);

typedef enum { PAGE_GUARD_ARMED, PAGE_GUARD_SHARED_PAGES, PAGE_GUARD_PROTECT_FAILED, PAGE_GUARD_NO_HANDLER } page_guard_result_t;

/* bits must start an allocation of its own (nothing else may share its pages). */
page_guard_result_t page_guard_arm(void* bits, size_t bytes, page_guard_touched_t touched, void* context);
/* The buffer is accessible again; 1 when it was still untouched (touched will not run). */
int page_guard_disarm(void);
/* As a first access would: the pages open and touched runs now. */
void page_guard_touch(void);
int page_guard_armed(void);

/* A second view of a section's pages that stays inaccessible while no Lock uses it: arming hands it out and a Lock
 * that ends untouched closes nothing, so neither costs a protection change. Its first access opens it (touched runs
 * first, as above); an access while no Lock waits only opens it, so a pointer kept past Unlock keeps working.
 * Returns an id (0: none). */
int page_alias_create(void* section, size_t bytes);
void page_alias_destroy(int alias);
unsigned char* page_alias_view(int alias);
page_guard_result_t page_guard_arm_alias(int alias, page_guard_touched_t touched, void* context);

#endif
