// What the Display and Graphics pages show about this PC.
#pragma once

#include <string>
#include <vector>

#include "graphics_cards.h"
#include "jpt_gpu.h"
#include "jpt_monitors.h"

namespace jpt {

// The adapters as the game's renderer enumerates them (jpt_gpu_list): entry i is DXGI adapter i. `detail` gets the reason
// when none is usable.
std::vector<jpt_gpu_t> ListGpus(std::string* detail = nullptr);
// Bit n: n samples work for the game's colour and depth formats on adapter `index`. Creates a short-lived device.
unsigned GpuMsaaMask(int index);

std::vector<jpt_monitor_t> ListMonitors();

}  // namespace jpt
