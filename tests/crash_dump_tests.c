#include "crash_dump.h"
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

static void check(int ok, const char *what) {
    ++checks;
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static int starts_with_mdmp(const char *path) {
    FILE *f = fopen(path, "rb");
    char sig[4] = {0};
    if (!f) return 0;
    size_t n = fread(sig, 1, 4, f);
    fclose(f);
    return n == 4 && memcmp(sig, "MDMP", 4) == 0;
}

static void test_dump_paths(void) {
    char out[256];
    check(jpt_crash_dump_path(out, sizeof(out), "C:\\logs\\trespasser_recomp_294.log", 1) == 1, "attempt 1 ok");
    check(strcmp(out, "C:\\logs\\trespasser_recomp_294.dmp") == 0, "attempt 1 path");
    check(jpt_crash_dump_path(out, sizeof(out), "C:\\logs\\trespasser_recomp_294.log", 2) == 1, "attempt 2 ok");
    check(strcmp(out, "C:\\logs\\trespasser_recomp_294_2.dmp") == 0, "attempt 2 path");
    check(jpt_crash_dump_path(out, sizeof(out), "C:\\logs\\trespasser_recomp_294.log", 3) == 1, "attempt 3 ok");
    check(strcmp(out, "C:\\logs\\trespasser_recomp_294_3.dmp") == 0, "attempt 3 path");
    check(jpt_crash_dump_path(out, 10, "C:\\logs\\trespasser_recomp_294.log", 1) == 0, "small buffer rejected");
    check(jpt_crash_dump_path(out, sizeof(out), "C:\\dir.with.dot\\noext", 1) == 0, "no extension rejected");
    check(jpt_crash_dump_path(NULL, sizeof(out), "a.log", 1) == 0, "null out rejected");
    check(jpt_crash_dump_path(out, sizeof(out), NULL, 1) == 0, "null log_path rejected");
}

static void test_write_minidump(void) {
    char temp_dir[MAX_PATH] = {0};
    char log_file[MAX_PATH] = {0};
    char dmp1[MAX_PATH] = {0};
    char dmp2[MAX_PATH] = {0};
    GetTempPathA(sizeof(temp_dir), temp_dir);
    _snprintf(log_file, sizeof(log_file) - 1, "%sjpt_crash_test_%lu.log", temp_dir, GetCurrentProcessId());
    check(host_log_open(NULL, log_file) == 1, "host_log_open");
    check(jpt_crash_dump_install() == 1, "jpt_crash_dump_install");

    EXCEPTION_RECORD rec;
    CONTEXT ctx;
    memset(&rec, 0, sizeof(rec));
    memset(&ctx, 0, sizeof(ctx));
    rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    ctx.ContextFlags = CONTEXT_FULL;
    RtlCaptureContext(&ctx);
    EXCEPTION_POINTERS ep = {&rec, &ctx};

    check(jpt_crash_dump_write(&ep, GetCurrentThreadId()) == 1, "first dump written");
    jpt_crash_dump_path(dmp1, sizeof(dmp1), log_file, 1);
    check(starts_with_mdmp(dmp1), "first dump has MDMP signature");

    check(jpt_crash_dump_write(&ep, GetCurrentThreadId()) == 1, "second dump written without overwrite");
    jpt_crash_dump_path(dmp2, sizeof(dmp2), log_file, 2);
    check(starts_with_mdmp(dmp2), "second dump has _2.dmp MDMP signature");

    DeleteFileA(dmp1);
    DeleteFileA(dmp2);
    DeleteFileA(log_file);
}

int main(void) {
    test_dump_paths();
    test_write_minidump();
    if (failures) {
        fprintf(stderr, "crash_dump_tests: %d checks, %d failure(s)\n", checks, failures);
        return 1;
    }
    printf("crash_dump_tests: %d checks, 0 failures\n", checks);
    return 0;
}
