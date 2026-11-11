// What the Display and Graphics pages show about this PC.
#pragma once

#include <string>
#include <vector>

#include "jpt_monitors.h"

namespace jpt {

struct GpuInfo {
    std::string name;            // the adapter the game's Direct3D 11 device uses (DXGI adapter 0); empty when none
    unsigned long long vram = 0; // dedicated video memory, bytes
    unsigned msaa_mask = 0;      // bit n: n samples work for the game's colour and depth formats
};

// Creates a short-lived device on the adapter to ask for its MSAA levels, then releases it.
GpuInfo QueryGpu();

std::vector<jpt_monitor_t> ListMonitors();

}  // namespace jpt
