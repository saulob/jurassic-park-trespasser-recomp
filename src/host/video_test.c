/*
 * Video Test (development): F5 in the main menu opens a panel that plays each of the game's videos with the game's own
 * presentation or the HD one. A request waits for the main menu's next frame, then runs what the Options window's
 * Credits button runs (0x004163D5): menu music and its timer off, a CVideoWnd on the stack, Play, music and timer back.
 */
#include "guest_va.h"
#include "host.h"

#include <stdio.h>
#include <string.h>

#include "jpt_tr.h"
#include "video_test_panel.h"
#include "video_test_state.h"

#define MENU_AUDIO GVA(0x0066E338)
#define MENU_MUSIC_OFF GVA(0x00414EB0)
#define MENU_MUSIC_ON GVA(0x00414E80)
#define MENU_TIMER_OFF GVA(0x00415360)
#define MENU_TIMER_ON GVA(0x00415330)
#define VIDEO_WND_CTOR GVA(0x00418700)
#define VIDEO_WND_PLAY GVA(0x00418AE0)
#define VIDEO_WND_DTOR GVA(0x00418760)
#define WND_OWNER 0x04u
#define VIDEO_WND_STACK 0x200u

static vt_state_t s_state;
static vt_panel_row_t s_rows[VT_VIDEO_COUNT];
static int s_rows_read;

static int main_menu_front(const guest_ui_view_t* v) {
    return v->kind == GUEST_WND_MAIN_MENU && v->ui == GUEST_UI_MENU && v->depth == 1;
}

static void read_rows(void) {
    char dir[MAX_PATH], path[MAX_PATH];
    char* slash;
    strncpy(dir, guest_env_exe_path(), sizeof dir - 1);
    dir[sizeof dir - 1] = 0;
    slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    for (int i = 0; i < VT_VIDEO_COUNT; i++) {
        vt_panel_row_t* r = &s_rows[i];
        uint8_t header[VL_SMK_HEADER_BYTES];
        vl_smk_info_t info;
        FILE* f;
        size_t got = 0;
        r->file = vt_videos[i].file;
        _snprintf(path, sizeof path - 1, "%s\\data\\menu\\%s", dir, vt_videos[i].file);
        path[sizeof path - 1] = 0;
        f = fopen(path, "rb");
        if (f) {
            got = fread(header, 1, sizeof header, f);
            fclose(f);
        }
        if (!vl_smk_parse(header, (uint32_t)got, &info)) {
            snprintf(r->size, sizeof r->size, "%s", jpt_tr("not found"));
            r->length[0] = 0;
            continue;
        }
        if (vl_smk_shown_rows(info.height, info.flags) != info.height)
            _snprintf(r->size, sizeof r->size - 1, "%ux%u (shown %ux%u)", info.width, info.height, info.width,
                      vl_smk_shown_rows(info.height, info.flags));
        else
            _snprintf(r->size, sizeof r->size - 1, "%ux%u", info.width, info.height);
        _snprintf(r->length, sizeof r->length - 1, "%.1f s", info.frames * vl_smk_ms_per_frame(info.frame_rate) / 1000.0);
    }
    s_rows_read = 1;
}

void video_test_f5(void) {
    guest_ui_view_t front;
    guest_ui_probe(&front);
    if (vt_f5(&s_state, main_menu_front(&front)))
        host_log("video-test", "panel %s", s_state.open ? "opened (F5 in the main menu)" : "closed");
}

int video_test_visible(void) { return vt_visible(&s_state); }

int video_test_mouse(UINT msg, WPARAM wparam, LPARAM lparam) {
    RECT view;
    if (!vt_visible(&s_state)) return 0;
    if (msg != WM_MOUSEWHEEL && window_host_view_rect(&view))
        vt_panel_mouse_move((float)((short)LOWORD(lparam) - view.left), (float)((short)HIWORD(lparam) - view.top));
    switch (msg) {
    case WM_MOUSEMOVE: return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: vt_panel_mouse_button(0, 1); break;
    case WM_LBUTTONUP: vt_panel_mouse_button(0, 0); break;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK: vt_panel_mouse_button(1, 1); break;
    case WM_RBUTTONUP: vt_panel_mouse_button(1, 0); break;
    case WM_MOUSEWHEEL: vt_panel_mouse_wheel((float)(short)HIWORD(wparam) / (float)WHEEL_DELTA); break;
    default: break;
    }
    return 1;
}

static float dpi_scale(void) {
    static float scale;
    if (!scale) {
        HDC dc = GetDC(NULL);
        scale = dc ? (float)GetDeviceCaps(dc, LOGPIXELSY) / 96.0f : 1.0f;
        if (dc) ReleaseDC(NULL, dc);
        if (scale < 1.0f) scale = 1.0f;
    }
    return scale;
}

void video_test_overlay(struct ID3D11Device* device, struct ID3D11DeviceContext* context,
                        struct ID3D11RenderTargetView* target, int width, int height) {
    vt_panel_frame_t f;
    RECT view;
    int video = 0, mode = VL_PLAY_AUTO;
    if (!vt_visible(&s_state) || !window_host_view_rect(&view)) return;
    if (!s_rows_read) read_rows();
    f.count = VT_VIDEO_COUNT;
    f.rows = s_rows;
    f.hd_available = d3dr_hardware();
    f.view_w = (float)(view.right - view.left);
    f.view_h = (float)(view.bottom - view.top);
    f.dpi_scale = dpi_scale();
    if (vt_panel_draw(device, context, target, width, height, &f, &video, &mode))
        vt_request(&s_state, video, (vl_play_mode_t)mode);
}

/* this, then one stack argument or none; each callee pops what it was given. */
static void call_guest(uint32_t va, uint32_t self, const uint32_t* arg) {
    recomp_func_t f = recomp_lookup(va);
    if (arg) PUSH32(g_esp, *arg);
    PUSH32(g_esp, RECOMP_RETADDR);
    g_ecx = self;
    f();
}

void video_test_ui_frame(void) {
    guest_ui_view_t front;
    vl_play_mode_t mode;
    int video;
    uint32_t owner = 0, object, esp = g_esp, eax = g_eax, ecx = g_ecx, edx = g_edx, name, played;
    if (!s_state.pending) return;
    guest_ui_probe(&front);
    if (!vt_start(&s_state, main_menu_front(&front), &video, &mode)) return;
    host_log("video-test", "%s requested in %s mode", vt_videos[video].file, vl_play_mode_name(mode));
    if (!guest_read32(front.top + WND_OWNER, &owner) || !owner) {
        host_log("video-test", "the main menu has no window owner to play %s with", vt_videos[video].file);
        vt_finished(&s_state);
        return;
    }
    name = GVA_AT(vt_videos[video].name_id);
    g_esp -= VIDEO_WND_STACK;
    object = g_esp;
    call_guest(MENU_MUSIC_OFF, MENU_AUDIO, NULL);
    call_guest(MENU_TIMER_OFF, 0, NULL);
    call_guest(VIDEO_WND_CTOR, object, &owner);
    video_hd_force(mode);
    call_guest(VIDEO_WND_PLAY, object, &name);
    played = g_eax;
    video_hd_force(VL_PLAY_AUTO);
    call_guest(MENU_MUSIC_ON, MENU_AUDIO, NULL);
    call_guest(MENU_TIMER_ON, 0, NULL);
    call_guest(VIDEO_WND_DTOR, object, NULL);
    if (g_esp != object)
        host_log("video-test", "the game's stack moved by %d bytes around %s", (int)(g_esp - object), vt_videos[video].file);
    if (!played) host_log("video-test", "the game could not play %s", vt_videos[video].file);
    g_esp = esp;
    g_eax = eax;
    g_ecx = ecx;
    g_edx = edx;
    vt_finished(&s_state);
}

void video_test_release(void) { vt_panel_release(); }
