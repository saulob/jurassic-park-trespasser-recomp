#include "linux_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "jpt_fs.h"
#include "jpt_sync.h"

static FILE* s_file;
static char s_path[JPT_PATH_MAX];

int lx_log_open(const char* dir, const char* file) {
    if (s_file) return 1;
    if (file) {
        snprintf(s_path, sizeof s_path, "%s", file);
    } else {
        for (int n = 1; n < 10000; n++) {
            snprintf(s_path, sizeof s_path, "%s/trespasser_linux_%03d.log", dir, n);
            if (!jpt_fs_exists(s_path)) break;
        }
    }
    s_file = fopen(s_path, "a");
    return s_file != NULL;
}

const char* lx_log_path(void) { return s_path; }
int lx_log_fd(void) { return s_file ? fileno(s_file) : -1; }

static void log_line(const char* tag, const char* fmt, va_list args) {
    char line[2048];
    int n = snprintf(line, sizeof line, "[%8.3f] [%s] ", (double)jpt_time_ms() / 1000.0, tag);
    if (n > 0 && (size_t)n < sizeof line) vsnprintf(line + n, sizeof line - (size_t)n, fmt, args);
    fprintf(stderr, "%s\n", line);
    if (s_file) {
        fprintf(s_file, "%s\n", line);
        fflush(s_file);
    }
}

void lx_log(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_line(tag, fmt, args);
    va_end(args);
}

/* The log interface the portable audio and video sources are written against (src/host/audio/audio_log.h). */
void host_log(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_line(tag, fmt, args);
    va_end(args);
}

void host_log_perf(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_line(tag, fmt, args);
    va_end(args);
}

void lx_log_close(void) {
    if (s_file) fclose(s_file);
    s_file = NULL;
}
