/*
 * The game's Smacker videos in HD. CVideoWnd::Play (0x00418AE0) decodes each frame into its 640x480 raster, copies the
 * changed rectangles to the screen and Unlocks it, and the renderer presents that. While Play runs with Direct3D 11,
 * those presents are drawn instead on a surface covering the whole window (the game's own surface keeps the 640x480
 * screen's 4:3 shape): the picture at the window's size, its sides extended. The game keeps decoding, timing, audio
 * and skipping; the surface goes once the game's own picture is back on screen.
 */
#define COBJMACROS
#include "guest_va.h"
#include "host.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <string.h>

#include "ddraw/d3d_legacy.h"
#include "ddraw/video_gpu.h"
#include "video_layout.h"

#define VIDEO_PLAY_VA GVA(0x00418AE0)
#define PLAYER_SMACK 0x134u
#define PLAYER_RASTER 0x13Cu
#define PLAYER_TOP 0x144u
#define PLAYER_LEFT 0x148u
#define PLAYER_BUFFER_PATH 0x154u /* the decoder blits to the window: the screen had no lpSurface */
#define PLAYER_FIRST_PENDING 0x155u
#define NULL_RASTER GVA(0x00669B90)
#define SMACK_WIDTH 0x04u
#define SMACK_HEIGHT 0x08u
#define SMACK_FRAMES 0x0Cu
#define SMACK_RATE 0x10u
#define SMACK_FLAGS 0x14u
#define SMACK_PALETTE 0x6Cu
#define SMACK_FRAME 0x374u
#define SMACK_BYTES 0x378u
#define OVERLAY_CLASS "TrespasserVideoSurface"

typedef HRESULT(WINAPI* dwm_flush_fn)(void);

typedef struct {
    uint32_t smack, width, shown, frames, flags, frame;
    int32_t rate;
    int32_t left, top;
} player_t;

static struct {
    recomp_func_t play_lifted;
    vl_play_mode_t forced;
    vl_session_t session;
    uint32_t player;
    char name[64];
    player_t first;
    int logged_start, broken;
    video_gpu_t* gpu;
    HWND overlay, parent;
    IDXGISwapChain1* chain;
    ID3D11RenderTargetView* rtv;
    int chain_w, chain_h, client_w, client_h;
    LARGE_INTEGER qpc_frequency;
} V;

static vl_palette_map_t s_palette;

static double now_ms(void) {
    LARGE_INTEGER t;
    if (!V.qpc_frequency.QuadPart) QueryPerformanceFrequency(&V.qpc_frequency);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)V.qpc_frequency.QuadPart;
}

static void release_chain(void) {
    if (V.rtv) ID3D11RenderTargetView_Release(V.rtv);
    if (V.chain) IDXGISwapChain1_Release(V.chain);
    V.rtv = NULL;
    V.chain = NULL;
    V.chain_w = V.chain_h = 0;
}

static void release_overlay(void) {
    release_chain();
    video_gpu_destroy(V.gpu);
    V.gpu = NULL;
    if (V.overlay && IsWindow(V.overlay)) DestroyWindow(V.overlay);
    V.overlay = NULL;
    V.parent = NULL;
    V.client_w = V.client_h = 0;
    s_palette.valid = 0;
}

static void warn_once(const char* why) {
    if (V.broken) return;
    V.broken = 1;
    host_log("video", "HD video presentation is off (%s): the game's own playback is shown", why);
}

static LRESULT CALLBACK overlay_proc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcA(window, msg, wparam, lparam);
}

static HWND create_overlay(HWND parent, int w, int h) {
    static int registered;
    HINSTANCE instance = GetModuleHandleA(NULL);
    HWND window;
    if (!registered) {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = overlay_proc;
        wc.hInstance = instance;
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = OVERLAY_CLASS;
        if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return NULL;
        registered = 1;
    }
    window = CreateWindowExA(0, OVERLAY_CLASS, NULL, WS_CHILD | WS_CLIPSIBLINGS, 0, 0, w, h, parent, NULL, instance, NULL);
    if (!window) return NULL;
    SetWindowPos(window, HWND_TOP, 0, 0, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return window;
}

static int create_chain(int w, int h) {
    ID3D11Device* device = NULL;
    ID3D11DeviceContext* context = NULL;
    IDXGIDevice* dxgi = NULL;
    IDXGIAdapter* adapter = NULL;
    IDXGIFactory2* factory = NULL;
    DXGI_SWAP_CHAIN_DESC1 desc;
    HRESULT hr = E_FAIL;
    if (!present_device(&device, &context)) return 0;
    if (SUCCEEDED(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void**)&dxgi)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter)))
        hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void**)&factory);
    if (SUCCEEDED(hr)) {
        memset(&desc, 0, sizeof desc);
        desc.Width = (UINT)w;
        desc.Height = (UINT)h;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown*)device, V.overlay, &desc, NULL, NULL, &V.chain);
        if (FAILED(hr)) {
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown*)device, V.overlay, &desc, NULL, NULL, &V.chain);
        }
        if (SUCCEEDED(hr)) {
            HWND top = GetAncestor(V.parent, GA_ROOT);
            IDXGIFactory2_MakeWindowAssociation(factory, top ? top : V.parent,
                                                DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        }
    }
    if (factory) IDXGIFactory2_Release(factory);
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxgi) IDXGIDevice_Release(dxgi);
    if (FAILED(hr)) {
        V.chain = NULL;
        return 0;
    }
    V.chain_w = w;
    V.chain_h = h;
    return 1;
}

static int ensure_target(void) {
    ID3D11Device* device = NULL;
    ID3D11DeviceContext* context = NULL;
    ID3D11Texture2D* back = NULL;
    HRESULT hr;
    if (V.rtv) return 1;
    if (!present_device(&device, &context)) return 0;
    hr = IDXGISwapChain1_GetBuffer(V.chain, 0, &IID_ID3D11Texture2D, (void**)&back);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource*)back, NULL, &V.rtv);
    if (back) ID3D11Texture2D_Release(back);
    return SUCCEEDED(hr);
}

/* The surface covers the game window's client; its frames are rendered at the client's shape inside the render box. */
static int ensure_overlay(HWND parent, const char** why) {
    RECT client;
    unsigned box_w = 0, box_h = 0;
    int cw, ch, ow, oh;
    ID3D11Device* device = NULL;
    ID3D11DeviceContext* context = NULL;
    if (!GetClientRect(parent, &client) || client.right <= 0 || client.bottom <= 0) {
        *why = "the game window has no client area";
        return 0;
    }
    cw = client.right;
    ch = client.bottom;
    if (V.overlay && (V.parent != parent || !IsWindow(V.overlay))) release_overlay();
    if (!V.gpu) {
        if (!present_device(&device, &context) || !video_gpu_create(device, context, &V.gpu)) {
            *why = video_gpu_error()[0] ? video_gpu_error() : "no Direct3D 11 device";
            return 0;
        }
    }
    if (!V.overlay) {
        V.parent = parent;
        V.overlay = create_overlay(parent, cw, ch);
        if (!V.overlay) {
            *why = "the video surface window could not be created";
            return 0;
        }
        V.client_w = cw;
        V.client_h = ch;
    } else if (cw != V.client_w || ch != V.client_h) {
        SetWindowPos(V.overlay, HWND_TOP, 0, 0, cw, ch, SWP_NOACTIVATE);
        V.client_w = cw;
        V.client_h = ch;
    }
    d3dr_render_box(&box_w, &box_h);
    vl_output_size(cw, ch, (int)box_w, (int)box_h, &ow, &oh);
    if (V.chain && (ow != V.chain_w || oh != V.chain_h)) {
        if (V.rtv) ID3D11RenderTargetView_Release(V.rtv);
        V.rtv = NULL;
        if (FAILED(IDXGISwapChain1_ResizeBuffers(V.chain, 0, (UINT)ow, (UINT)oh, DXGI_FORMAT_UNKNOWN, 0))) release_chain();
        else {
            V.chain_w = ow;
            V.chain_h = oh;
        }
    }
    if (!V.chain && !create_chain(ow, oh)) {
        *why = "the video swap chain could not be created";
        return 0;
    }
    if (!ensure_target()) {
        *why = "the video swap chain has no render target";
        return 0;
    }
    return 1;
}

static int read_player(player_t* p) {
    uint32_t smack = 0;
    memset(p, 0, sizeof *p);
    if (!guest_read32(V.player + PLAYER_SMACK, &smack) || !smack || !guest_readable(smack, SMACK_BYTES)) return 0;
    p->smack = smack;
    p->width = MEM32(smack + SMACK_WIDTH);
    p->shown = MEM32(smack + SMACK_HEIGHT);
    p->frames = MEM32(smack + SMACK_FRAMES);
    p->rate = (int32_t)MEM32(smack + SMACK_RATE);
    p->flags = MEM32(smack + SMACK_FLAGS);
    p->frame = MEM32(smack + SMACK_FRAME);
    p->left = (int32_t)MEM32(V.player + PLAYER_LEFT);
    p->top = (int32_t)MEM32(V.player + PLAYER_TOP);
    return 1;
}

static void log_start(const player_t* p, const vl_policy_t* policy, const vl_layout_t* l) {
    char stored[64] = "";
    if (V.logged_start) return;
    V.logged_start = 1;
    V.first = *p;
    if (policy->rows == VL_ROWS_REBUILD)
        _snprintf(stored, sizeof stored - 1, " (%ux%d stored, Smacker %s)", p->width, policy->stored_rows,
                  (p->flags & VL_SMK_Y_INTERLACED) ? "Y-interlaced" : "Y-doubled");
    host_log("video", "%s: %ux%u%s @ %.3g fps -> HD presentation %dx%d (picture %dx%d%s), deinterlace=%s, "
             "upscale=catmull-rom, colours=palette, dither=light, deblock=%s", V.name, p->width, p->shown, stored,
             1000.0 / vl_smk_ms_per_frame(p->rate), V.chain_w, V.chain_h, l->video.w, l->video.h,
             l->side >= 2 ? ", sides extended" : "", vl_policy_text(policy),
             policy->rows == VL_ROWS_PROGRESSIVE ? "adaptive 4x4" : "off");
}

static void check_stray_line(const video_gpu_frame_t* f, const player_t* p) {
    int lone;
    if (f->policy.rows != VL_ROWS_PROGRESSIVE || V.session.stray_line) return;
    lone = vl_lone_bottom_pixels(f->rows, f->stride, f->width, f->policy.shown_rows);
    if (vl_session_stray_line(&V.session, lone))
        host_log("video", "%s: frame %u: the last row is a stray line over black (%d pixels); its lone pixels take the "
                 "row above for the rest of the video", V.name, p->frame, lone);
}

static void log_finish(void) {
    const vl_session_t* s = &V.session;
    if (!s->started) return;
    if (s->last_frame >= 0 && (uint32_t)s->last_frame + 1 < V.first.frames)
        host_log("video", "%s finished: %u frames, %u dropped, max processing %.1f ms (ended at frame %d of %u)", V.name,
                 s->frames, s->dropped, s->max_ms, s->last_frame + 1, V.first.frames);
    else
        host_log("video", "%s finished: %u frames, %u dropped, max processing %.1f ms", V.name, s->frames, s->dropped,
                 s->max_ms);
}

static int show_black(HWND window, const char** why) {
    if (!ensure_overlay(window, why)) return 0;
    video_gpu_draw_black(V.gpu, V.rtv);
    if (FAILED(IDXGISwapChain1_Present(V.chain, 0, 0))) {
        *why = "Present of the video surface failed";
        return 0;
    }
    return 1;
}

static int show_frame(HWND window, const BYTE* bits, LONG pitch, const RECT* source, int rgb565, const uint16_t* gamma,
                      const player_t* p, const char** why) {
    video_gpu_frame_t f;
    const video_subtitle_image_t* subtitle;
    vl_policy_t policy = vl_policy_for(p->flags, p->shown);
    vl_layout_t layout;
    int screen_w = source->right - source->left, screen_h = source->bottom - source->top, first_row;
    if (!p->width || !p->shown || p->left < 0 || p->top < 0 || p->left + (int32_t)p->width > screen_w ||
        p->top + (int32_t)p->shown > screen_h) {
        *why = "the video does not fit the game's screen";
        return 0;
    }
    if (!ensure_overlay(window, why)) return 0;
    first_row = source->top + p->top + (policy.rows == VL_ROWS_REBUILD ? policy.parity : 0);
    memset(&f, 0, sizeof f);
    f.rows = bits + (size_t)first_row * (size_t)pitch + (size_t)(source->left + p->left) * 2;
    f.stride = policy.rows == VL_ROWS_REBUILD ? pitch * 2 : pitch;
    f.width = (int)p->width;
    f.policy = policy;
    f.rgb565 = rgb565;
    if (rgb565) {
        f.palette_changed = vl_palette_map_update(&s_palette, (const uint8_t*)ADDR(p->smack + SMACK_PALETTE));
        f.palette_map = s_palette.map;
    }
    f.gamma = gamma;
    f.smooth = 1;
    f.deblock = 1;
    check_stray_line(&f, p);
    f.stray_bottom_line = V.session.stray_line;
    layout = vl_layout(V.chain_w, V.chain_h, (int)p->width, (int)p->shown);
    if (!video_gpu_draw(V.gpu, &f, &layout, V.chain_w, V.chain_h, V.rtv)) {
        *why = video_gpu_error();
        return 0;
    }
    subtitle = video_subtitles_image(V.chain_w, V.chain_h, layout.video.x, layout.video.y, layout.video.w, layout.video.h);
    if (subtitle) d3dr_draw_video_subtitle(V.rtv, V.chain_w, V.chain_h, subtitle);
    if (FAILED(IDXGISwapChain1_Present(V.chain, 0, 0))) {
        *why = "Present of the video surface failed";
        return 0;
    }
    log_start(p, &policy, &layout);
    return 1;
}

int video_hd_present(HWND window, const BYTE* bits, LONG pitch, const RECT* source, int rgb565, const uint16_t* gamma) {
    uint32_t raster = 0;
    player_t p;
    vl_show_t show;
    const char* why = "unknown";
    double start;
    int done;
    if (!V.session.playing || V.broken || !V.player) return 0;
    if (!guest_read32(V.player + PLAYER_RASTER, &raster) || !guest_readable(V.player + PLAYER_BUFFER_PATH, 2)) return 0;
    show = vl_session_route(&V.session, d3dr_hardware(), raster && raster != NULL_RASTER,
                            MEM8(V.player + PLAYER_FIRST_PENDING) != 0, MEM8(V.player + PLAYER_BUFFER_PATH) != 0);
    if (show == VL_SHOW_CLASSIC || !window_host_frames_allowed()) return 0;
    if (!read_player(&p)) return 0;
    start = now_ms();
    done = show == VL_SHOW_BLACK ? show_black(window, &why)
                                 : show_frame(window, bits, pitch, source, rgb565, gamma, &p, &why);
    if (!done) {
        warn_once(why);
        vl_session_failed(&V.session);
        return 0;
    }
    vl_session_shown(&V.session, show, (int32_t)p.frame, now_ms() - start);
    window_host_frame_presented();
    return 1;
}

int video_hd_picture(int32_t* frame, int* x, int* y, int* w, int* h) {
    player_t p;
    if (!V.player || !read_player(&p) || !p.width || !p.shown) return 0;
    *frame = (int32_t)p.frame;
    *x = p.left;
    *y = p.top;
    *w = (int)p.width;
    *h = (int)p.shown;
    return 1;
}

/* The game's own picture was just presented under the video surface: once it is composed, the surface can go. */
void video_hd_main_presented(void) {
    static dwm_flush_fn flush;
    static int looked;
    if (!V.session.overlay || !vl_session_main_presented(&V.session)) return;
    if (!looked) {
        HMODULE dwm = LoadLibraryExA("dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        flush = dwm ? (dwm_flush_fn)GetProcAddress(dwm, "DwmFlush") : NULL;
        looked = 1;
    }
    if (flush) flush();
    if (V.overlay && IsWindow(V.overlay)) ShowWindow(V.overlay, SW_HIDE);
    release_overlay();
}

void video_hd_release(void) {
    if (V.session.playing && (V.overlay || V.gpu)) vl_session_failed(&V.session);
    V.session.overlay = 0;
    release_overlay();
}

void video_hd_force(int mode) { V.forced = (vl_play_mode_t)mode; }

static void play(void) {
    char guest_name[64];
    vl_play_mode_t mode;
    if (g_alias_entry) {
        V.play_lifted();
        return;
    }
    mode = vl_play_mode_take(&V.forced);
    if (!guest_read_string(MEM32(g_esp + 4), guest_name, sizeof guest_name)) guest_name[0] = 0;
    if (skip_intro_try(guest_name, mode != VL_PLAY_AUTO, V.play_lifted)) return;
    vl_display_name(guest_name, V.name, sizeof V.name);
    if (!vl_play_uses_session(mode)) {
        host_log("video", "%s: the game's own presentation (Video Test, ORIGINAL)", V.name);
        V.player = g_ecx;
        video_subtitles_begin(guest_name);
        V.play_lifted();
        video_subtitles_end();
        V.player = 0;
        host_log("video", "%s finished with the game's own presentation", V.name);
        return;
    }
    if (mode == VL_PLAY_HD && (!d3dr_hardware() || V.broken))
        host_log("video", "%s: HD requested, but %s: the game's own presentation is shown", V.name,
                 V.broken ? "HD video presentation is off" : "the software renderer is active");
    V.player = g_ecx;
    V.logged_start = 0;
    memset(&V.first, 0, sizeof V.first);
    vl_session_begin(&V.session);
    if (!d3dr_hardware()) host_log("video", "%s: the software renderer shows the game's own playback", V.name);
    video_subtitles_begin(guest_name);
    V.play_lifted();
    video_subtitles_end();
    vl_session_end(&V.session);
    log_finish();
    V.player = 0;
}

void video_hd_install(void) {
    if (guest_override_install(VIDEO_PLAY_VA, play, &V.play_lifted) && d3dr_hardware()) video_gpu_prepare();
}
