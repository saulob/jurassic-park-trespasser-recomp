/* What the host's features need from the lifted guest, with no platform header: the registers and guest memory of the
 * lifted runtime, the logging, guest calls and guest memory reads the host provides, and the stable-entry overrides.
 * Windows implements it in the host (host.h includes this), Linux in src/linux/linux_host.c; a feature written against
 * this header runs unchanged on both. */
#ifndef JPT_GUEST_ABI_H
#define JPT_GUEST_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include "native32.h"
#else
#include "recomp_types.h"
#endif

#include "guest_ui_model.h"
#include "guest_va.h"
#include "host_policy.h"

/* A shim is entered with g_esp at [return slot][arg0][arg1]... */
#define GUEST_ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define GUEST_PTR(n) ((void*)ADDR(GUEST_ARG(n)))
#define GUEST_RETURN_STDCALL(nargs) (g_esp += 4 + 4 * (nargs))

void host_log(const char* tag, const char* fmt, ...);
/* Diagnostics and statistics: written only with --perf-log. */
void host_log_perf(const char* tag, const char* fmt, ...);
/* --perf-log is on (always off on Linux). */
int host_perf_logging(void);

/* Ends the session in an orderly way: the custom level cannot go on, and why is in the log. */
#ifdef _MSC_VER
__declspec(noreturn)
#endif
void host_stop_level(const char* message);

/* Whether a reservation of this size can be made now. */
int guest_reservation_fits(uint32_t bytes);

/* A lifted function called from the host; 0 when it is missing or left the stack moved. */
int guest_call(uint32_t va, uint32_t self, const uint32_t* args, int count, int caller_pops, uint32_t* result);
int guest_call_virtual(uint32_t self, uint32_t slot, const uint32_t* args, int count, uint32_t* result);

int guest_read32(uint32_t va, uint32_t* out);
int guest_readable(uint32_t va, uint32_t bytes);
int guest_writable(uint32_t va, uint32_t bytes);
/* A NUL-terminated guest string, cut to cap - 1 bytes; 0 when unreadable. */
int guest_read_string(uint32_t va, char* out, int cap);
int guest_write32(uint32_t va, uint32_t value);

/* The game's own window stack: which window is in front (guest_ui_model.h). 1 when the stack was read. */
int guest_ui_probe(guest_ui_view_t* out);
const char* guest_ui_class_name(uint32_t vtable);
/* The game's own "app active" flag: its message pump returns to its loops only while it is set. */
int guest_ui_active(const guest_ui_view_t* v);
/* The game's EndModal(code) on the window in front; quit_to_windows adds the flags its in-game Quit sets. 1 done, -1 the
 * window is already ending, 0 not possible. */
int guest_ui_end_window(const guest_ui_view_t* v, int32_t code, int quit_to_windows);

/* A string of the game's own resources, in the display code page; 0 when the game has none with that id. */
int guest_load_string(unsigned id, char* out, int cap);

/* While on, the game's own "NoVideo" reads 1 (Skip Intro): CVideoWnd::Play returns at once, as it does with the value stored. */
void guest_skip_video(int on);

/* The host folder holding the game's data files (its level scenes); 0 when it does not fit. */
int guest_data_dir(char* out, size_t cap);

/* A guest address for host bytes that stay valid for the run (a translated caption block): the same bytes give the same address. */
uint32_t guest_keep(const void* data, uint32_t size);
/* The host path of a path the guest uses (the game's own drive letters on Linux); 0 when it does not resolve. */
int guest_host_path(const char* guest_path, char* out, size_t cap);

/* The guest address of bytes the host keeps for the guest to read (a copy it hands a lifted function): `size` bytes that
 * stay valid until the next call for the same slot. */
uint32_t guest_scratch(unsigned slot, uint32_t size);

/* The lifted functions of guest_override_list.h, replaceable through their stable entries. Installs are refused until the
 * target executable is verified; 0 when the list is not sorted and unique. */
int guest_overrides_enable(void);
/* Points va's stable entry at replacement; *lifted receives the lifted body. 0: va is not listed or already replaced. */
int guest_override_install(uint32_t va, recomp_func_t replacement, recomp_func_t* lifted);
/* The lifted body behind va's stable entry; NULL when va has none. */
recomp_func_t guest_override_lifted(uint32_t va);
/* One line on what was installed; 0 when a required override is missing. */
int guest_overrides_check(void);

#endif
