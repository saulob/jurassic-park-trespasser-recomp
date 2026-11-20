#include "linux_msvcrt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "guest_memory.h"
#include "guest_ptr.h"
#include "linux_log.h"
#include "linux_spine.h"
#include "recomp_types.h"

#define MAX_ATEXIT 64

static uint32_t s_atexit[MAX_ATEXIT];
static unsigned s_natexit;
static uint32_t s_fmode_cell, s_commode_cell, s_errno_cell, s_acmdln_cell, s_adjust_fdiv_cell, s_cmdline;
static uint32_t s_cw = 0x0009001Fu;
static int s_app_type;
static uint32_t s_rand_seed = 1;

static uint32_t cell(uint32_t value) {
    uint32_t va = gm_heap_alloc(8, 1);
    if (va) gp_write_u32(va, value);
    return va;
}

static uint32_t guest_string(const char* text) {
    uint32_t n = (uint32_t)strlen(text) + 1, va = gm_heap_alloc(n, 0);
    if (va) gp_copy_out(va, text, n);
    return va;
}

void hle_msvcrt_init(const char* command_line) {
    s_natexit = 0;
    s_cw = 0x0009001Fu;
    s_app_type = 0;
    s_rand_seed = 1;
    s_fmode_cell = cell(0);
    s_commode_cell = cell(0);
    s_errno_cell = cell(0);
    s_adjust_fdiv_cell = cell(0);
    s_cmdline = guest_string(command_line);
    hle_msvcrt_io_init();
    s_acmdln_cell = cell(s_cmdline);
}

uint32_t hle_msvcrt_data(const char* name) {
    if (!strcmp(name, "_acmdln")) return s_acmdln_cell;
    if (!strcmp(name, "_adjust_fdiv")) return s_adjust_fdiv_cell;
    return hle_msvcrt_io_data(name);
}

static const char* cstr(uint32_t va) { return va ? gp_string(va, 4096) : NULL; }

/* ---- start-up and exit ---- */

static uint64_t m___set_app_type(const uint32_t* a, void* u) {
    (void)u;
    s_app_type = (int)a[0];
    return 0;
}

static uint64_t m___p__fmode(const uint32_t* a, void* u) { (void)a; (void)u; return s_fmode_cell; }
static uint64_t m___p__commode(const uint32_t* a, void* u) { (void)a; (void)u; return s_commode_cell; }
static uint64_t m__errno(const uint32_t* a, void* u) { (void)a; (void)u; return s_errno_cell; }
static uint64_t m___setusermatherr(const uint32_t* a, void* u) { (void)a; (void)u; return 0; }

/* MSVC's _controlfp bits against the x87 control word the lifted code rounds by. */
static uint16_t x87_from_msvc(uint32_t cw) {
    uint16_t x = 0;
    x |= (cw & 0x10u) ? 0x01 : 0;     /* invalid */
    x |= (cw & 0x80000u) ? 0x02 : 0;  /* denormal */
    x |= (cw & 0x08u) ? 0x04 : 0;     /* zero divide */
    x |= (cw & 0x04u) ? 0x08 : 0;     /* overflow */
    x |= (cw & 0x02u) ? 0x10 : 0;     /* underflow */
    x |= (cw & 0x01u) ? 0x20 : 0;     /* inexact */
    switch (cw & 0x30000u) {
    case 0x00000u: x |= 0x300; break; /* 64-bit */
    case 0x10000u: x |= 0x200; break; /* 53-bit */
    default: x |= 0x000; break;       /* 24-bit */
    }
    switch (cw & 0x300u) {
    case 0x100u: x |= 0x400; break;   /* down */
    case 0x200u: x |= 0x800; break;   /* up */
    case 0x300u: x |= 0xC00; break;   /* chop */
    default: break;
    }
    return x;
}

static uint64_t m__controlfp(const uint32_t* a, void* u) {
    (void)u;
    if (a[1]) s_cw = (s_cw & ~a[1]) | (a[0] & a[1]);
    g_fpu_cw = x87_from_msvc(s_cw);
    return s_cw;
}

static uint64_t m___getmainargs(const uint32_t* a, void* u) {
    uint32_t argv = gm_heap_alloc(16, 1), envp = gm_heap_alloc(16, 1);
    (void)u;
    if (!argv || !envp) return (uint64_t)-1;
    gp_write_u32(argv, guest_string("C:\\trespass.exe"));
    gp_write_u32(envp, 0);
    gp_write_u32(a[0], 1);
    gp_write_u32(a[1], argv);
    gp_write_u32(a[2], envp);
    return 0;
}

static uint64_t m__initterm(const uint32_t* a, void* u) {
    (void)u;
    for (uint32_t p = a[0]; p < a[1]; p += 4) {
        uint32_t fn = 0;
        if (gp_read_u32(p, &fn) && fn) lx_call_guest(fn, LX_CDECL, 0, NULL, 0, NULL);
    }
    return 0;
}

static uint64_t m__onexit(const uint32_t* a, void* u) {
    (void)u;
    if (s_natexit == MAX_ATEXIT) return 0;
    s_atexit[s_natexit++] = a[0];
    return a[0];
}

static uint64_t m___dllonexit(const uint32_t* a, void* u) { return m__onexit(a, u); }

static void terminate(uint32_t code) {
    uint32_t exit_thunk = hle_resolve("kernel32.dll", "ExitProcess", 0, NULL);
    lx_call_guest(exit_thunk, LX_STDCALL, 1, &code, 0, NULL);
}

static uint64_t m_exit(const uint32_t* a, void* u) {
    (void)u;
    while (s_natexit) lx_call_guest(s_atexit[--s_natexit], LX_CDECL, 0, NULL, 0, NULL);
    terminate(a[0]);
    return 0;
}

static uint64_t m__exit(const uint32_t* a, void* u) {
    (void)u;
    terminate(a[0]);
    return 0;
}

/* ---- memory ---- */

/* _EH_prolog: eax = the handler, the return address on the stack. It links an exception frame (prev, handler, trylevel -1) and
 * makes ebp the frame pointer, as the original does, and returns with the stack left at the frame. */
static uint64_t m__EH_prolog(const uint32_t* a, void* u) {
    uint32_t e = g_esp, prev = 0, ret = 0;
    (void)a;
    (void)u;
    gp_read_u32(g_fs_base, &prev);
    gp_read_u32(e, &ret);
    gp_write_u32(e - 4, 0xFFFFFFFFu);
    gp_write_u32(e - 8, g_eax);
    gp_write_u32(e - 12, prev);
    gp_write_u32(e, g_ebp);
    gp_write_u32(g_fs_base, e - 12);
    g_ebp = e;
    g_esp = e - 12;
    g_eax = ret;
    return 0;
}

static uint64_t m_malloc(const uint32_t* a, void* u) { (void)u; return gm_heap_alloc(a[0] ? a[0] : 1, 0); }
static uint64_t m_free(const uint32_t* a, void* u) { (void)u; gm_heap_free(a[0]); return 0; }
static uint64_t m__msize(const uint32_t* a, void* u) { (void)u; return gm_heap_size(a[0]); }

/* qsort sorts in place in guest memory (the comparison function receives guest addresses of the elements): a heapsort. */
typedef struct {
    uint32_t base, size, compare;
    uint8_t* temp;
} sort_t;

static int sort_less(const sort_t* s, uint32_t i, uint32_t j) {
    uint32_t args[2];
    args[0] = s->base + i * s->size, args[1] = s->base + j * s->size;
    return (int32_t)lx_call_guest(s->compare, LX_CDECL, 2, args, 0, NULL) < 0;
}

static void sort_swap(const sort_t* s, uint32_t i, uint32_t j) {
    uint8_t* x = (uint8_t*)gm_ptr_checked(s->base + i * s->size, s->size, 1);
    uint8_t* y = (uint8_t*)gm_ptr_checked(s->base + j * s->size, s->size, 1);
    if (!x || !y || x == y) return;
    memcpy(s->temp, x, s->size);
    memcpy(x, y, s->size);
    memcpy(y, s->temp, s->size);
}

static void sort_sift(const sort_t* s, uint32_t root, uint32_t end) {
    for (;;) {
        uint32_t child = 2 * root + 1;
        if (child >= end) return;
        if (child + 1 < end && sort_less(s, child, child + 1)) child++;
        if (!sort_less(s, root, child)) return;
        sort_swap(s, root, child);
        root = child;
    }
}

static uint64_t m_qsort(const uint32_t* a, void* u) {
    sort_t s;
    (void)u;
    if (a[1] < 2 || !a[2] || !a[3]) return 0;
    s.base = a[0], s.size = a[2], s.compare = a[3];
    if (!gm_ptr_checked(a[0], a[1] * a[2], 1)) return 0;
    s.temp = (uint8_t*)malloc(s.size);
    if (!s.temp) return 0;
    for (uint32_t i = a[1] / 2; i-- > 0;) sort_sift(&s, i, a[1]);
    for (uint32_t end = a[1] - 1; end > 0; end--) {
        sort_swap(&s, 0, end);
        sort_sift(&s, 0, end);
    }
    free(s.temp);
    return 0;
}

/* dynamic_cast over the Visual C++ 6 run-time type information of the guest image (all fields are 32-bit guest addresses):
 * vftable[-1] -> complete object locator {signature, offset, cdOffset, type, hierarchy}; hierarchy {signature, attributes, count,
 * base array}; base descriptor {type, contained bases, PMD {mdisp, pdisp, vdisp}, attributes}. */
#define BCD_NOTVISIBLE 0x1u
#define BCD_AMBIGUOUS 0x2u
#define BCD_PRIVORPROTBASE 0x4u

static int same_type(uint32_t a, uint32_t b) {
    const char* x;
    const char* y;
    if (a == b) return 1;
    x = gp_string(a + 8, 256);
    y = gp_string(b + 8, 256);
    return x && y && !strcmp(x, y);
}

static uint64_t m___RTDynamicCast(const uint32_t* a, void* u) {
    uint32_t inptr = a[0], vftable, col[5], hier[4], bases, object;
    int32_t cd = 0;
    (void)u;
    if (!inptr || !gp_read_u32(inptr, &vftable) || !gp_read_u32(vftable - 4, &col[0]) || !gp_copy_in(col, col[0], sizeof col)) return 0;
    object = inptr - col[1];
    if (col[2] && gp_copy_in(&cd, inptr - col[2], 4)) object -= (uint32_t)cd;
    if (!a[3]) return object;
    if (!gp_copy_in(hier, col[4], sizeof hier) || !gp_read_u32(hier[3], &bases)) return 0;
    for (uint32_t i = 0; i < hier[2]; i++) {
        uint32_t bcd_va, bcd[6];
        int32_t mdisp, pdisp, vdisp;
        uint32_t result;
        if (!gp_read_u32(hier[3] + 4 * i, &bcd_va) || !gp_copy_in(bcd, bcd_va, sizeof bcd)) return 0;
        if (!same_type(bcd[0], a[3]) || (bcd[5] & (BCD_NOTVISIBLE | BCD_AMBIGUOUS | BCD_PRIVORPROTBASE))) continue;
        mdisp = (int32_t)bcd[2], pdisp = (int32_t)bcd[3], vdisp = (int32_t)bcd[4];
        result = object + (uint32_t)mdisp;
        if (pdisp >= 0) {
            uint32_t vbtable = 0;
            int32_t shift = 0;
            if (!gp_read_u32(object + (uint32_t)pdisp, &vbtable) || !gp_copy_in(&shift, vbtable + (uint32_t)vdisp, 4)) return 0;
            result += (uint32_t)pdisp + (uint32_t)shift;
        }
        return result;
    }
    if (a[4]) lx_log("msvcrt", "dynamic_cast to a reference failed: std::bad_cast would be thrown");
    return 0;
}

static uint64_t m_operator_new(const uint32_t* a, void* u) { return m_malloc(a, u); }
static uint64_t m_set_new_handler(const uint32_t* a, void* u) { (void)a; (void)u; return 0; }
static uint64_t m__set_new_mode(const uint32_t* a, void* u) { (void)a; (void)u; return 0; }

/* ---- the x87 helpers: arguments on the FPU stack ---- */

static double st_pop(void) { return fp_pop_ring(); }
static void st_push(double v) { fp_push_ring(v, 0); }

static uint64_t m__ftol(const uint32_t* a, void* u) {
    double v = st_pop();
    int64_t q = (v != v || v >= 9.2233720368547758e18 || v < -9.2233720368547758e18) ? INT64_MIN : (int64_t)v;
    (void)a;
    (void)u;
    return (uint64_t)q;
}

#define UNARY(name, expr)                          \
    static uint64_t m__CI##name(const uint32_t* a, void* u) { \
        double x = st_pop();                       \
        (void)a;                                   \
        (void)u;                                   \
        st_push(expr);                             \
        return 0;                                  \
    }
#define BINARY(name, expr)                         \
    static uint64_t m__CI##name(const uint32_t* a, void* u) { \
        double y = st_pop(), x = st_pop();         \
        (void)a;                                   \
        (void)u;                                   \
        st_push(expr);                             \
        return 0;                                  \
    }
UNARY(sqrt, sqrt(x))
UNARY(sin, sin(x))
UNARY(cos, cos(x))
UNARY(asin, asin(x))
UNARY(acos, acos(x))
UNARY(exp, exp(x))
BINARY(pow, pow(x, y))
BINARY(fmod, fmod(x, y))
BINARY(atan2, atan2(x, y))

static double arg_double(const uint32_t* a) {
    uint64_t bits = (uint64_t)a[0] | (uint64_t)a[1] << 32;
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

static uint64_t m_floor(const uint32_t* a, void* u) {
    (void)u;
    st_push(floor(arg_double(a)));
    return 0;
}

static uint64_t m__finite(const uint32_t* a, void* u) { (void)u; return isfinite(arg_double(a)) ? 1 : 0; }

/* ---- strings, numbers, time ---- */

static uint64_t m_atoi(const uint32_t* a, void* u) {
    const char* s = cstr(a[0]);
    (void)u;
    return s ? (uint32_t)(int32_t)atoi(s) : 0;
}

static uint64_t m_isspace(const uint32_t* a, void* u) { (void)u; return a[0] < 128 && isspace((int)a[0]) ? 8 : 0; }
static uint64_t m_toupper(const uint32_t* a, void* u) { (void)u; return a[0] < 128 ? (uint32_t)toupper((int)a[0]) : a[0]; }
static uint64_t m_tolower(const uint32_t* a, void* u) { (void)u; return a[0] < 128 ? (uint32_t)tolower((int)a[0]) : a[0]; }

static uint64_t m_srand(const uint32_t* a, void* u) { (void)u; s_rand_seed = a[0]; return 0; }

static uint64_t m_rand(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    s_rand_seed = s_rand_seed * 214013u + 2531011u;
    return (s_rand_seed >> 16) & 0x7FFFu;
}

static uint64_t m_strchr(const uint32_t* a, void* u) {
    const char* s = cstr(a[0]);
    const char* p;
    (void)u;
    if (!s) return 0;
    p = strchr(s, (int)(a[1] & 0xFF));
    return p ? a[0] + (uint32_t)(p - s) : 0;
}

static uint64_t m_strpbrk(const uint32_t* a, void* u) {
    const char* s = cstr(a[0]);
    const char* set = cstr(a[1]);
    const char* p;
    (void)u;
    if (!s || !set) return 0;
    p = strpbrk(s, set);
    return p ? a[0] + (uint32_t)(p - s) : 0;
}

static uint64_t m_strncpy(const uint32_t* a, void* u) {
    const char* src = cstr(a[1]);
    char* dst = (char*)gm_ptr_checked(a[0], a[2], 1);
    (void)u;
    if (!src || !dst) return 0;
    strncpy(dst, src, a[2]);
    return a[0];
}

static uint64_t m__strdup(const uint32_t* a, void* u) {
    const char* s = cstr(a[0]);
    (void)u;
    return s ? guest_string(s) : 0;
}

static uint64_t m__strcmpi(const uint32_t* a, void* u) {
    const char* x = cstr(a[0]);
    const char* y = cstr(a[1]);
    (void)u;
    if (!x || !y) return 0;
    for (;; x++, y++) {
        int cx = tolower((unsigned char)*x), cy = tolower((unsigned char)*y);
        if (cx != cy) return (uint32_t)(cx < cy ? -1 : 1);
        if (!cx) return 0;
    }
}

static uint64_t m_strlen(const uint32_t* a, void* u) {
    const char* s = cstr(a[0]);
    (void)u;
    return s ? strlen(s) : 0;
}

static uint64_t m_strcpy(const uint32_t* a, void* u) {
    const char* s = cstr(a[1]);
    char* dst = s ? (char*)gm_ptr_checked(a[0], (uint32_t)strlen(s) + 1, 1) : NULL;
    (void)u;
    if (!dst) return 0;
    strcpy(dst, s);
    return a[0];
}

static uint64_t m_memcpy(const uint32_t* a, void* u) {
    void* dst = a[2] ? gm_ptr_checked(a[0], a[2], 1) : NULL;
    const void* src = a[2] ? gm_ptr_checked(a[1], a[2], 0) : NULL;
    (void)u;
    if (a[2] && dst && src) memmove(dst, src, a[2]);
    return a[0];
}

static uint64_t m_memmove(const uint32_t* a, void* u) { return m_memcpy(a, u); }
static uint64_t m_isdigit(const uint32_t* a, void* u) { (void)u; return a[0] < 128 && isdigit((int)a[0]) ? 4 : 0; }
static uint64_t m_isxdigit(const uint32_t* a, void* u) { (void)u; return a[0] < 128 && isxdigit((int)a[0]) ? 0x80 : 0; }
static uint64_t m_operator_delete(const uint32_t* a, void* u) { return m_free(a, u); }

static uint64_t m__purecall(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    lx_abort(LX_FATAL_ABI, g_cur_func, "pure virtual function call");
    return 0;
}

static uint64_t m_terminate(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    lx_abort(LX_FATAL_ABI, g_cur_func, "terminate: an unhandled guest C++ exception");
    return 0;
}

static uint64_t m_getenv(const uint32_t* a, void* u) {
    (void)a;
    (void)u;
    return 0;
}

static uint64_t m_time(const uint32_t* a, void* u) {
    uint32_t now = (uint32_t)time(NULL);
    (void)u;
    if (a[0]) gp_write_u32(a[0], now);
    return now;
}

#define C(fn, argc) { "msvcrt.dll", #fn, m_##fn, argc, HT_CDECL }
#define CX(sym, fn, argc) { "msvcrt.dll", sym, m_##fn, argc, HT_CDECL }
#define CR(fn) { "msvcrt.dll", #fn, m_##fn, 0, HT_RAW }

static const hle_entry_t k_table[] = {
    C(__set_app_type, 1), C(__p__fmode, 0), C(__p__commode, 0), C(_errno, 0), C(__setusermatherr, 1), C(_controlfp, 2),
    C(__getmainargs, 5), C(_initterm, 2), C(_onexit, 1), C(__dllonexit, 3), C(exit, 1), C(_exit, 1),
    C(malloc, 1), C(free, 1), C(_msize, 1),
    CX("??2@YAPAXI@Z", operator_new, 1), CX("?set_new_handler@@YAP6AXXZP6AXXZ@Z", set_new_handler, 1),
    CX("?_set_new_mode@@YAHH@Z", _set_new_mode, 1), CX("?_set_new_handler@@YAP6AHI@ZP6AHI@Z@Z", set_new_handler, 1),
    C(_ftol, 0), C(_CIsqrt, 0), C(_CIsin, 0), C(_CIcos, 0), C(_CIasin, 0), C(_CIacos, 0), C(_CIexp, 0), C(_CIpow, 0),
    C(_CIfmod, 0), C(_CIatan2, 0), C(floor, 2), C(_finite, 2),
    C(atoi, 1), C(isspace, 1), C(toupper, 1), C(tolower, 1), C(srand, 1), C(rand, 0), C(strchr, 2), C(strpbrk, 2),
    C(strncpy, 3), C(_strdup, 1), C(_strcmpi, 2), C(time, 1), C(getenv, 1), CR(_EH_prolog), C(strlen, 1), C(strcpy, 2), C(memcpy, 3), C(memmove, 3), C(isdigit, 1), C(isxdigit, 1),
    C(qsort, 4), C(__RTDynamicCast, 5), C(_purecall, 0), CX("??3@YAXPAX@Z", operator_delete, 1), CX("?terminate@@YAXXZ", terminate, 0),
};

const hle_entry_t* hle_msvcrt_table(size_t* count) {
    *count = sizeof k_table / sizeof k_table[0];
    return k_table;
}
