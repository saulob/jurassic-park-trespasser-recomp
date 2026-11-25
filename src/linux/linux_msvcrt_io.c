/* msvcrt's file and formatting functions over the host's C library, with the guest's 32-bit layouts (FILE of 32 bytes, int file
 * descriptors, the _iob and __pioinfo data) at the boundary. Guest file names go through the game's path resolver. */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "jpt_fs.h"
#include "linux_frontend.h"
#include "linux_hle.h"
#include "linux_log.h"
#include "linux_msvcrt.h"
#include "linux_spine.h"
#include "recomp_types.h"

#define MAX_FDS 64
#define MAX_FILES 64
#define FILE_SIZE 32u
#define IOB_COUNT 3u
#define EOF_GUEST 0xFFFFFFFFu

typedef struct {
    uint32_t va;
    FILE* file;
    int std;
} guest_file_t;

static int s_fds[MAX_FDS];
static guest_file_t s_files[MAX_FILES];
static uint32_t s_iob, s_pioinfo, s_badioinfo;

static const char* cstr(uint32_t va) { return va ? gp_string(va, 4096) : NULL; }

void hle_msvcrt_io_init(void) {
    uint32_t block;
    for (int i = 0; i < MAX_FDS; i++) s_fds[i] = -1;
    s_fds[0] = 0;
    s_fds[1] = 1;
    s_fds[2] = 2;
    memset(s_files, 0, sizeof s_files);
    s_iob = gm_heap_alloc(FILE_SIZE * IOB_COUNT, 1);
    for (uint32_t i = 0; i < IOB_COUNT; i++) {
        uint32_t flags[2] = { i == 0 ? 1u : i == 1 ? 2u : 6u, i };
        gp_copy_out(s_iob + FILE_SIZE * i + 12, flags, sizeof flags);
        s_files[i].va = s_iob + FILE_SIZE * i;
        s_files[i].file = i == 0 ? stdin : i == 1 ? stdout : stderr;
        s_files[i].std = 1;
    }
    s_pioinfo = gm_heap_alloc(64 * 4, 1);
    block = gm_heap_alloc(32 * 8, 1);
    for (uint32_t fd = 0; fd < 32; fd++) {
        uint32_t entry[2] = { fd < 3 ? fd : 0xFFFFFFFFu, fd < 3 ? 0x41u : 0u };
        gp_copy_out(block + fd * 8, entry, sizeof entry);
    }
    gp_write_u32(s_pioinfo, block);
    s_badioinfo = gm_heap_alloc(8, 1);
    gp_write_u32(s_badioinfo, 0xFFFFFFFFu);
}

uint32_t hle_msvcrt_io_data(const char* name) {
    if (!strcmp(name, "_iob")) return s_iob;
    if (!strcmp(name, "__pioinfo")) return s_pioinfo;
    if (!strcmp(name, "__badioinfo")) return s_badioinfo;
    return 0;
}

static guest_file_t* file_of(uint32_t va) {
    for (int i = 0; i < MAX_FILES; i++)
        if (s_files[i].file && s_files[i].va == va) return &s_files[i];
    return NULL;
}

static int host_fd(uint32_t guest_fd) { return guest_fd < MAX_FDS ? s_fds[guest_fd] : -1; }

/* ---- low-level descriptors ---- */

#define O_RDONLY_G 0x0000u
#define O_WRONLY_G 0x0001u
#define O_RDWR_G 0x0002u
#define O_APPEND_G 0x0008u
#define O_CREAT_G 0x0100u
#define O_TRUNC_G 0x0200u
#define O_EXCL_G 0x0400u

static uint64_t io__sopen(const uint32_t* a, void* u) {
    const char* name = cstr(a[0]);
    uint32_t mode = a[1];
    char host[JPT_PATH_MAX];
    int flags = (mode & O_RDWR_G) == O_RDWR_G ? O_RDWR : (mode & O_WRONLY_G) ? O_WRONLY : O_RDONLY, fd;
    (void)u;
    if (!name || !hle_vpath()) return (uint32_t)-1;
    if (mode & O_APPEND_G) flags |= O_APPEND;
    if (mode & O_CREAT_G) flags |= O_CREAT;
    if (mode & O_TRUNC_G) flags |= O_TRUNC;
    if (mode & O_EXCL_G) flags |= O_EXCL;
    if (mode & (O_CREAT_G | O_TRUNC_G | O_WRONLY_G | O_RDWR_G)) {
        if (jpt_vpath_write_refused(hle_vpath(), name)) return (uint32_t)-1;
        if (jpt_vpath_resolve(hle_vpath(), name, (mode & O_CREAT_G) != 0, host, sizeof host) != JPT_VP_OK) return (uint32_t)-1;
    } else if (jpt_vpath_resolve_read(hle_vpath(), name, host, sizeof host, NULL) != JPT_VP_OK) {
        return (uint32_t)-1;
    }
    fd = open(host, flags | O_CLOEXEC, 0666);
    if (fd < 0) return (uint32_t)-1;
    if (mode & O_CREAT_G) jpt_vpath_invalidate(hle_vpath(), host);
    if ((mode & O_CREAT_G) && (mode & O_TRUNC_G)) lx_frontend_file_created(name);
    for (int i = 3; i < MAX_FDS; i++)
        if (s_fds[i] < 0) {
            s_fds[i] = fd;
            return (uint32_t)i;
        }
    close(fd);
    return (uint32_t)-1;
}

static uint64_t io__open(const uint32_t* a, void* u) { return io__sopen(a, u); }

static uint64_t io_remove(const uint32_t* a, void* u) {
    const char* name = cstr(a[0]);
    char host[JPT_PATH_MAX];
    (void)u;
    if (!name || !hle_vpath() || jpt_vpath_write_refused(hle_vpath(), name)) return (uint32_t)-1;
    if (jpt_vpath_resolve(hle_vpath(), name, 0, host, sizeof host) != JPT_VP_OK) return (uint32_t)-1;
    if (!jpt_fs_remove(host)) return (uint32_t)-1;
    jpt_vpath_invalidate(hle_vpath(), host);
    return 0;
}

static uint64_t io_rename(const uint32_t* a, void* u) {
    const char *from = cstr(a[0]), *to_name;
    char source[JPT_PATH_MAX], target[JPT_PATH_MAX];
    (void)u;
    to_name = cstr(a[1]);
    if (!from || !to_name || !hle_vpath()) return (uint32_t)-1;
    if (jpt_vpath_write_refused(hle_vpath(), from) || jpt_vpath_write_refused(hle_vpath(), to_name)) return (uint32_t)-1;
    if (jpt_vpath_resolve(hle_vpath(), from, 0, source, sizeof source) != JPT_VP_OK) return (uint32_t)-1;
    if (jpt_vpath_resolve(hle_vpath(), to_name, 1, target, sizeof target) != JPT_VP_OK) return (uint32_t)-1;
    if (rename(source, target) != 0) return (uint32_t)-1;
    jpt_vpath_invalidate(hle_vpath(), source);
    jpt_vpath_invalidate(hle_vpath(), target);
    return 0;
}

static uint64_t io__read(const uint32_t* a, void* u) {
    int fd = host_fd(a[0]);
    void* buf = a[2] ? gm_ptr_checked(a[1], a[2], 1) : NULL;
    ssize_t n;
    (void)u;
    if (fd < 0 || (a[2] && !buf)) return (uint32_t)-1;
    n = a[2] ? read(fd, buf, a[2]) : 0;
    return (uint32_t)(int32_t)n;
}

static uint64_t io__write(const uint32_t* a, void* u) {
    int fd = host_fd(a[0]);
    const void* buf = a[2] ? gm_ptr_checked(a[1], a[2], 0) : NULL;
    ssize_t n;
    (void)u;
    if (fd < 0 || (a[2] && !buf)) return (uint32_t)-1;
    n = a[2] ? write(fd, buf, a[2]) : 0;
    return (uint32_t)(int32_t)n;
}

static uint64_t io__close(const uint32_t* a, void* u) {
    int fd = host_fd(a[0]);
    (void)u;
    if (fd < 0) return (uint32_t)-1;
    if (a[0] >= 3) close(fd);
    s_fds[a[0]] = a[0] < 3 ? s_fds[a[0]] : -1;
    return 0;
}

static uint64_t io__lseek(const uint32_t* a, void* u) {
    int fd = host_fd(a[0]);
    off_t at;
    (void)u;
    if (fd < 0) return (uint32_t)-1;
    at = lseek(fd, (off_t)(int32_t)a[1], a[2] == 1 ? SEEK_CUR : a[2] == 2 ? SEEK_END : SEEK_SET);
    return at < 0 ? (uint32_t)-1 : (uint32_t)at;
}

static uint64_t io__setmode(const uint32_t* a, void* u) {
    (void)u;
    return host_fd(a[0]) < 0 ? (uint32_t)-1 : 0x4000u;
}

/* ---- the current directory ---- */

static uint64_t io__getcwd(const uint32_t* a, void* u) {
    char cwd[JPT_PATH_MAX];
    size_t n;
    uint32_t dst = a[0];
    (void)u;
    if (!hle_vpath()) return 0;
    snprintf(cwd, sizeof cwd, "%s", jpt_vpath_cwd(hle_vpath()));
    n = strlen(cwd);
    if (n > 3 && cwd[n - 1] == '\\') cwd[--n] = 0;
    if (!dst) dst = gm_heap_alloc(a[1] > n ? a[1] : (uint32_t)n + 1, 0);
    if (!dst || a[1] < n + 1) return 0;
    return gp_copy_out(dst, cwd, (uint32_t)n + 1) ? dst : 0;
}

static uint64_t io__chdir(const uint32_t* a, void* u) {
    const char* name = cstr(a[0]);
    char host[JPT_PATH_MAX];
    (void)u;
    if (!name || !hle_vpath() || jpt_vpath_resolve(hle_vpath(), name, 0, host, sizeof host) != JPT_VP_OK || !jpt_fs_is_dir(host))
        return (uint32_t)-1;
    return jpt_vpath_chdir(hle_vpath(), name) ? 0 : (uint32_t)-1;
}

static uint64_t io__mkdir(const uint32_t* a, void* u) {
    const char* name = cstr(a[0]);
    char host[JPT_PATH_MAX];
    (void)u;
    if (!name || !hle_vpath() || jpt_vpath_write_refused(hle_vpath(), name)) return (uint32_t)-1;
    if (jpt_vpath_resolve(hle_vpath(), name, 1, host, sizeof host) != JPT_VP_OK) return (uint32_t)-1;
    if (jpt_fs_is_dir(host)) return (uint32_t)-1;
    if (!jpt_fs_mkdir(host)) return (uint32_t)-1;
    jpt_vpath_invalidate(hle_vpath(), host);
    return 0;
}

static void put_part(uint32_t dst, const char* src, size_t n) {
    char* out;
    if (!dst) return;
    out = (char*)gm_ptr_checked(dst, (uint32_t)n + 1, 1);
    if (!out) return;
    memcpy(out, src, n);
    out[n] = 0;
}

static uint64_t io__splitpath(const uint32_t* a, void* u) {
    const char* path = cstr(a[0]);
    const char* rest;
    const char* slash = NULL;
    const char* dot;
    size_t drive_len = 0;
    (void)u;
    if (!path) return 0;
    rest = path;
    if (path[0] && path[1] == ':') {
        drive_len = 2;
        rest += 2;
    }
    for (const char* p = rest; *p; p++)
        if (*p == '\\' || *p == '/') slash = p;
    {
        const char* name = slash ? slash + 1 : rest;
        dot = strrchr(name, '.');
        put_part(a[1], path, drive_len);
        put_part(a[2], rest, (size_t)(name - rest));
        put_part(a[3], name, dot ? (size_t)(dot - name) : strlen(name));
        put_part(a[4], dot ? dot : name + strlen(name), dot ? strlen(dot) : 0);
    }
    return 0;
}

/* ---- FILE ---- */

static uint64_t io_fputc(const uint32_t* a, void* u) {
    guest_file_t* f = file_of(a[1]);
    (void)u;
    if (!f) return EOF_GUEST;
    return fputc((int)(a[0] & 0xFF), f->file) == EOF ? EOF_GUEST : (a[0] & 0xFF);
}

static uint64_t io_fgetc(const uint32_t* a, void* u) {
    guest_file_t* f = file_of(a[0]);
    int c;
    (void)u;
    if (!f) return EOF_GUEST;
    c = fgetc(f->file);
    return c == EOF ? EOF_GUEST : (uint32_t)c;
}

static uint64_t io_fwrite(const uint32_t* a, void* u) {
    guest_file_t* f = file_of(a[3]);
    uint64_t bytes = (uint64_t)a[1] * a[2];
    const void* buf = bytes ? gm_ptr_checked(a[0], (uint32_t)bytes, 0) : NULL;
    (void)u;
    if (!f || (bytes && !buf) || !a[1]) return 0;
    return bytes ? fwrite(buf, a[1], a[2], f->file) : 0;
}

static uint64_t io_fread(const uint32_t* a, void* u) {
    guest_file_t* f = file_of(a[3]);
    uint64_t bytes = (uint64_t)a[1] * a[2];
    void* buf = bytes ? gm_ptr_checked(a[0], (uint32_t)bytes, 1) : NULL;
    (void)u;
    if (!f || (bytes && !buf) || !a[1]) return 0;
    return bytes ? fread(buf, a[1], a[2], f->file) : 0;
}

static uint64_t io_fseek(const uint32_t* a, void* u) {
    guest_file_t* f = file_of(a[0]);
    (void)u;
    return f && fseek(f->file, (long)(int32_t)a[1], a[2] == 1 ? SEEK_CUR : a[2] == 2 ? SEEK_END : SEEK_SET) == 0 ? 0 : (uint32_t)-1;
}

static uint64_t io_ftell(const uint32_t* a, void* u) {
    guest_file_t* f = file_of(a[0]);
    long at;
    (void)u;
    if (!f) return (uint32_t)-1;
    at = ftell(f->file);
    return at < 0 ? (uint32_t)-1 : (uint32_t)at;
}

static uint64_t io_fflush(const uint32_t* a, void* u) {
    (void)u;
    if (!a[0]) return fflush(NULL) == 0 ? 0 : EOF_GUEST;
    return file_of(a[0]) && fflush(file_of(a[0])->file) == 0 ? 0 : EOF_GUEST;
}

/* ---- numbers from text ---- */

static const char* skip_space(const char* s) {
    while (*s == ' ' || (*s >= 9 && *s <= 13)) s++;
    return s;
}

static uint64_t io_strtol(const uint32_t* a, void* u) {
    const char* s = cstr(a[0]);
    char* end;
    long v;
    (void)u;
    if (!s) return 0;
    v = strtol(s, &end, (int)a[2]);
    if (a[1]) gp_write_u32(a[1], a[0] + (uint32_t)(end - s));
    if (v > 2147483647L) return 0x7FFFFFFFu;
    if (v < -2147483648L) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}

static uint64_t io_strtoul(const uint32_t* a, void* u) {
    const char* s = cstr(a[0]);
    char* end;
    unsigned long v;
    (void)u;
    if (!s) return 0;
    v = strtoul(s, &end, (int)a[2]);
    if (a[1]) gp_write_u32(a[1], a[0] + (uint32_t)(end - s));
    return v > 0xFFFFFFFFul ? 0xFFFFFFFFu : (uint32_t)v;
}

static uint64_t io_strtod(const uint32_t* a, void* u) {
    const char* s = cstr(a[0]);
    char* end;
    double v;
    (void)u;
    if (!s) return 0;
    v = strtod(skip_space(s), &end);
    if (a[1]) gp_write_u32(a[1], a[0] + (uint32_t)(end - s));
    fp_push_ring(v, 0);
    return 0;
}

/* ---- sprintf over the guest's argument dwords ---- */

typedef struct {
    const uint32_t* args;
    unsigned used, count;
} arg_cursor_t;

static uint32_t next_dword(arg_cursor_t* c) { return c->used < c->count ? c->args[c->used++] : 0; }

static double next_double(arg_cursor_t* c) {
    uint64_t bits = (uint64_t)next_dword(c);
    double d;
    bits |= (uint64_t)next_dword(c) << 32;
    memcpy(&d, &bits, sizeof d);
    return d;
}

static size_t guest_format(const char* fmt, arg_cursor_t* c, char* out, size_t cap) {
    size_t n = 0;
    while (*fmt && n + 1 < cap) {
        char spec[32], piece[512];
        size_t k = 0;
        int written = 0;
        if (*fmt != '%') {
            out[n++] = *fmt++;
            continue;
        }
        spec[k++] = *fmt++;
        while (*fmt && strchr("-+ #0", *fmt) && k < 8) spec[k++] = *fmt++;
        if (*fmt == '*') {
            k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int)next_dword(c));
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9' && k < 12) spec[k++] = *fmt++;
        if (*fmt == '.') {
            spec[k++] = *fmt++;
            if (*fmt == '*') {
                k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int)next_dword(c));
                fmt++;
            }
            while (*fmt >= '0' && *fmt <= '9' && k < 20) spec[k++] = *fmt++;
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'I') fmt++;
        switch (*fmt) {
        case 'd':
        case 'i':
            spec[k++] = 'd', spec[k] = 0;
            written = snprintf(piece, sizeof piece, spec, (int)next_dword(c));
            break;
        case 'u':
        case 'x':
        case 'X':
        case 'o':
            spec[k++] = *fmt, spec[k] = 0;
            written = snprintf(piece, sizeof piece, spec, (unsigned)next_dword(c));
            break;
        case 'c':
            spec[k++] = 'c', spec[k] = 0;
            written = snprintf(piece, sizeof piece, spec, (int)(next_dword(c) & 0xFF));
            break;
        case 's': {
            const char* s = cstr(next_dword(c));
            spec[k++] = 's', spec[k] = 0;
            written = snprintf(piece, sizeof piece, spec, s ? s : "(null)");
            break;
        }
        case 'p':
            written = snprintf(piece, sizeof piece, "%08X", next_dword(c));
            break;
        case 'f':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
            spec[k++] = *fmt, spec[k] = 0;
            written = snprintf(piece, sizeof piece, spec, next_double(c));
            break;
        case '%':
            piece[0] = '%', piece[1] = 0, written = 1;
            break;
        default:
            piece[0] = 0;
            break;
        }
        if (*fmt) fmt++;
        if (written > 0)
            for (int i = 0; i < written && i < (int)sizeof piece - 1 && n + 1 < cap; i++) out[n++] = piece[i];
    }
    out[n] = 0;
    return n;
}

static uint64_t io_sprintf(const uint32_t* a, void* u) {
    const char* fmt = cstr(a[1]);
    arg_cursor_t c = { a + 2, 0, 14 };
    char out[4096];
    size_t n;
    char* dst;
    (void)u;
    if (!fmt) return 0;
    n = guest_format(fmt, &c, out, sizeof out);
    dst = (char*)gm_ptr_checked(a[0], (uint32_t)n + 1, 1);
    if (!dst) return 0;
    memcpy(dst, out, n + 1);
    return (uint32_t)n;
}

static uint64_t io_printf(const uint32_t* a, void* u) {
    const char* fmt = cstr(a[0]);
    arg_cursor_t c = { a + 1, 0, 15 };
    char out[4096];
    size_t n;
    (void)u;
    if (!fmt) return 0xFFFFFFFFu;
    n = guest_format(fmt, &c, out, sizeof out);
    return fwrite(out, 1, n, stdout) == n ? (uint32_t)n : 0xFFFFFFFFu;
}

static uint64_t io_fprintf(const uint32_t* a, void* u) {
    guest_file_t* f = file_of(a[0]);
    const char* fmt = cstr(a[1]);
    arg_cursor_t c = { a + 2, 0, 14 };
    char out[4096];
    size_t n;
    (void)u;
    if (!f || !fmt) return 0xFFFFFFFFu;
    n = guest_format(fmt, &c, out, sizeof out);
    return fwrite(out, 1, n, f->file) == n ? (uint32_t)n : 0xFFFFFFFFu;
}

#define IO(fn, argc) { "msvcrt.dll", #fn, io_##fn, argc, HT_CDECL }

static const hle_entry_t k_io_table[] = {
    IO(_splitpath, 5), IO(_getcwd, 2), IO(_chdir, 1), IO(_mkdir, 1), IO(_sopen, 4), IO(_open, 3), IO(remove, 1), IO(rename, 2), IO(_read, 3), IO(_write, 3), IO(_close, 1), IO(_lseek, 3), IO(_setmode, 2),
    IO(fputc, 2), IO(fgetc, 1), IO(fwrite, 4), IO(fread, 4), IO(fseek, 3), IO(ftell, 1), IO(fflush, 1),
    IO(strtol, 3), IO(strtoul, 3), IO(strtod, 2), IO(sprintf, 16), IO(printf, 16), IO(fprintf, 16),
};

const hle_entry_t* hle_msvcrt_io_table(size_t* count) {
    *count = sizeof k_io_table / sizeof k_io_table[0];
    return k_io_table;
}
