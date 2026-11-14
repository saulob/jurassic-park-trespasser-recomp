# Intel ASSAO (Adaptive Screen Space Ambient Occlusion)

The D3D11 renderer's ambient occlusion. Upstream: <https://github.com/GameTechDev/ASSAO>, MIT licence
(`LICENSE`, Copyright (c) 2016 Intel). The repository is archived and has no tags, so the files are pinned to its
last commit:

    c21d57a95b43c700ee01a2eb18f5589d7becefb2 (2023-01-03, "Archiving Notice")

| File here | Upstream path | Upstream SHA-256 | Changed |
|---|---|---|---|
| `ASSAO.h` | `Projects/ASSAO/ASSAO/ASSAO.h` | `6ccf30419e5da1b6e0fa8cdfc7d92f56cc907297143b040518ff262a067d6030` | no |
| `ASSAO.hlsl` | `Projects/ASSAO/ASSAO/ASSAO.hlsl` | `90a8a5d226e58626b539d28cd6f0a1b017fb5873e21daff6075c55730e037a2d` | no |
| `ASSAODX11.cpp` | `Projects/ASSAO/ASSAO/ASSAODX11.cpp` | `4b24168cdc2f6d3d793fada8d772474b80dcc72c88eaedad9c588dcf15743a99` | yes, see below |
| `LICENSE` | `LICENSE` | `65443e5db00c9512fda4f380fe2def8a0596724288851c24054623ece3a00461` | no |

Only the effect itself is here: the sample application, its framework (`Modules/`), the demo, `ASSAOWrapper*` and
the binaries are not.

## Local change

`ASSAODX11.cpp`, `CompileShader`: when `ASSAO_SHADER_ERROR_HANDLER` is defined, a shader compilation error goes to
that function instead of `MessageBoxA` (the game must never open a dialog), and a missing error blob is not
dereferenced. Without the macro the upstream behaviour is unchanged. The algorithm, shaders, constants and resources
are untouched.

## How the port uses it

- CMake builds `ASSAODX11.cpp` as the `assao` static library with `NDEBUG` and
  `ASSAO_SHADER_ERROR_HANDLER=assao_shader_error`; `ASSAO.hlsl` is compiled into the executable as text and handed to
  `ASSAO_Effect::CreateInstance`, which compiles it with `d3dcompiler_47.dll`.
- `src/host/ddraw/assao_host.cpp` is the only code that sees ASSAO's types. It converts the game's depth buffer
  (multisampled or not) to the single-sample `R32_FLOAT` depth ASSAO reads, gives ASSAO the projection the game's
  vertices were made with (`src/host/ao_policy.c`), lets ASSAO reconstruct normals from depth (`NormalSRV` is null),
  draws ASSAO's output opaque into its own buffer and multiplies the frame by it where the renderer's stencil allows.
- Quality levels: Low, Medium, High and Ultra are ASSAO's own `QualityLevel` 0, 1, 2 and 3 (Ultra is its adaptive
  "Highest"); every other setting is `ASSAO_Settings`' default except the radius (1 m) and the fade-out
  (40-150 m), which follow Trespasser's metre units.
