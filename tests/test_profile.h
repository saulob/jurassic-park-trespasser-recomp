/* The profile every host-code test reads guest addresses of: English 1.0.116 (lift116), as the retail executable the
 * tests read. */
#ifndef JPT_TEST_PROFILE_H
#define JPT_TEST_PROFILE_H

#include <stdio.h>
#include <stdlib.h>

#include "active_profile.h"

static void test_profile_activate(void) {
    char error[256];
    if (!jpt_profile_activate(jpt_game_version_by_id("en-116"), error, sizeof error)) {
        fprintf(stderr, "cannot activate en-116: %s\n", error);
        exit(2);
    }
}

#endif
