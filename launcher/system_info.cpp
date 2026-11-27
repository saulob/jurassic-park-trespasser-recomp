#include "system_info.h"

#ifdef _WIN32
#include <d3d11.h>
#include <dxgi.h>
#else
#include <cstdio>

#include "vk_context.h"
#endif

namespace jpt {

#ifdef _WIN32

std::vector<jpt_gpu_t> ListGpus(std::string* detail) {
    std::vector<jpt_gpu_t> gpus(JPT_GPU_MAX);
    gpus.resize(static_cast<size_t>(jpt_gpu_list(gpus.data(), JPT_GPU_MAX)));
    if (detail && GpuChoices(gpus).empty()) {
        *detail = gpus.empty() ? "DXGI lists no adapter" : "no adapter creates a Direct3D 11 device";
    }
    return gpus;
}

unsigned GpuMsaaMask(int index) {
    unsigned mask = 0;
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    if (index < 0 || FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return 0;
    if (SUCCEEDED(factory->EnumAdapters1(static_cast<UINT>(index), &adapter))) {
        ID3D11Device* device = nullptr;
        if (SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                        &device, nullptr, nullptr))) {
            for (UINT n = 2; n <= 8; n *= 2) {
                UINT color = 0, depth = 0;
                if (SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, n, &color)) && color &&
                    SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, n, &depth)) && depth)
                    mask |= 1u << n;
            }
            device->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return mask;
}

#else

namespace {

// One headless Vulkan probe serves the card list and the sample counts. The game enumerates the devices the same way.
const jpt_vk_probe_result_t& Probe() {
    static jpt_vk_probe_result_t probe;
    static bool done = false;
    if (!done) {
        jpt_vk_probe_headless("", &probe);
        done = true;
    }
    return probe;
}

}  // namespace

std::vector<jpt_gpu_t> ListGpus(std::string* detail) {
    const jpt_vk_probe_result_t& probe = Probe();
    if (detail && !probe.available) *detail = std::string(jpt_vk_probe_status_name(probe.status)) + ": " + probe.summary;
    std::vector<jpt_gpu_t> gpus;
    for (int i = 0; i < probe.gpu_count && i < JPT_GPU_MAX; ++i) {
        const jpt_vk_gpu_info_t& info = probe.gpus[i];
        jpt_gpu_t gpu = {};
        if (info.usable) {
            std::snprintf(gpu.name, sizeof gpu.name, "%s", info.name);
            std::snprintf(gpu.label, sizeof gpu.label, "%s", info.label);
        }
        gpus.push_back(gpu);
    }
    return gpus;
}

unsigned GpuMsaaMask(int index) {
    const jpt_vk_probe_result_t& probe = Probe();
    if (index < 0 || index >= probe.gpu_count) return 0;
    const jpt_vk_gpu_info_t& info = probe.gpus[index];
    const unsigned both = info.msaa_color_counts & info.msaa_depth_counts;
    unsigned mask = 0;
    for (unsigned n = 2; n <= 8; n *= 2)
        if (both & n) mask |= 1u << n;
    return mask;
}

#endif

std::vector<jpt_monitor_t> ListMonitors() {
    std::vector<jpt_monitor_t> monitors(JPT_MAX_MONITORS);
    monitors.resize(static_cast<size_t>(jpt_list_monitors(monitors.data(), JPT_MAX_MONITORS)));
    return monitors;
}

}  // namespace jpt
