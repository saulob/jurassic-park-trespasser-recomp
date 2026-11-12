#include "host.h"

#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>

#define LOG_NAME "trespasser"
#define MAX_LOG_NUMBER 99999

static HANDLE s_file = INVALID_HANDLE_VALUE;
static HANDLE s_console = INVALID_HANDLE_VALUE;
static char s_path[MAX_PATH];
static DWORD s_start_tick;
static int s_perf_logging;

void host_perf_logging_enable(void) { s_perf_logging = 1; }

int host_perf_logging(void) { return s_perf_logging; }

static HANDLE open_append(const char* path, DWORD disposition) {
    return CreateFileA(path, FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disposition,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

/* The same numbering as tools\build.ps1: one past the highest trespasser_NNN.log in the folder. */
static HANDLE create_numbered(const char* dir) {
    WIN32_FIND_DATAA found;
    char pattern[MAX_PATH];
    unsigned highest = 0, n;
    HANDLE search;
    _snprintf(pattern, sizeof pattern - 1, "%s\\" LOG_NAME "_*.log", dir);
    pattern[sizeof pattern - 1] = 0;
    search = FindFirstFileA(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (sscanf(found.cFileName, LOG_NAME "_%u.log", &n) == 1 && n > highest) highest = n;
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
    for (n = highest + 1; n <= MAX_LOG_NUMBER; n++) {
        HANDLE h;
        _snprintf(s_path, sizeof s_path - 1, "%s\\" LOG_NAME "_%03u.log", dir, n);
        s_path[sizeof s_path - 1] = 0;
        h = open_append(s_path, CREATE_NEW);
        if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) return h;
    }
    return INVALID_HANDLE_VALUE;
}

/* stderr is pointed at the log so pcrecomp's own fprintf(stderr) reports land in it. The file is
 * opened for appending only, so whatever the build script wrote before stays as it was, and
 * stderr is unbuffered so a crash loses nothing. */
int host_log_open(const char* dir, const char* file) {
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE h;
    int fd;
    s_start_tick = GetTickCount();
    if (err && err != INVALID_HANDLE_VALUE)
        DuplicateHandle(GetCurrentProcess(), err, GetCurrentProcess(), &s_console, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (file) {
        _snprintf(s_path, sizeof s_path - 1, "%s", file);
        s_path[sizeof s_path - 1] = 0;
        h = open_append(s_path, OPEN_ALWAYS);
    } else {
        CreateDirectoryA(dir, NULL);
        h = create_numbered(dir);
    }
    if (h == INVALID_HANDLE_VALUE) return 0;
    fd = _open_osfhandle((intptr_t)h, _O_APPEND | _O_TEXT);
    if (fd < 0 || _dup2(fd, _fileno(stderr)) != 0) {
        if (fd < 0) CloseHandle(h);
        return 0;
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    s_file = h;
    return 1;
}

const char* host_log_path(void) { return s_path; }

static void echo_to_console(const char* text, unsigned len) {
    DWORD written;
    if (s_console != INVALID_HANDLE_VALUE) WriteFile(s_console, text, len, &written, NULL);
}

void host_log(const char* tag, const char* fmt, ...) {
    char line[1024];
    va_list ap;
    int head = _snprintf(line, sizeof line - 2, "[%9.3f] [%-8s] ",
                         (GetTickCount() - s_start_tick) / 1000.0, tag);
    int body;
    if (head < 0) return;
    va_start(ap, fmt);
    body = _vsnprintf(line + head, sizeof line - 2 - head, fmt, ap);
    va_end(ap);
    if (body < 0) body = (int)(sizeof line - 2 - head);
    line[head + body] = '\n';
    line[head + body + 1] = 0;
    fputs(line, stderr);
    echo_to_console(line, (unsigned)(head + body + 1));
}

/* No stdio: usable from the fault handler while another thread holds the CRT lock. */
void host_log_raw(const char* text, unsigned len) {
    DWORD written;
    if (s_file != INVALID_HANDLE_VALUE) WriteFile(s_file, text, len, &written, NULL);
    echo_to_console(text, len);
}
