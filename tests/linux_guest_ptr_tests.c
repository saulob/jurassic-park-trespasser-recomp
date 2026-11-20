/* A guest address must never work as a host pointer: the guest window sits far above the 32-bit range, nothing of the process
 * is mapped where the guest's addresses would land if they were used as pointers, and the code that converts between the two
 * refuses the mistakes (a host pointer stored into guest memory, a 64-bit value squeezed into 32 bits). Headless; Linux only. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fault_guard.h"
#include "guest_memory.h"
#include "guest_ptr.h"
#include "guest_thunks.h"

static int g_checks, g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        g_checks++;                                                                                                     \
        if (!(cond)) {                                                                                                  \
            g_failures++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

static int low_range_is_unmapped(uint64_t lo, uint64_t hi, char* offender, size_t cap) {
    FILE* f = fopen("/proc/self/maps", "r");
    char line[512];
    int clean = 1;
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        uint64_t a, b;
        if (sscanf(line, "%" SCNx64 "-%" SCNx64, &a, &b) == 2 && a < hi && b > lo) {
            snprintf(offender, cap, "%s", line);
            clean = 0;
            break;
        }
    }
    fclose(f);
    return clean;
}

static void test_window_is_far_from_low_addresses(void) {
    char offender[512] = "";
    uint64_t base = (uint64_t)(uintptr_t)gm_base();
    int clean;
    CHECK(base >= 0x100000000ull, "the guest window starts above 4 GB (0x%" PRIx64 "), so no guest address is also a host address", base);
    clean = low_range_is_unmapped(0x1000, 0x80000000ull, offender, sizeof offender);
    if (clean < 0) {
        printf("  /proc/self/maps unreadable: guest-range check skipped\n");
        return;
    }
    CHECK(clean == 1, "nothing of this process is mapped in 0x1000-0x7FFFFFFF: %s", offender);
}

static void test_raw_guest_addresses_fault(void) {
    static const uint32_t samples[] = { 0x00010000u, 0x00400000u, 0x0063B000u, 0x01000000u, 0x10000000u, 0x7F000000u };
    uint32_t heap = gm_heap_alloc(64, 1);
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++)
        CHECK(faults_on_read((const volatile uint8_t*)(uintptr_t)samples[i]), "reading host address 0x%08X faults", samples[i]);
    CHECK(heap != 0 && faults_on_read((const volatile uint8_t*)(uintptr_t)heap), "a guest heap block's address is not a readable host address (0x%08X)", heap);
    CHECK(!faults_on_read((const volatile uint8_t*)gm_ptr(heap, 4)), "but through the window it reads");
    CHECK(gm_ptr(heap, 4) != (void*)(uintptr_t)heap, "gm_ptr never returns the guest address itself");
    gm_heap_free(heap);
}

static void test_conversions_refuse_mistakes(void) {
    uint32_t slot = gm_heap_alloc(16, 1);
    int host_local = 0;
    uint64_t before = gp_leak_count();
    CHECK(!gp_store_ptr(slot, &host_local) && gp_leak_count() == before + 1, "a host pointer is not stored into guest memory");
    CHECK(!gp_store_u32(slot, (uint64_t)(uintptr_t)&host_local) && gp_leak_count() == before + 2, "nor a host address squeezed into 32 bits");
    CHECK(gm_va(&host_local) == 0, "a host address has no guest address");
    CHECK(gm_va(gm_ptr(slot, 4)) == slot, "a guest address survives the round trip");
    CHECK(gm_ptr(0x00001000u, 4) == NULL, "the null guard page has no host pointer");
    CHECK(gm_ptr(0xFFFFFFFCu, 8) == NULL, "a range past 4 GB has none");
    gm_heap_free(slot);
}

int main(void) {
    if (!gm_init(0, 0) || !ht_init()) {
        printf("cannot reserve the guest window\n");
        return 2;
    }
    test_window_is_far_from_low_addresses();
    test_raw_guest_addresses_fault();
    test_conversions_refuse_mistakes();
    gm_shutdown();
    printf("linux_guest_ptr_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
