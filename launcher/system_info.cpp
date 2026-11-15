#include "system_info.h"

#include <d3d11.h>
#include <dxgi.h>

namespace jpt {

std::vector<jpt_gpu_t> ListGpus() {
    std::vector<jpt_gpu_t> gpus(JPT_GPU_MAX);
    gpus.resize(static_cast<size_t>(jpt_gpu_list(gpus.data(), JPT_GPU_MAX)));
    return gpus;
}

std::vector<std::string> GpuChoices(const std::vector<jpt_gpu_t>& gpus) {
    std::vector<std::string> choices;
    for (const jpt_gpu_t& gpu : gpus) {
        if (gpu.label[0]) {
            choices.emplace_back(gpu.label);
        }
    }
    return choices;
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

std::vector<jpt_monitor_t> ListMonitors() {
    std::vector<jpt_monitor_t> monitors(JPT_MAX_MONITORS);
    monitors.resize(static_cast<size_t>(jpt_list_monitors(monitors.data(), JPT_MAX_MONITORS)));
    return monitors;
}

}  // namespace jpt
