#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include "jpt_gpu.h"

#pragma warning(push)
#pragma warning(disable : 4201) /* the SDK headers' nameless unions, under /W4 C */
#include <d3d11.h>
#pragma warning(pop)
#include <dxgi.h>
#include <stdio.h>
#include <string.h>

const int jpt_gpu_feature_levels[JPT_GPU_FEATURE_LEVEL_COUNT] = {
    D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_1,
};

static void adapter_name(IDXGIAdapter1* adapter, char* name) {
    DXGI_ADAPTER_DESC1 desc;
    size_t n;
    name[0] = 0;
    if (FAILED(IDXGIAdapter1_GetDesc1(adapter, &desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return;
    /* No device and no context: Direct3D only answers whether one could be created. */
    if (FAILED(D3D11CreateDevice((IDXGIAdapter*)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                                 (const D3D_FEATURE_LEVEL*)jpt_gpu_feature_levels,
                                 JPT_GPU_FEATURE_LEVEL_COUNT, D3D11_SDK_VERSION, NULL, NULL, NULL)))
        return;
    if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, JPT_GPU_NAME_MAX, NULL, NULL)) name[0] = 0;
    for (n = strlen(name); n && name[n - 1] == ' '; n--) name[n - 1] = 0;
}

int jpt_gpu_list(jpt_gpu_t* out, int cap) {
    IDXGIFactory1* factory = NULL;
    IDXGIAdapter1* adapter = NULL;
    int count = 0;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&factory))) return 0;
    while (count < cap && SUCCEEDED(IDXGIFactory1_EnumAdapters1(factory, (UINT)count, &adapter))) {
        adapter_name(adapter, out[count++].name);
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
    jpt_gpu_assign_labels(out, count);
    return count;
}

void jpt_gpu_assign_labels(jpt_gpu_t* gpus, int count) {
    for (int i = 0; i < count; i++) {
        int same = 1;
        gpus[i].label[0] = 0;
        if (!gpus[i].name[0]) continue;
        for (int j = 0; j < i; j++)
            if (!strcmp(gpus[j].name, gpus[i].name)) same++;
        if (same == 1) snprintf(gpus[i].label, sizeof gpus[i].label, "%s", gpus[i].name);
        else snprintf(gpus[i].label, sizeof gpus[i].label, "%s (%d)", gpus[i].name, same);
    }
}

int jpt_gpu_find(const jpt_gpu_t* gpus, int count, const char* request) {
    if (!request || !request[0]) return -1;
    for (int i = 0; i < count; i++)
        if (gpus[i].label[0] && !strcmp(gpus[i].label, request)) return i;
    return -1;
}

int jpt_gpu_resolve(const jpt_gpu_t* gpus, int count, const char* request) {
    const int found = jpt_gpu_find(gpus, count, request);
    if (found >= 0) return found;
    for (int i = 0; i < count; i++)
        if (gpus[i].label[0]) return i;
    return -1;
}
