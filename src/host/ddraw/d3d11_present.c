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

#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>

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
} presenter_t;

static presenter_t s_p;
static int s_vsync = 1;

void present_set_vsync(int on) { s_vsync = on; }

static void log_hr(const char* what, HRESULT hr) { host_log("d3d11", "%s failed, HRESULT 0x%08lX", what, (unsigned long)hr); }

int present_adapter(dd_adapter_t* out) {
    static dd_adapter_t cached;
    static int state;
    IDXGIFactory1* factory = NULL;
    IDXGIAdapter1* adapter = NULL;
    memset(out, 0, sizeof *out);
    if (state == 0) {
        DXGI_ADAPTER_DESC1 desc;
        state = -1;
        if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&factory)) &&
            SUCCEEDED(IDXGIFactory1_EnumAdapters1(factory, 0, &adapter)) &&
            SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc))) {
            WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, cached.description, sizeof cached.description - 1, NULL, NULL);
            cached.vendor = desc.VendorId;
            cached.device = desc.DeviceId;
            cached.subsystem = desc.SubSysId;
            cached.revision = desc.Revision;
            cached.dedicated_video_memory = desc.DedicatedVideoMemory;
            IDXGIAdapter1_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &cached.driver_version);
            state = 1;
        }
        if (adapter) IDXGIAdapter1_Release(adapter);
        if (factory) IDXGIFactory1_Release(factory);
    }
    if (state != 1) return 0;
    *out = cached;
    return 1;
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

void present_release(void) {
    video_hd_release();
    video_test_release();
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
}

static int create_device(void) {
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_1,
    };
    D3D_FEATURE_LEVEL level;
    dd_adapter_t adapter;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &s_p.device, &level, &s_p.context);
    const char* kind = "hardware";
    if (FAILED(hr)) {
        log_hr("D3D11CreateDevice (hardware)", hr);
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               &s_p.device, &level, &s_p.context);
        kind = "WARP";
    }
    if (FAILED(hr)) {
        log_hr("D3D11CreateDevice (WARP)", hr);
        return 0;
    }
    present_adapter(&adapter);
    s_p.generation++;
    host_log("d3d11", "device created: %s, feature level %u.%u, adapter \"%s\"", kind, ((unsigned)level >> 12) & 0xF,
             ((unsigned)level >> 8) & 0xF, adapter.description);
    return 1;
}

int present_device(ID3D11Device** device, ID3D11DeviceContext** context) {
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
            present_release();
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
        present_release();
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
        present_release();
    } else if (FAILED(hr)) {
        log_hr("Present", hr);
    }
}

void present_frame(HWND window, const BYTE* bits, LONG pitch, const RECT* source, const DDPIXELFORMAT* format,
                   const DDGAMMARAMP* gamma, int gamma_changed, int vsync) {
    UINT width = (UINT)(source->right - source->left), height = (UINT)(source->bottom - source->top);
    HWND target = window_host_surface(window);
    ID3D11Texture2D* staging;
    ID3D11Texture2D* back = NULL;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    if (format->dwRGBBitCount != 16 || !width || !height || !window_host_frames_allowed()) return;
    if (IsIconic(window)) {
        Sleep(10);
        return;
    }
    if (d3dr_hardware()) {
        d3dr_present_dib(window, bits, pitch, source, format, gamma, vsync);
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
    ID3D11DeviceContext_Unmap(s_p.context, (ID3D11Resource*)staging, 0);
    hr = IDXGISwapChain_GetBuffer(s_p.swapchain, 0, &IID_ID3D11Texture2D, (void**)&back);
    if (FAILED(hr)) {
        handle_present_result(hr);
        return;
    }
    ID3D11DeviceContext_CopyResource(s_p.context, (ID3D11Resource*)back, (ID3D11Resource*)staging);
    if (video_test_visible()) {
        ID3D11RenderTargetView* view = NULL;
        if (SUCCEEDED(ID3D11Device_CreateRenderTargetView(s_p.device, (ID3D11Resource*)back, NULL, &view))) {
            video_test_overlay(s_p.device, s_p.context, view, (int)width, (int)height);
            ID3D11RenderTargetView_Release(view);
        }
    }
    ID3D11Texture2D_Release(back);
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
    return s_p.back_rtv;
}

int present_submit(int vsync) {
    HRESULT hr;
    LARGE_INTEGER start;
    if (!s_p.swapchain) return 0;
    if (s_p.back_rtv) video_test_overlay(s_p.device, s_p.context, s_p.back_rtv, (int)s_p.width, (int)s_p.height);
    QueryPerformanceCounter(&start);
    hr = IDXGISwapChain_Present(s_p.swapchain, vsync && s_vsync ? 1 : 0, 0);
    handle_present_result(hr);
    if (hr != S_OK) return 0;
    d3dr_frame_presented(ms_since(start));
    window_host_frame_presented();
    video_hd_main_presented();
    if (++s_p.frames == 1) host_log("d3d11", "first frame presented by the hardware path: %ux%u", s_p.width, s_p.height);
    return 1;
}
