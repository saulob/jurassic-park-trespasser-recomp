/* The user32 functions the guest calls, answered as guest compatibility: windows are guest objects behind 32-bit tokens (slot and
 * generation), their procedures run through lx_call_guest, and the one real window is a backend (linux_window.h) the top-level
 * window is shown in. The message queue is host-side; input events from the backend become Win32 messages here. */
#ifndef JPT_LINUX_USER32_H
#define JPT_LINUX_USER32_H

#include <stddef.h>
#include <stdint.h>

#include "linux_hle.h"

const hle_entry_t* hle_user32_table(size_t* count);
void hle_user32_init(void);
void hle_user32_shutdown(void);

/* Headless runs: after this many PeekMessage calls with nothing to deliver and no frame presented, the run stops (LX_STOP,
 * "idle"). 0 = never. */
void lx_user32_set_idle_limit(unsigned empty_peeks);

/* Tests and the other Win32 layers. */
int lx_user32_window_exists(uint32_t hwnd);
int lx_user32_client_size(uint32_t hwnd, int* w, int* h);
uint32_t lx_user32_main_window(void);
unsigned lx_user32_window_count(void);
unsigned lx_user32_class_count(void);
uint32_t lx_user32_class_wndproc(const char* name);
/* A guest window message sent to the window's procedure. */
uint32_t lx_user32_send(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);
int lx_user32_post(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);
uint32_t lx_user32_queue_length(void);

#endif
