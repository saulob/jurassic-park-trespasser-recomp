#include "linux_log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "jpt_fs.h"
#include "jpt_port.h"
#include "jpt_sync.h"

static FILE* s_file;
static int s_quiet;
static char s_path[JPT_PATH_MAX];

static unsigned session_file_number(const char* filename, const char* prefix) {
    size_t plen = strlen(prefix);
    unsigned number = 0, attempt = 0;
    int used = 0, tail = 0;
    if (strncmp(filename, prefix, plen) != 0 || filename[plen] != '_') return 0;
    const char* rest = filename + plen + 1;
    if (rest[0] < '0' || rest[0] > '9') return 0;
    if (sscanf(rest, "%u%n", &number, &used) != 1 || used <= 0) return 0;
    rest += used;
    if (!strcmp(rest, ".log") || !strcmp(rest, ".dmp")) return number;
    if (rest[0] == '_' && rest[1] >= '0' && rest[1] <= '9' &&
        sscanf(rest, "_%u.dmp%n", &attempt, &tail) == 1 && tail > 0 && !rest[tail])
        return number;
    return 0;
}

static unsigned highest_numbered(const char* dir) {
    static const char* const k_prefixes[] = { JPT_LOG_NAME, JPT_LEGACY_LINUX_LOG_NAME, JPT_LEGACY_LOG_NAME };
    DIR* d = opendir(dir);
    unsigned highest = 0;
    if (!d) return 0;
    for (struct dirent* e; (e = readdir(d)) != NULL;) {
        for (size_t i = 0; i < sizeof k_prefixes / sizeof k_prefixes[0]; i++) {
            unsigned n = session_file_number(e->d_name, k_prefixes[i]);
            if (n > highest) highest = n;
        }
    }
    closedir(d);
    return highest;
}

static int attach_fd(int fd, const char* path) {
    FILE* f = fdopen(fd, "a");
    if (!f) {
        close(fd);
        return 0;
    }
    s_file = f;
    snprintf(s_path, sizeof s_path, "%s", path);
    return 1;
}

int lx_log_open_from(const char* dir, unsigned first_number) {
    char candidate[JPT_PATH_MAX];
    const char* sep;
    size_t dlen;
    if (s_file) return 1;
    if (!dir || !*dir) return 0;
    jpt_fs_mkdir(dir);
    dlen = strlen(dir);
    sep = (dlen > 0 && dir[dlen - 1] == '/') ? "" : "/";
    if (first_number == 0) first_number = 1;
    for (unsigned n = first_number; n <= JPT_MAX_LOG_NUMBER; n++) {
        int len = snprintf(candidate, sizeof candidate, "%s%s" JPT_LOG_NAME "_%03u.log", dir, sep, n);
        int fd;
        if (len < 0 || (size_t)len >= sizeof candidate) return 0;
        fd = open(candidate, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0666);
        if (fd >= 0) return attach_fd(fd, candidate);
        if (errno != EEXIST) return 0;
    }
    return 0;
}

int lx_log_open(const char* dir, const char* file) {
    if (s_file) return 1;
    if (file) {
        char candidate[JPT_PATH_MAX];
        int len, fd;
        if (!*file) return 0;
        len = snprintf(candidate, sizeof candidate, "%s", file);
        if (len < 0 || (size_t)len >= sizeof candidate) return 0;
        fd = open(candidate, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
        if (fd < 0) return 0;
        return attach_fd(fd, candidate);
    }
    if (!dir || !*dir) return 0;
    jpt_fs_mkdir(dir);
    return lx_log_open_from(dir, highest_numbered(dir) + 1);
}

void lx_log_quiet(int quiet) { s_quiet = quiet; }

const char* lx_log_path(void) { return s_path; }
int lx_log_fd(void) { return s_file ? fileno(s_file) : -1; }

static void log_line(const char* tag, const char* fmt, va_list args) {
    char line[2048];
    int n = snprintf(line, sizeof line, "[%8.3f] [%s] ", (double)jpt_time_ms() / 1000.0, tag);
    if (n > 0 && (size_t)n < sizeof line) vsnprintf(line + n, sizeof line - (size_t)n, fmt, args);
    if (!s_quiet) fprintf(stderr, "%s\n", line);
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
    s_path[0] = 0;
}
