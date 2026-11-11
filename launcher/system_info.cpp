#include "system_info.h"

#include <d3d11.h>
#include <dxgi.h>

namespace jpt {

GpuInfo QueryGpu() {
    GpuInfo info;
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return info;
    if (SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            char name[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name - 1, nullptr, nullptr);
            info.name = name;
            info.vram = desc.DedicatedVideoMemory;
        }
        ID3D11Device* device = nullptr;
        if (SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                        &device, nullptr, nullptr))) {
            for (UINT n = 2; n <= 8; n *= 2) {
                UINT color = 0, depth = 0;
                if (SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, n, &color)) && color &&
                    SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, n, &depth)) && depth)
                    info.msaa_mask |= 1u << n;
            }
            device->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return info;
}

std::vector<jpt_monitor_t> ListMonitors() {
    std::vector<jpt_monitor_t> monitors(JPT_MAX_MONITORS);
    monitors.resize(static_cast<size_t>(jpt_list_monitors(monitors.data(), JPT_MAX_MONITORS)));
    return monitors;
}

}  // namespace jpt
