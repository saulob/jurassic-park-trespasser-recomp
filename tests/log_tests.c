/*
 * Headless checks of the log's performance policy: without --perf-log a run's log holds no statistics, with it all of them
 * are there, and the lines that report a state or a failure are always written. Writes only a scratch file.
 * Build: CMake target log_tests.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

static int s_failures, s_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        s_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            s_failures++;                                                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
        }                                                                                                              \
    } while (0)

static char* read_all(const char* path) {
    FILE* f = fopen(path, "rb");
    long n;
    char* text;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = (char*)calloc((size_t)n + 1, 1);
    if (text && fread(text, 1, (size_t)n, f) != (size_t)n) text[0] = 0;
    fclose(f);
    return text;
}

static void emit_run(void) {
    host_log("d3d", "failed_d3d_calls %d", 0);
    host_log("renderer", "selected %s", "d3d11");
    host_log("perf-detail", "frame breakdown %d", 1);
    host_log_perf("renderer", "summary at exit:");
    host_log_perf("video", "water refraction: %d water batches", 7);
    host_log_perf("selfmod", "first native entry at 0x%08X", 0x63C4D0u);
    host_log("exit", "exit code %d", 0);
}

static char* run(const char* path, int perf) {
    remove(path);
    CHECK(host_log_open(".", path));
    if (perf) host_perf_logging_enable();
    emit_run();
    fflush(stderr);
    return read_all(path);
}

/* The producers of the statistics call the gated writers, and F9's snapshot does not depend on --perf-log. */
static char* read_source(const char* rel) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", JPT_SOURCE_DIR, rel);
    return read_all(path);
}

static int line_uses(const char* text, const char* needle, const char* writer) {
    const char* at = text ? strstr(text, needle) : NULL;
    const char* start = at;
    while (start && start > text && start[-1] != '\n') start--;
    return start && !strncmp(start + strspn(start, " \t"), writer, strlen(writer));
}

static void test_producers(void) {
    char* renderer = read_source("src/host/ddraw/d3d11_renderer.c");
    char* watch = read_source("src/host/physics_watch.c");
    char* limits = read_source("src/host/engine_limits.c");
    CHECK(renderer && watch && limits);
    if (renderer) {
        const char* summary = strstr(renderer, "void d3dr_summary(void)");
        CHECK(summary && strstr(summary, "host_perf_logging()") &&
              strstr(summary, "host_perf_logging()") < strstr(summary, "log_metric_lines(\"summary at exit:\")"));
        CHECK(line_uses(renderer, "\"water refraction: %llu water batches", "host_log_perf(") &&
              line_uses(renderer, "\"water refraction: largest wave difference", "host_log_perf(") &&
              line_uses(renderer, "\"CPU pixels composited over the GPU target", "host_log_perf("));
        CHECK(strstr(summary, "failed_d3d_calls %lu") != NULL);
    }
    if (limits)
        CHECK(line_uses(limits, "\"guest hardware renderer accepted %u times, never rejected", "host_log_perf(") &&
              line_uses(limits, "\"guest hardware renderer accepted %u times, rejected", "host_log("));
    if (watch) {
        const char* request = strstr(watch, "if (W.requested) {");
        CHECK(request && strstr(request, "log_snapshot(&s);") &&
              strstr(request, "log_snapshot(&s);") < strstr(request, "log_automatic_snapshot(&s)"));
        CHECK(strstr(watch, "static void log_automatic_snapshot(const pw_snapshot_t* s) {\n    if (host_perf_logging()) log_snapshot(s);") ||
              strstr(watch, "static void log_automatic_snapshot(const pw_snapshot_t* s) {\r\n    if (host_perf_logging()) log_snapshot(s);"));
        CHECK(line_uses(watch, "\"F9: snapshot at the next gameplay frame\"", "host_log("));
    }
    free(renderer);
    free(watch);
    free(limits);
}

int main(void) {
    char path[MAX_PATH];
    char* text;
    snprintf(path, sizeof path, "%s\\log_tests_%lu.log", getenv("TEMP") ? getenv("TEMP") : ".", GetCurrentProcessId());
    CHECK(!host_perf_logging());
    text = run(path, 0);
    CHECK(text != NULL);
    if (text) {
        CHECK(strstr(text, "failed_d3d_calls 0") && strstr(text, "selected d3d11") && strstr(text, "exit code 0"));
        CHECK(!strstr(text, "summary at exit") && !strstr(text, "water refraction") && !strstr(text, "frame breakdown") &&
              !strstr(text, "first native entry"));
    }
    free(text);
    host_perf_logging_enable();
    CHECK(host_perf_logging());
    emit_run();
    fflush(stderr);
    text = read_all(path);
    CHECK(text != NULL);
    if (text) {
        CHECK(strstr(text, "summary at exit") && strstr(text, "water refraction: 7 water batches") &&
              strstr(text, "frame breakdown 1") && strstr(text, "first native entry at 0x0063C4D0"));
        CHECK(strstr(text, "failed_d3d_calls 0") && strstr(text, "exit code 0"));
    }
    free(text);
    remove(path);
    test_producers();
    printf("log_tests: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
