# Khronos Vulkan C Headers

Pinned Vulkan C headers for the native Vulkan renderer (`src/vulkan/`), so builds on Windows and Linux do not depend on a locally installed Vulkan SDK.

- Upstream: <https://github.com/KhronosGroup/Vulkan-Headers>
- Version: `1.3.275` (`VK_HEADER_VERSION 275`), runtime API baseline `VK_API_VERSION_1_2`
- Licence: Apache License, Version 2.0 (`LICENSE.txt`)

| File here | Upstream path | SHA-256 | Changed |
|---|---|---|---|
| `vulkan/vk_platform.h` | `include/vulkan/vk_platform.h` | `c4cabbcf699c90cde344095ae54da9435d117951aad114ba3a540c5997d7906f` | no |
| `vulkan/vulkan.h` | `include/vulkan/vulkan.h` | `52297513cc6b6cc8db2921aa849f72e3f55d678b3a58a2c3eb297c02ded7a837` | no |
| `vulkan/vulkan_core.h` | `include/vulkan/vulkan_core.h` | `3dba3ef8adaff8f24e9080e74d365fc64b5fe53ec8bda607c29c103e15888fcf` | no |
| `LICENSE.txt` | Apache-2.0 | Standard Apache 2.0 licence text | no |

## How the port uses it

- Compiled with `VK_NO_PROTOTYPES` and loaded dynamically at runtime via SDL3 (`SDL_Vulkan_LoadLibrary` / `SDL_Vulkan_GetVkGetInstanceProcAddr`) with an `SDL_LoadObject` headless fallback; no hard link to `vulkan-1.lib` or `libvulkan.so.1`.
- Platform WSI headers (`vulkan_win32.h`, `vulkan_xlib.h`, `vulkan_wayland.h`) are not vendored because SDL3 owns surface creation (`SDL_Vulkan_CreateSurface`).
