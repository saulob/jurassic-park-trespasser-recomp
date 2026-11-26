/*
 * Shows the virtual display's front buffer in its window through D3D11. Swap chain buffers cannot
 * be 16 bpp, so each frame is expanded once, through a table built from the surface's real masks
 * and the gamma ramp, into a staging texture that is copied to the back buffer. The swap chain is
 * windowed (flip model where available) and DXGI stretches it to the window it was made for,
 * the game window's presentation surface, which the window host sizes to the display's aspect.
 * With --renderer d3d11 the device is shared with d3d11_renderer.c, which draws into the swap
 * chain itself; a frame counts only when Present succeeds.
 */
#define COBJMACROS
#include "d3d_legacy.h"
#include "host.h"
#include "jpt_gpu.h"
#include "jpt_video_subtitles.h"
#include "perf_sections.h"
#include "renderer_backend.h"
#include "text_layer.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>

#define STAGING_COUNT 2

typedef struct {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    IDXGISwapChain* swapchain;
    ID3D11Texture2D* staging[STAGING_COUNT];
    ID3D11RenderTargetView* back_rtv;
    int next_staging;
    unsigned generation;
    HWND window;
    UINT width, height;
    HWND failed_window;
    int frames;
    uint32_t* table;
    DDPIXELFORMAT table_format;
    int table_ready;
    RECT text_source;
    ID3D11Texture2D* text_tex;
    ID3D11ShaderResourceView* text_srv;
    UINT text_w, text_h;
    uint32_t* text_scratch;
    size_t text_scratch_cap;
} presenter_t;

static presenter_t s_p;
static const FLOAT k_black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
static int s_vsync = 1;
static char s_graphics_card[JPT_GPU_LABEL_MAX];
static dd_adapter_t s_adapter;
static int s_adapter_state;

void d3d11_present_set_vsync(int on) { s_vsync = on; }

void d3d11_present_set_graphics_card(const char* name) { snprintf(s_graphics_card, sizeof s_graphics_card, "%s", name ? name : ""); }

static void log_hr(const char* what, HRESULT hr) { host_log("d3d11", "%s failed, HRESULT 0x%08lX", what, (unsigned long)hr); }

/* The jpt_gpu_list index of the Graphics card setting's adapter, resolved once; -1 when no adapter is usable. */
static int chosen_adapter_index(void) {
    static int index = -2;
    jpt_gpu_t gpus[JPT_GPU_MAX];
    int count;
    if (index != -2) return index;
    count = jpt_gpu_list(gpus, JPT_GPU_MAX);
    index = jpt_gpu_resolve(gpus, count, s_graphics_card);
    if (index < 0) {
        host_log("d3d11", "no graphics card found: Direct3D 11 chooses the device");
        return index;
    }
    if (s_graphics_card[0] && strcmp(gpus[index].label, s_graphics_card))
        host_log("d3d11", "saved graphics card \"%s\" not available; using %s", s_graphics_card, gpus[index].label);
    host_log("d3d11", "graphics card: %s (DXGI adapter %d)", gpus[index].label, index);
    return index;
}

static IDXGIAdapter* open_adapter(int index) {
    IDXGIFactory1* factory = NULL;
    IDXGIAdapter1* adapter = NULL;
    if (index < 0 || FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&factory))) return NULL;
    if (FAILED(IDXGIFactory1_EnumAdapters1(factory, (UINT)index, &adapter))) adapter = NULL;
    IDXGIFactory1_Release(factory);
    return (IDXGIAdapter*)adapter;
}

static int describe_adapter(IDXGIAdapter* adapter) {
    DXGI_ADAPTER_DESC desc;
    if (!adapter || FAILED(IDXGIAdapter_GetDesc(adapter, &desc))) return 0;
    memset(&s_adapter, 0, sizeof s_adapter);
    WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, s_adapter.description, sizeof s_adapter.description - 1, NULL, NULL);
    s_adapter.vendor = desc.VendorId;
    s_adapter.device = desc.DeviceId;
    s_adapter.subsystem = desc.SubSysId;
    s_adapter.revision = desc.Revision;
    s_adapter.dedicated_video_memory = desc.DedicatedVideoMemory;
    IDXGIAdapter_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &s_adapter.driver_version);
    s_adapter_state = 1;
    return 1;
}

/* The device's adapter once it exists; before that, the one it will be created on. */
int d3d11_present_adapter(dd_adapter_t* out) {
    memset(out, 0, sizeof *out);
    if (!renderer_backend_guard_d3d11("d3d11_present_adapter")) return 0;
    if (!s_adapter_state) {
        IDXGIAdapter* adapter = open_adapter(chosen_adapter_index());
        if (!adapter) adapter = open_adapter(0);
        if (!describe_adapter(adapter)) s_adapter_state = -1;
        if (adapter) IDXGIAdapter_Release(adapter);
    }
    if (s_adapter_state != 1) return 0;
    *out = s_adapter;
    return 1;
}

static void describe_device_adapter(void) {
    IDXGIDevice* dxgi_device = NULL;
    IDXGIAdapter* adapter = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(s_p.device, &IID_IDXGIDevice, (void**)&dxgi_device)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_device, &adapter)))
        describe_adapter(adapter);
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxgi_device) IDXGIDevice_Release(dxgi_device);
}

static void release_staging(void) {
    for (int i = 0; i < STAGING_COUNT; i++)
        if (s_p.staging[i]) {
            ID3D11Texture2D_Release(s_p.staging[i]);
            s_p.staging[i] = NULL;
        }
}

static void release_back_view(void) {
    if (s_p.back_rtv) ID3D11RenderTargetView_Release(s_p.back_rtv);
    s_p.back_rtv = NULL;
}

static void release_text_layer(void) {
    if (s_p.text_srv) ID3D11ShaderResourceView_Release(s_p.text_srv);
    if (s_p.text_tex) ID3D11Texture2D_Release(s_p.text_tex);
    s_p.text_srv = NULL;
    s_p.text_tex = NULL;
    s_p.text_w = s_p.text_h = 0;
}

void d3d11_present_release(void) {
    int had_swapchain = s_p.swapchain != NULL;
    release_text_layer();
    video_hd_release();
    video_test_release();
    debug_menu_release();
    if (s_p.device) d3dr_device_lost();
    release_back_view();
    release_staging();
    if (s_p.swapchain) IDXGISwapChain_Release(s_p.swapchain);
    if (s_p.context) ID3D11DeviceContext_Release(s_p.context);
    if (s_p.device) ID3D11Device_Release(s_p.device);
    s_p.swapchain = NULL;
    s_p.context = NULL;
    s_p.device = NULL;
    s_p.window = NULL;
    s_p.width = s_p.height = 0;
    if (had_swapchain) window_host_presentation_target(0);
}

static HRESULT create_on(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, D3D_FEATURE_LEVEL* level) {
    return D3D11CreateDevice(adapter, type, NULL, 0, (const D3D_FEATURE_LEVEL*)jpt_gpu_feature_levels,
                             JPT_GPU_FEATURE_LEVEL_COUNT, D3D11_SDK_VERSION, &s_p.device, level, &s_p.context);
}

static int create_device(void) {
    if (!renderer_backend_guard_d3d11("d3d11_present::create_device")) return 0;
    D3D_FEATURE_LEVEL level;
    IDXGIAdapter* chosen = open_adapter(chosen_adapter_index());
    const char* kind = "hardware";
    HRESULT hr = E_FAIL;
    if (chosen) {
        /* An explicit adapter takes D3D_DRIVER_TYPE_UNKNOWN; HARDWARE is only valid with a NULL adapter. */
        hr = create_on(chosen, D3D_DRIVER_TYPE_UNKNOWN, &level);
        IDXGIAdapter_Release(chosen);
        if (FAILED(hr)) log_hr("D3D11CreateDevice (graphics card)", hr);
    }
    if (FAILED(hr)) {
        hr = create_on(NULL, D3D_DRIVER_TYPE_HARDWARE, &level);
        if (FAILED(hr)) log_hr("D3D11CreateDevice (hardware)", hr);
    }
    if (FAILED(hr)) {
        hr = create_on(NULL, D3D_DRIVER_TYPE_WARP, &level);
        kind = "WARP";
    }
    if (FAILED(hr)) {
        log_hr("D3D11CreateDevice (WARP)", hr);
        return 0;
    }
    describe_device_adapter();
    s_p.generation++;
    host_log("d3d11", "device created: %s, feature level %u.%u, adapter \"%s\"", kind, ((unsigned)level >> 12) & 0xF,
             ((unsigned)level >> 8) & 0xF, s_adapter.description);
    return 1;
}

int present_device(ID3D11Device** device, ID3D11DeviceContext** context) {
    if (!renderer_backend_guard_d3d11("d3d11_present::present_device")) return 0;
    if (!s_p.device && !create_device()) return 0;
    *device = s_p.device;
    *context = s_p.context;
    return 1;
}

unsigned present_device_generation(void) { return s_p.generation; }

static int create_swapchain(HWND window, UINT width, UINT height) {
    IDXGIDevice* dxgi_device = NULL;
    IDXGIAdapter* adapter = NULL;
    IDXGIFactory* factory = NULL;
    IDXGIFactory2* factory2 = NULL;
    HRESULT hr = ID3D11Device_QueryInterface(s_p.device, &IID_IDXGIDevice, (void**)&dxgi_device);
    const char* effect = NULL;
    if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
    if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void**)&factory);
    if (FAILED(hr)) {
        log_hr("DXGI factory lookup", hr);
    } else {
        if (SUCCEEDED(IDXGIFactory_QueryInterface(factory, &IID_IDXGIFactory2, (void**)&factory2))) {
            static const DXGI_SWAP_EFFECT effects[] = { DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL };
            static const char* const names[] = { "flip discard", "flip sequential" };
            for (int i = 0; i < 2 && !s_p.swapchain; i++) {
                DXGI_SWAP_CHAIN_DESC1 desc;
                IDXGISwapChain1* chain = NULL;
                memset(&desc, 0, sizeof desc);
                desc.Width = width;
                desc.Height = height;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                desc.BufferCount = 2;
                desc.Scaling = DXGI_SCALING_STRETCH;
                desc.SwapEffect = effects[i];
                desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                hr = IDXGIFactory2_CreateSwapChainForHwnd(factory2, (IUnknown*)s_p.device, window, &desc, NULL, NULL, &chain);
                if (SUCCEEDED(hr)) {
                    s_p.swapchain = (IDXGISwapChain*)chain;
                    effect = names[i];
                }
            }
        }
        if (!s_p.swapchain) {
            DXGI_SWAP_CHAIN_DESC desc;
            memset(&desc, 0, sizeof desc);
            desc.BufferDesc.Width = width;
            desc.BufferDesc.Height = height;
            desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 1;
            desc.OutputWindow = window;
            desc.Windowed = TRUE;
            desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            hr = IDXGIFactory_CreateSwapChain(factory, (IUnknown*)s_p.device, &desc, &s_p.swapchain);
            if (SUCCEEDED(hr)) effect = "discard (bitblt model)";
        }
        if (s_p.swapchain) {
            HWND top = GetAncestor(window, GA_ROOT);
            IDXGIFactory_MakeWindowAssociation(factory, top ? top : window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        } else {
            log_hr("swap chain creation", hr);
        }
    }
    if (factory2) IDXGIFactory2_Release(factory2);
    if (factory) IDXGIFactory_Release(factory);
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxgi_device) IDXGIDevice_Release(dxgi_device);
    if (!s_p.swapchain) return 0;
    host_log("d3d11", "windowed swap chain on window 0x%08X: %ux%u B8G8R8A8, %s, no exclusive full screen",
             (uint32_t)(uintptr_t)window, width, height, effect);
    window_host_presentation_target(1);
    return 1;
}

static double ms_since(LARGE_INTEGER start) {
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    return (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)f.QuadPart;
}

/* Only the software path needs staging textures; they are made on its first frame at this size. */
static int create_staging(UINT width, UINT height) {
    D3D11_TEXTURE2D_DESC desc;
    memset(&desc, 0, sizeof desc);
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    for (int i = 0; i < STAGING_COUNT; i++) {
        HRESULT hr = ID3D11Device_CreateTexture2D(s_p.device, &desc, NULL, &s_p.staging[i]);
        if (FAILED(hr)) {
            log_hr("staging texture creation", hr);
            return 0;
        }
    }
    return 1;
}

/* A change of window or size rebuilds what depends on it; a failure is reported once and that
 * window gets no more attempts rather than one every frame. */
static int ensure_resources(HWND window, UINT width, UINT height) {
    if (s_p.failed_window == window) return 0;
    if (s_p.swapchain && s_p.window == window && s_p.width == width && s_p.height == height) return 1;
    if (s_p.swapchain && s_p.window == window) {
        HRESULT hr;
        release_back_view();
        release_staging();
        hr = IDXGISwapChain_ResizeBuffers(s_p.swapchain, 0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            log_hr("ResizeBuffers", hr);
            d3d11_present_release();
        } else {
            s_p.width = width;
            s_p.height = height;
            host_log("d3d11", "swap chain resized to %ux%u", width, height);
            return 1;
        }
    }
    if (s_p.device) {
        release_back_view();
        release_staging();
        if (s_p.swapchain) IDXGISwapChain_Release(s_p.swapchain);
        s_p.swapchain = NULL;
    }
    if ((!s_p.device && !create_device()) || !create_swapchain(window, width, height)) {
        d3d11_present_release();
        s_p.failed_window = window;
        host_log("d3d11", "presentation is off for window 0x%08X: the game keeps running, but nothing reaches the screen",
                 (uint32_t)(uintptr_t)window);
        return 0;
    }
    s_p.window = window;
    s_p.width = width;
    s_p.height = height;
    return 1;
}

static DWORD mask_shift(DWORD mask) {
    DWORD shift = 0;
    while (mask && !((mask >> shift) & 1)) shift++;
    return shift;
}

static BYTE expand_channel(DWORD pixel, DWORD mask, const WORD* ramp) {
    DWORD shift = mask_shift(mask), max = mask >> shift;
    DWORD value = max ? (((pixel & mask) >> shift) * 255 + max / 2) / max : 0;
    return (BYTE)(ramp[value] >> 8);
}

static int build_table(const DDPIXELFORMAT* format, const DDGAMMARAMP* gamma) {
    if (!s_p.table) s_p.table = (uint32_t*)malloc(65536 * sizeof *s_p.table);
    if (!s_p.table) return 0;
    for (DWORD v = 0; v < 65536; v++)
        s_p.table[v] = 0xFF000000u | ((DWORD)expand_channel(v, format->dwRBitMask, gamma->red) << 16) |
                       ((DWORD)expand_channel(v, format->dwGBitMask, gamma->green) << 8) |
                       expand_channel(v, format->dwBBitMask, gamma->blue);
    s_p.table_format = *format;
    s_p.table_ready = 1;
    return 1;
}

static void handle_present_result(HRESULT hr) {
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        HRESULT reason = s_p.device ? ID3D11Device_GetDeviceRemovedReason(s_p.device) : hr;
        host_log("d3d11", "device lost (0x%08lX, reason 0x%08lX): recreating on the next frame", (unsigned long)hr,
                 (unsigned long)reason);
        d3d11_present_release();
    } else if (FAILED(hr)) {
        log_hr("Present", hr);
    }
}

int present_take_text_layer(void** texture, int* legacy_w, int* legacy_h, int* left, int* top, int* right, int* bottom) {
    tl_view_t v;
    tl_rect_t region;
    RECT source;
    const uint32_t* table = d3dr_hardware() ? d3dr_rgb565_table() : s_p.table;
    D3D11_BOX box;
    size_t pixels;
    if (!s_p.device || !s_p.context || !table || !tl_current(&v)) return 0;
    source = d3dr_hardware() ? (RECT){ 0, 0, (LONG)v.w, (LONG)v.h } : s_p.text_source;
    region = v.dirty;
    if (region.left < source.left) region.left = source.left;
    if (region.top < source.top) region.top = source.top;
    if (region.right > source.right) region.right = source.right;
    if (region.bottom > source.bottom) region.bottom = source.bottom;
    if (region.right <= region.left || region.bottom <= region.top) return 0;
    if (!s_p.text_tex || s_p.text_w != (UINT)(source.right - source.left) || s_p.text_h != (UINT)(source.bottom - source.top)) {
        D3D11_TEXTURE2D_DESC desc;
        release_text_layer();
        memset(&desc, 0, sizeof desc);
        desc.Width = (UINT)(source.right - source.left);
        desc.Height = (UINT)(source.bottom - source.top);
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_p.device, &desc, NULL, &s_p.text_tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(s_p.device, (ID3D11Resource*)s_p.text_tex, NULL, &s_p.text_srv))) {
            release_text_layer();
            return 0;
        }
        s_p.text_w = desc.Width;
        s_p.text_h = desc.Height;
    }
    pixels = (size_t)(region.right - region.left) * (size_t)(region.bottom - region.top);
    if (pixels > s_p.text_scratch_cap) {
        free(s_p.text_scratch);
        s_p.text_scratch = (uint32_t*)malloc(pixels * sizeof *s_p.text_scratch);
        s_p.text_scratch_cap = s_p.text_scratch ? pixels : 0;
        if (!s_p.text_scratch) return 0;
    }
    tl_expand(&region, table, s_p.text_scratch, (size_t)(region.right - region.left));
    box.left = (UINT)(region.left - source.left);
    box.top = (UINT)(region.top - source.top);
    box.front = 0;
    box.right = (UINT)(region.right - source.left);
    box.bottom = (UINT)(region.bottom - source.top);
    box.back = 1;
    ID3D11DeviceContext_UpdateSubresource(s_p.context, (ID3D11Resource*)s_p.text_tex, 0, &box, s_p.text_scratch,
                                          (UINT)(region.right - region.left) * 4, 0);
    *texture = s_p.text_srv;
    *legacy_w = (int)s_p.text_w;
    *legacy_h = (int)s_p.text_h;
    *left = (int)box.left;
    *top = (int)box.top;
    *right = (int)box.right;
    *bottom = (int)box.bottom;
    return 1;
}

void d3d11_present_frame(HWND window, const BYTE* bits, LONG pitch, const RECT* source, const DDPIXELFORMAT* format,
                         const DDGAMMARAMP* gamma, int gamma_changed, int vsync, unsigned generation) {
    UINT width = (UINT)(source->right - source->left), height = (UINT)(source->bottom - source->top);
    HWND target = window_host_surface(window);
    ID3D11Texture2D* staging;
    ID3D11Texture2D* back = NULL;
    D3D11_MAPPED_SUBRESOURCE mapped;
    const video_subtitle_image_t* subtitle;
    HRESULT hr;
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    s_p.text_source = *source;
    if (format->dwRGBBitCount != 16 || !width || !height || !window_host_frames_allowed()) return;
    if (IsIconic(window)) {
        Sleep(10);
        return;
    }
    if (d3dr_hardware()) {
        d3dr_present_dib(window, bits, pitch, source, format, gamma, vsync, generation);
        return;
    }
    if (!ensure_resources(target, width, height)) return;
    if (!s_p.staging[0] && !create_staging(width, height)) {
        release_staging();
        return;
    }
    if (gamma_changed || !s_p.table_ready || !dd_format_equal(&s_p.table_format, format))
        if (!build_table(format, gamma)) return;
    staging = s_p.staging[s_p.next_staging];
    s_p.next_staging = (s_p.next_staging + 1) % STAGING_COUNT;
    hr = ID3D11DeviceContext_Map(s_p.context, (ID3D11Resource*)staging, 0, D3D11_MAP_WRITE, 0, &mapped);
    if (FAILED(hr)) {
        log_hr("Map of the staging texture", hr);
        return;
    }
    for (UINT y = 0; y < height; y++) {
        const WORD* in = (const WORD*)(bits + (size_t)(source->top + (LONG)y) * pitch) + source->left;
        uint32_t* out = (uint32_t*)((BYTE*)mapped.pData + (size_t)y * mapped.RowPitch);
        for (UINT x = 0; x < width; x++) out[x] = s_p.table[in[x]];
    }
    subtitle = video_subtitles_for_screen((int)width, (int)height, (int)width, (int)height);
    if (subtitle)
        jvs_blend((uint32_t*)mapped.pData, (long)(mapped.RowPitch / 4), (int)width, (int)height, subtitle->pixels,
                  subtitle->w, subtitle->h, subtitle->x, subtitle->y);
    ID3D11DeviceContext_Unmap(s_p.context, (ID3D11Resource*)staging, 0);
    hr = IDXGISwapChain_GetBuffer(s_p.swapchain, 0, &IID_ID3D11Texture2D, (void**)&back);
    if (FAILED(hr)) {
        handle_present_result(hr);
        return;
    }
    ID3D11DeviceContext_CopyResource(s_p.context, (ID3D11Resource*)back, (ID3D11Resource*)staging);
    if (video_test_visible() || debug_menu_overlay_is_wanted()) {
        ID3D11RenderTargetView* view = NULL;
        if (SUCCEEDED(ID3D11Device_CreateRenderTargetView(s_p.device, (ID3D11Resource*)back, NULL, &view))) {
            video_test_overlay(s_p.device, s_p.context, view, (int)width, (int)height);
            int64_t section = perf_section_begin();
            debug_menu_overlay(s_p.device, s_p.context, view, (int)width, (int)height);
            perf_section_end(PS_HUD_DRAW, section);
            ID3D11RenderTargetView_Release(view);
        }
    }
    ID3D11Texture2D_Release(back);
    window_host_frame_commit(generation);
    hr = IDXGISwapChain_Present(s_p.swapchain, vsync && s_vsync ? 1 : 0, 0);
    handle_present_result(hr);
    if (hr != S_OK) return;
    d3dr_frame_presented(ms_since(start));
    window_host_frame_presented();
    video_hd_main_presented();
    if (++s_p.frames == 1)
        host_log("d3d11", "first frame presented: %ux%u RGB %lX/%lX/%lX expanded to B8G8R8A8", width, height,
                 format->dwRBitMask, format->dwGBitMask, format->dwBBitMask);
}

/* The hardware path draws into the back buffer itself; the window is the game window. */
ID3D11RenderTargetView* present_target(HWND window, UINT width, UINT height) {
    HWND target = window_host_surface(window);
    ID3D11Texture2D* back = NULL;
    HRESULT hr;
    if (IsIconic(window) || !width || !height || !ensure_resources(target, width, height)) return NULL;
    if (s_p.back_rtv) return s_p.back_rtv;
    hr = IDXGISwapChain_GetBuffer(s_p.swapchain, 0, &IID_ID3D11Texture2D, (void**)&back);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(s_p.device, (ID3D11Resource*)back, NULL, &s_p.back_rtv);
    if (back) ID3D11Texture2D_Release(back);
    if (FAILED(hr)) {
        log_hr("swap chain render target view", hr);
        return NULL;
    }
    /* New or resized buffers hold undefined pixels; the frame covers them all, the clear makes that independent of it. */
    ID3D11DeviceContext_ClearRenderTargetView(s_p.context, s_p.back_rtv, k_black);
    return s_p.back_rtv;
}

int present_submit(int vsync, unsigned generation) {
    HRESULT hr;
    LARGE_INTEGER start;
    if (!s_p.swapchain) return 0;
    if (s_p.back_rtv) {
        video_test_overlay(s_p.device, s_p.context, s_p.back_rtv, (int)s_p.width, (int)s_p.height);
        int64_t section = perf_section_begin();
        debug_menu_overlay(s_p.device, s_p.context, s_p.back_rtv, (int)s_p.width, (int)s_p.height);
        perf_section_end(PS_HUD_DRAW, section);
    }
    QueryPerformanceCounter(&start);
    window_host_frame_commit(generation);
    hr = IDXGISwapChain_Present(s_p.swapchain, vsync && s_vsync ? 1 : 0, 0);
    handle_present_result(hr);
    if (hr != S_OK) return 0;
    d3dr_frame_presented(ms_since(start));
    window_host_frame_presented();
    video_hd_main_presented();
    if (++s_p.frames == 1) host_log("d3d11", "first frame presented by the hardware path: %ux%u", s_p.width, s_p.height);
    return 1;
}
