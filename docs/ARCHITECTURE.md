# Architecture

## What runs where

`trespasser_recomp.exe` is a 32-bit Windows process, the only executable of the port: it carries every lifted build and runs the one the selected game version needs. It runs on Windows 11 x64 through WoW64,
like any 32-bit program. It is not an emulator and it never opens an original executable: the guest's memory is laid out
from `guest.img`, the image the launcher installed from the player's disc (the original executables are development
inputs only), mapped as data and never executed as a program.

| Part of the original | How it runs here | Kind |
|---|---|---|
| `.text`: 17,767 entry points (11,737 function bodies, 6,030 alias entries into them): all game and engine code, CRT startup, `WinMain` | C generated from the x86 by pcrecomp's `lift32`, compiled by MSVC into the host | **statically recompiled** |
| `SelfMod`: 39,797 bytes of rasteriser inner loops | the original bytes, at their original address, executed by the CPU | **original machine code, native** |
| `.rdata`, `.data`, `.rsrc` | mapped at their original addresses, used in place | original data |
| `MSVCRT.dll`, `USER32`, `KERNEL32`, `GDI32`, `ADVAPI32`, `LZ32`, `WINMM`, `COMCTL32` | the real 32-bit system DLLs | **Windows** |
| `DDRAW` (DirectDraw) | a virtual 16 bpp display in the host, presented with D3D11 (`src/host/ddraw/`); the system `ddraw.dll` is loaded but its exports are routed away | **host implementation** |
| `MSVCIRT.DLL`, `SMACKW32.DLL`, `Processor.dll` | the original DLLs from the disc, loaded by the Windows loader | **original binaries** |
| `DSound.dll` | whatever `LoadLibrary` finds on the machine; `A3d.dll` is never loaded (the host answers "not found") | Windows, optional |
| Registry key `HKLM\Software\DreamWorks Interactive\Trespasser` | a text file, `user_data\saves\recomp-registry.txt`; `Installed Directory` (saves, level swap files) is `user_data\saves\`, `Data Drive` is `game\` | **host implementation** |
| The original installer | the launcher reads the player's disc image itself (docs/LAUNCHER.md) | **host implementation** |
| Image loading, import binding, calls between lifted and native code, threads | pcrecomp `native32` | host runtime |

This is a hybrid, and the table is the whole of it. Nothing interprets x86 instructions.

## Why a 32-bit host

The task names Windows 11 x64, and a 64-bit process was considered. It was rejected for the
first boot:

- The image has no relocation table, so it must sit at `0x00400000` and every pointer in it is
  32 bits wide. A 64-bit host has to translate every pointer, handle and structure that crosses
  an API call.
- 105 of the 297 imports are the C and C++ runtimes (`MSVCRT`, `MSVCIRT`), with `FILE*`,
  iostream objects and `this`-call methods. pcrecomp's 64-bit-capable layer, `win32hle`, has no
  MSVCRT, no iostreams and no Smacker, and it ends the run on guest SEH. All of that would have
  to be written and stubbed first.
- `SelfMod` cannot be lifted statically without first modelling every patch site. In a 32-bit
  process it simply runs, exactly as shipped.

In a 32-bit host the guest's structures are already the ones Windows expects, so imports are
forwarded rather than reimplemented, and the amount of invented behaviour stays close to zero.
A 64-bit build remains possible later, once behaviour is reproduced and there is a reference
to compare against.

## Components reused from pcrecomp

Fetched at a pinned commit into `external/pcrecomp` (MIT, not vendored).

| Component | Use |
|---|---|
| `tools/pe/*`, `tools/cpp/rtti.py` | PE analysis, RTTI and vtable recovery |
| `tools/disasm/disasm32.py` | function catalog |
| `tools/lift/lift32.py`, `generate.py` | x86-32 and x87 to C |
| `runtime/recomp32/recomp_types.h` | register, memory, flag and x87 model of the lifted code |
| `runtime/recomp32/image_loader.c` | maps the PE at its base |
| `runtime/native32/native32.c`, `x87crt.c` | import binding, the lifted-to-native bridge, the Windows-to-guest callback trampoline, the machine lock, guest threads, `_ftol`/`_CI*`/`_controlfp` |
| `runtime/recomp32/recomp_trace.c` | `--calltrace`, `--firsthit`, `--watch` and friends |

`native32` and `win32hle` were both evaluated. `recomp32/main.c` on its own is a 64-bit-style
host with a hand-written shim per import and was not used. `runtime/hybrid` targets partially
lifted programs; here all of `.text` is lifted, so its routing is not needed.

Written for this project: `tools/` (extraction, analysis and lift drivers) and `src/host/`.

## The ABI boundary

**Lifted to native.** `call [import]` becomes an indirect dispatch. `native32` copies the
argument slots to the real stack, calls the function, and reads the stack purge from how far
`esp` moved, so stdcall, cdecl and thiscall (with `ecx`) need no per-function table. A float
result in `st(0)` is moved onto the lifted x87 stack.

**Native to lifted.** `.text` is mapped non-executable. When Windows, the CRT or `MSVCIRT`
calls a window procedure, a thread start, a `qsort` comparator, an `atexit` handler or a guest
vtable slot, the fetch faults and `native32` redirects it to the lifted function.

**x87.** Lifted code keeps the x87 stack as eight doubles and its own control word, and rounds
`fistp` by that word. Arithmetic is done in double precision, not in 80-bit or in the 24-bit
precision the game may select. This is the main known fidelity gap and the first suspect for
any physics divergence.

**Structured exceptions.** The guest's `__try` frames are pushed onto a simulated TIB chain that
Windows never walks, so a fault is not delivered to `_except_handler3`. It is reported by the
host and ends the run, which is what bring-up needs. The game's own `error.log` path is
therefore not exercised.

**Threads.** One lock: a thread owns the lifted register file while it runs lifted code and
releases it around native calls. Each guest thread gets its own guest stack and TIB.

## SelfMod

`src/host/selfmod.c`. The section is mapped writable and executable with its original bytes.
A call into it:

1. loads all eight general registers from the lifted register file and switches to the guest
   stack, so the routine finds its return address and arguments where the original `call` put
   them;
2. rebuilds the real x87 stack from the lifted one and loads the guest's control word;
3. runs the routine natively, self-patching included;
4. reads the registers, the x87 stack and the control word back.

Writes from lifted `.text` into the section land in the real bytes, because guest addresses are
host addresses. The section's only calls out are `atexit` registrations, which re-enter lifted
code through the callback trampoline; the lifted stack pointer is parked 256 KB below the
native frame for that case.

Nothing in `SelfMod` is stubbed, skipped or replaced. Converting it to parameterised lifted
code is future work and is not needed for behaviour to be correct.

## Host shims

Eighteen imports are answered by the host instead of going straight to Windows. Each one is
complete for what the game does with it; none returns a made-up success.

| Import | Reason |
|---|---|
| `GetModuleHandleA(NULL)`, `GetModuleFileNameA` | the guest's instance is its own image, not the host exe |
| `RegOpenKeyExA`, `RegCreateKeyA`, `RegQueryValueExA`, `RegSetValueExA`, `RegDeleteValueA`, `RegCloseKey` | the game's key is a file; other keys are read-only pass-through; nothing is written to the real registry |
| `MessageBoxA`, `DialogBoxParamA`, `OutputDebugStringA` | forwarded; the text, dialog template and answer are logged |
| `CreateFileA`, `GetFileAttributesA`, `FindFirstFileA`, `LZOpenFileA`, `LoadLibraryA`, `fopen`, `_open` | forwarded unchanged; path and outcome are logged |

`native32` adds its own: `CreateThread`, `TerminateThread`, `VirtualAlloc`, `VirtualProtect`,
`RaiseException`, and the x87-argument CRT helpers.

The media check is satisfied because the file it looks for really is at the configured path.
No check is patched out.

## Video

The game loads `ddraw.dll` with `LoadLibraryA` and takes `DirectDrawCreate` from
`GetProcAddress`; every DirectDraw call is an indirect call from lifted code, so it passes the
host's `recomp_lookup_manual` (a call outside `.text` and outside the dispatch table's range goes straight to
the import bridge, skipping the search that cannot find it). There `src/host/ddraw/dd_dispatch.c` routes the addresses of
`ddraw.dll`'s exports to the host's own DirectDraw, so the system's DirectDraw objects are never
created. Windows 11's `ddraw.dll` has no 16 bpp display modes (`SetDisplayMode(640, 480, 16)`
returns `DDERR_UNSUPPORTED`, and the primary stays 32 bpp), and the renderer needs one.

`src/host/ddraw/` implements `IDirectDraw`, `IDirectDraw2`, `IDirectDraw4`,
`IDirectDrawSurface` 1 to 4, `IDirectDrawGammaControl`, `IDirectDrawClipper` and
`IDirectDrawPalette` over a virtual display that is always RGB565:

- `SetDisplayMode` sets the virtual mode (16 bpp, 320x240 up to the desktop's size, and the two renderers' gameplay
  modes at any size); the desktop's mode is never touched, and the window is not resized by it (see "Window").
- Surfaces are DIB sections in the requested format, so `Lock` and `GetDC` share the pixels. A
  primary with back buffers is a flip chain; `Flip` exchanges buffers.
- Every change to the front buffer (`Flip`, `Blt`/`BltFast` to it, `Unlock`, `ReleaseDC`) is
  presented by `d3d11_present.c`: a windowed DXGI swap chain (flip model where available) of the
  display's size, the frame expanded once from the surface's masks, through the gamma ramp, into a
  staging texture. DXGI stretches it to the presentation surface (see "Window"); the swap chain is
  resized only when the virtual mode changes, never for a window resize.
- Methods the game never calls return `DDERR_UNSUPPORTED` or the documented error and log why.
  With `--renderer software` Direct3D interfaces are refused with `E_NOINTERFACE`, which keeps
  the software renderer.

### Renderers

`--renderer software` (the default for now) is the original path: SelfMod rasterises into the
RGB565 back buffer and `d3d11_present.c` shows it. `--renderer d3d11` runs the game's own Direct3D 6
path on the GPU. The registry shim answers `Use D3D` (1 or 0) and `D3D GUID`
(`IID_IDirect3DHALDevice`) from the option, overriding stored values and storing nothing.
`docs/D3D_LEGACY_INVENTORY.md` lists every interface slot the game uses.

- `d3d_objects.c`: `IDirect3D3` (a face of the DirectDraw object), `IDirect3DDevice3`,
  `IDirect3DViewport3` and `IDirect3DTexture2` (a face of the texture surface). Unused methods and
  untranslated states are refused, logged once with their caller.
- `d3d11_renderer.c`, on the presenter's D3D11 device: the device's back buffer gets a GPU colour
  target at the render resolution (`render_resolution`, docs/LAUNCHER.md: a render box from the
  preset and the Aspect Ratio, and each target is the largest size of its surface's own shape inside
  it, so a 640x480 menu keeps 4:3 in a 16:9 box; `original` is the smallest 4:3 size covering the
  presentation area, up to 2560x1920) with `--msaa` samples (default 4, reduced to
  what `CheckMultisampleQualityLevels` allows) and a D24S8 depth buffer. Pre-transformed vertices
  are drawn by a shader pair that reproduces the fixed-function subset in use (TEXTUREMAPBLEND,
  specular, vertex fog, alpha test, colour key, flat shading); fans become indexed lists,
  consecutive draws with unchanged state are batched into one `DrawIndexed`, and buffers are
  reused rings. World textures are slices of arrays per size; a batch binds up to sixteen arrays and each vertex
  carries its slice, its array, its texture's alpha and colour-key flags and its sampler (wrap or clamp per axis
  among four bound samplers; another address mode uses the device's), so only a seventeenth array, a render state or
  a filter change ends a batch. Blend, depth, rasterizer and sampler states are cached, and a flush binds only
  what differs from the previous flush; shaders are compiled once.
- Textures are converted to B8G8R8A8 on upload, only the changed rectangle and only when the
  game wrote to them (Lock, Blt, `Load`); a `Load` (the image cache reloads whole pages) copies and uploads only the
  rows that differ from the texture's own copy. A texture uploaded on many consecutive frames keeps level 0 only and
  moves to a one-level array of its size, shared with the other textures of that size. Texture memory is allocated write-watched
  (`MEM_WRITE_WATCH`), so rows changed without any of those are found before each use, uploaded and
  logged (`changed without Lock/Unlock`). `--texture-filter` (default anisotropic, 16x) applies where
  the game asks for linear filtering. With bilinear or anisotropic filtering each texture gets a full mip chain
  built on the CPU from its single level (`d3dt_build_mips`): colour weighted by alpha so transparent texels never
  darken edges, binary alpha kept binary at the coverage of the level above (ordered dither), so alpha-tested
  foliage keeps its density. A texture re-uploaded in consecutive frames (water, impostors) becomes dynamic and keeps
  level 0 only. Each distinct sampler is logged once with the D3D11 filter and `MaxAnisotropy` actually bound.
- Engine limits (`engine_limits.c`, D3D11 only, `--original-engine-limits` to compare):
  - Objects: `fCullDistanceCombined` (`0x006634EC`, set to 1 by `SetGlobalPartitionData`) multiplies every squared
    cull distance, a level's own `Culling` included; the host makes it 1.4^2. `bShouldCache`'s minimum impostor
    distance (`0x0064E458`, ATX's ObjectClarityDrawDistance) is set per call from the partition's bounding sphere:
    a static instance or spatial group stays 3D while the sphere spans more than 256 px (the impostor's own size)
    with the main camera's projection (25-150 m, times CacheMul, never nearer than 1998's 15 m times CacheMul).
    Entities (`CEntity` and descendants, by vtable) and meshes that are not hardware-able keep 1998's 15 m. The
    partition tree is visited from the root: an instance kept 3D in a frame for a reason that passes later (too close,
    too big on screen, inside the 3D distance, parallax) keeps its parts 3D too, so it never splits between geometry
    and impostors; one that can never be cached (level flags, movement, volume) leaves its parts to their own
    decisions, as in 1998. A static mesh spanning under 1.5 px is not drawn.
  - Those CPU-drawn polygons (`cpu_polygons.c`) go through `CScreenRenderDWI::DrawPolygon` (`0x0055F540`), whose
    primitives copy the polygon to `arvRasterVertices` before their walker calls `CRasterVid::Lock` (`0x00503400`).
    The coordinates are relative to the game's view, which `CRasterWin::Lock` (`0x00504D20`) places
    `iOffsetPointer` (`+0x90`) bytes into the surface. One DirectDraw Lock spans a run of polygons; each claims the
    32 px tiles of its screen box, which alone are captured, and the Unlock composites the union of the boxes.
    Primitives proven to only store pixels (bump, texture linear/perspective, flat) read nothing; any other reads
    its box from the GPU, never the frame. `FlushBatch`'s Lock (`0x00555AB0`), a wait that touches no pixel, costs
    nothing. After a Flip the reference matches the other CPU buffer; a polygon run then captures lazily: only tiles
    claimed since the Flip are filled, compared and uploaded, until something makes the whole copy consistent.
  - Scene heaps (`engine_heaps.c`): `pvCommitMem` (`0x005F8B10`) ignores `VirtualAlloc`'s result, and
    `CDArray::bCommit` commits before it checks its capacity, so a shape asking past a heap's reservation left pages
    marked committed that were not (the 0x0051B90D fault of log 031). Commits are kept inside the reservation, and
    the four per-scene pipeline arrays (`0x00732EF0`/`F10`/`F28`/`F40`: vertices, polygon pointers, polygons, vertex
    pointers) and the terrain heaps are reserved 4x larger, capacity with them.
  - "Max Recommended Tex Dim" 128 -> 256 (world textures, impostors and terrain pages; the impostor scale branch
    still sees 128).
  - Texture memory (`iLargeMemAllocMB`) 24 -> 64 MB. The engine splits it with signed 32-bit products
    (`engine_texture_layout`) that wrap from 46 MB; the host corrects them in place: inside the split
    (`0x00506EB0`/`0x00506C00`) the image cache (`0x00547710`) gets min(40%, 20 MB) and the regular pool the rest,
    and `AllocateMemory` (`0x00514FE0`) gets its 85%/95% marks in 64-bit arithmetic. Unused textures are evicted only
    above the 95% mark.
  - Small texture surfaces share write-watched 1 MB arenas (`page_arena.c`) instead of a 64 KB address-space
    granule each.
- Terrain (`terrain_quality.c`): under Direct3D only `TrrNoDynTextureDist` applies (`TrrNoTextureDist` is
  software-only): beyond it, or when the terrain pages are full, a node draws a crop of the level's base texture.
  `CQuadRootTIN::Update` sees own textures out to the far clip (at most 1000 m) and tolerances capped at the
  community's 4 / 12 px scaled from 480 lines to the render height, for that call only. Terrain pages are 256 px in
  16 MB. The terrain's fixed heaps (`pHeapAlloc`, no overflow check) are reserved 4x.
- Texture residency: a texture surface the game frees with an unchanged GPU copy is parked under its content hash and
  adopted by a new surface with the same pixels and conversion (the game drops all its textures on every device
  release); parked textures last 30 s, 256 MB at most.
- Texture uploads (`texture_streaming.c`): each frame `UploadPolygonArray` (`0x0055A4E0`) links the textures of
  newly visible polygons until `GetTickCount() - start > 7`; the polygons it does not reach draw with another linked
  mip or, with none, as a fill of the texture's average colour (`GetBestMipLevels`, `0x0055A920`). With GetTickCount's
  15.6 ms steps that window was anywhere from one upload to 15 ms; inside the call the host's GetTickCount stands
  still for 10 ms of performance-counter time and then ends the loop (`engine_upload_clock`).
- Lifted code optimisation: every lifted file compiles at `/O2 /Ob1`, a /Od build included; `/Ob1` expands only the
  runtime's `inline` helpers (`_pop32`, the x87 ring, flags, `recomp_resolve`). Overrides do not depend on it or on the
  linker: each replaceable function has a stable entry (below). pcrecomp's runtime comes with `patches\pcrecomp`: the
  x87 stack as a ring (`RECOMP_X87_RING`, with `g_fp_top` still counted), its helpers force-inlined, a two-entry cache
  per indirect call site for lifted and shim targets (`RECOMP_ICALL_SITE_RANGE`) and an O(1) `native32_name`. The
  host's own Direct3D methods called per polygon run in place, without native32's bridge (`d3d_method_in_place`).
- The surface's DIB stays the CPU copy; `target_sync.c` keeps where the picture is (GPU target, CPU
  copy, or both, as WineD3D's resource locations). The game (OpenTrespasser `RenderDB.cpp`,
  `ScreenRenderDWI.cpp`) clears with `Clear(0)`, draws sky and terrain with Direct3D, then
  `BeginFrame` (`0x0055F1D0`) Locks only to hand the surface address to the span rasterisers, each
  software polygon Locks again before drawing (and may read the pixels under it, as water does),
  switching back to hardware Unlocks, and `FlushBatch` (`0x00555AB0`) Locks only to wait for the GPU.
  So the GPU picture is read back only for a Lock that may read it; the two pixel-free Locks,
  `DDLOCK_WRITEONLY`/`DISCARDCONTENTS` and `Blt` destinations start a capture instead: the CPU copy
  and a reference are filled with an unwritten marker, and later only pixels that differ from the
  reference (black ones included) are composited over the GPU target, point-sampled. Writes still
  pending reach the GPU before any readback, so a stale CPU copy never replaces the GPU picture. A
  target holding only a full clear is recreated on the CPU instead of read back; after a frame the
  CPU made (menus, videos) the front buffer holds the GPU picture and is copied instead. A target
  the GPU has never drawn (a new device on a surface the game drew) is uploaded whole at its first
  draw and presented by DirectDraw until then. A colour-fill `Blt` of the target is a GPU clear.
- A Lock of a target whose CPU copy owns its pages (the DIB) defers all of that to the first touch:
  the pages are made inaccessible, the fault of the first access performs the readback, capture or
  recreation, and an Unlock with no touch restores the previous state. The game's Lock/Unlock pairs
  that only wait for the GPU (`FlushBatch`, the hardware-water upload in `0x0060E590`) therefore read
  nothing back. GetDC and Blt still sync at once: GDI and the host access the pixels directly.
- Host overrides: every lifted function the host may replace is listed in `src/host/guest_override_list.h`
  (required or optional). `tools/run_lift.py` names its lifted body `<lift>_lifted_sub_<va>` (every symbol of a lift carries
  the lift's prefix, e.g. `lift116_`), and `guest_overrides.c` defines `<lift>_sub_<va>` for each of the three lifts' lists,
  a stable entry that jumps through a pointer holding the lifted body until an override is installed. Direct calls, alias
  entries and the dispatch table all name `<lift>_sub_<va>`, so an override reaches them with or without
  incremental linking; a list and a lift that disagree fail the link. Installs are refused until the executable's
  fingerprint is verified; one startup line counts them, and a required override that is missing stops the run
  (`tests/override_tests.c` links a fixture both ways).
- Lift hooks: `src/host/lift_hook_list.h` lists guest addresses inside lifted functions; `tools/run_lift.py` puts
  `RECOMP_LIFT_HOOK(lift_hook_<va>)` in front of the instruction at each one, in every lifted body that holds it (`<va>` is
  the 1.0.116 address in every lift), and the host defines the function once. A hook sees and may change the guest registers, flags and memory, then the instruction
  runs as lifted; a listed address no lifted body holds fails the lift. The official v1.1 fixes use them
  (`patch117_player.c`, `patch117_physics.c`).
- `guest_overrides.c` also installs the host kernels: the water texture rows (`0x0060E3F0`) and the CSimWaveVar2D step (`0x00638670`, `water_kernels.c`),
  the occlusion test (`0x00630D90`, `occlusion_kernel.c`) and the per-polygon Direct3D submission (`0x00559240`,
  `polygon_submit.c`: address modes, the lit and fogged D3DTLVERTEX fan, render states and DrawPrimitive, each call
  made through the device's vtable as the lifted code makes it). `tests/water_tests.c` runs them against the lifted
  C copied out of `src/recomp/gen`; `--lifted-water` keeps the lifted functions.
- `bump_spans.c` models the software renderer's bump-mapped scanlines (SelfMod `0x00640FE0`): the span records,
  the texel walk in closed form and the per-pixel colour, which `water_tests` checks against the game's own
  routine run from `game	respass.exe`, and which `d3d11_bump_shader_source` draws on the GPU. The renderer does
  not draw them yet: bump-mapped polygons still go through the CPU path.
- Texture formats come from the whole `DDPIXELFORMAT`: RGB565, X/A RGB1555, X/A RGB4444, X/A RGB8888
  and palettised 8-bit (converted through the surface's palette, re-uploaded when it changes).
  Anything else is logged once per layout (`unknown texture format`) and drawn untextured; the draw
  state classes say `none` for draws without a texture and `unusable` for those. A limited sample of
  draws that can only come out black (black texture content, an unusable texture, black vertex
  colour, full fog to a black fog colour) is logged as `dark draw` with the state behind it.
- Readbacks are packed to RGB565 on the GPU (`ps_pack565`) into an `R16_UINT` target, point-sampled
  so composited CPU pixels come back exactly (the pixel calibration keeps linear filtering), and the
  CPU only copies rows. Every readback is classified (`READBACK_LOCK_READ`, `_LOCK_WRITE`, `_GETDC`,
  `_PIXEL_CALIBRATION` for `sub_00513660`, `_BLT_SOURCE`, `_FRONT_BUFFER`, `_OTHER`), timed, sized and
  charged to the game function behind the raster Lock wrappers (`CRasterVid::Lock` `0x00503400`,
  `CRasterWin::Lock` `0x00504D20`), found in the indirect-call ring.
- Composites scan only the rectangles the CPU wrote (Lock rectangle, Blt destination, the whole
  surface for GetDC), skip unchanged rows with `memcmp`, and upload and draw only the box that
  actually changed.
- Render states: every state the game sets is translated (`docs/D3D_LEGACY_INVENTORY.md`), including
  `ZBIAS` (47, a depth bias towards the viewer of the current `ZFUNC`) and the sampling states of
  stages 1-7, whose `COLOROP` stays `DISABLE` as in Direct3D 6. Anything else is refused with
  `DDERR_INVALIDPARAMS`, logged once with its name and caller, and counted:
  `unsupported_render_states`, `unsupported_texture_stage_states` and `failed_d3d_calls` close the log.
- `Flip` of a chain whose back buffer the GPU drew presents the GPU target (gamma ramp applied in
  the shader); frames drawn only by the CPU (menus, video) are presented from the DIB, scaled.
- Ambient occlusion (`ambient_occlusion`, Off by default; `third_party/assao`, `assao_host.cpp`, `ao_policy.c`):
  Intel ASSAO, applied at `Flip` on the back buffer right after the 3D picture is copied there and before the game's
  UI layer, the HD menus and the host's ImGui overlays (HUD, Debug Menu, Cheats), so none of those is darkened. With
  feature level 11.0 the depth buffer is `R24G8_TYPELESS` (DSV `D24_UNORM_S8_UINT`, as before, plus depth and
  stencil views). ASSAO gets the depth as the game wrote it (`sz` = 0.1 / metres, `CCamera::SetProperties`
  `0x0051FF0B`) with the matching projection (the main camera's vertical tangent, Hor+, divided by its zoom; the
  target's own aspect) and reconstructs normals itself. While AO is on, the stencil marks every pixel without a depth
  of its own: CPU composites (game text, subtitles, CPU polygons), colour fills and draws without depth writes; only
  pixels left by depth-writing draws are darkened, and only on gameplay frames that drew 3D. Off creates nothing and
  draws nothing. The Debug Menu changes the level at run time; a failure (shaders, resources, device) logs and turns
  it Off.
- If the D3D11 device, feature level 10.0 or `d3dcompiler_47.dll` is missing, the host logs it and
  falls back to the software renderer before the game starts.
- The renderer is chosen once, before the game starts (`--renderer`, or the software fallback above), and stays
  until the game closes; no key or call changes it later. The game re-reads `Use D3D` each time it builds its display
  (`sub_005F6D70`, on every reactivation and level load); the registry shim always answers the renderer chosen at
  start. The game's own software fallback (`0x00503D8D`) only runs when its Direct3D set-up fails.
- `--debug-d3d-output texture|vertex-color|depth` makes every D3D11 draw return only its texture
  (magenta when untextured), its vertex colour or its depth, to see what reaches the pixel shader.
- After a Flip the target still holds the last frame, as a flipped DirectDraw back buffer holds an
  old one. The first CPU write after it takes a copy of the CPU picture, so only the pixels the game
  then changes are drawn over the target, not the stale CPU copy. In Direct3D the game still draws
  every polygon without a render type with the CPU into the locked back buffer
  (`CScreenRenderDWI` begins each frame with a Lock, `0x0055F1D0`), so CPU and GPU pictures
  alternate within a frame. Black pixels the CPU draws over the GPU picture, and among them those
  over non-black content, are counted and logged (gameplay and menus separately) with the game
  function behind the Lock and the operation.

`--ddraw system` restores the system `ddraw.dll`; `src/host/ddraw_trace.c` then observes its
calls. The game itself drops DirectDraw failures (`0x005053A0` reports to `0x00471710`, an empty
function), so the `ddraw` log lines are the only place they show.

### Videos

`CVideoWnd::Play(name)` (`0x00418AE0`) plays four Smacker files from `data\menu`: `tpassintro` at start-up
(`0x0040F2A0`), `newgame` from New Game before `BE.SCN` loads (`0x00415687`), `credits` from the main menu
(`0x004163FA`), and `win` then `credits` when the game is finished (state -2 of `0x0040F2E0`). All are SMK2, 8-bit
palette, 640 wide, 15 fps, 22050 Hz 16-bit stereo audio, on a 640x480 screen (`CGameWnd` slot 40, `0x0040BB50`,
restores 640x480 before the ending plays). The registry value `NoVideo` skips them: `Play` then pumps one message and
returns 1 without opening the file. Skip Intro (`skip_intro`) answers `NoVideo` with 1 for the `tpassintro` and
`newgame` calls only (`video_hd.c` wraps `Play`; `registry.c` answers), so their callers go on to the main menu and to
the level load as after the video; the credits, the ending and the Video Test's requests are never skipped (the Video
Test panel has no key since F5 became the Debug Menu).

The player opens the file with every audio track (`SMACKFILEHANDLE | SMACKNEEDVOLUME | SMACKTRACKS`), checks that the
primary has an `lpSurface` (always, here), decodes each frame with `SmackToBuffer(..., SMACKBUFFER565)` into a cleared
640x480 `CRasterMem` with the picture centred, copies Smacker's changed rectangles into the locked primary
(`0x004188C0`), and the primary's `Unlock` presents it. `SmackWait` paces the frames against Smacker's own audio
clock. Any key-up but Print Screen, or a left or right button-up, ends a video after its eighth frame; so do Esc and
Space through the window's key handler (`0x00418F60`).

`newgame.smk` is flagged Y-interlaced: it stores 172 rows and the decoder writes them to every second row of a 344-row
band, leaving the rows between them as the cleared raster's black. Those black rows are the horizontal lines of that
video. It is not interlaced (there is no second field). The other three are progressive; what remains in them is the
palette's ordered dither and Smacker's 4x4 blocks: in `tpassintro.smk` 77% of the blocks are one colour, and the mean
luma step across block edges is 2.2-2.5 times the step inside blocks (sky, clouds, dark areas). `credits.smk` carries
a stray line in its last row (row 347, x 274-639, grey 32 at full brightness, fading with each card) over a black band:
it is in the bitstream (SMACKW32 and FFmpeg decode it), from frame 11 on every lit pixel of that row stands alone
over the black rest of its block, and the other files never have more than 35 such pixels.

With D3D11, `src/host/video_hd.c` wraps `Play` (an optional override). While it runs, each present of the screen is
drawn on a child surface covering the whole client instead of the game's 4:3 surface (`src/host/ddraw/video_gpu.c`):
the RGB565 codes mapped back to the frame's palette colours, a Y-interlaced file's missing rows rebuilt edge-directed
from the rows above and below, and in progressive files the stray line's pixels taking the row above once a frame shows
128 or more of them (latched for that playback); then, at the picture's own size, the block edges softened (each edge
judged over its four lines: a step below 32 levels, larger than every change inside both blocks and never against
black, is spread over the two blocks as between their centres; newgame is left as it was), a light smoothing of the
dither (only pixels above or below both neighbours of a pair, up to the palette's step), then a Catmull-Rom scale
(clamped to its four nearest texels) to the largest size of the picture's shape, black bars above and below, and on
wider screens sides that extend the picture's edge colours, blurred and darkened. No frame waits for another: every
pass reads only the frame being shown, so timing, audio and skipping stay the game's own. The surface renders at the client's shape inside the render box, never above the client's size.
The player's three black clears come before its first frame; after the last one the frame stays until the game's own
surface has presented and the compositor has shown it (`DwmFlush`), then the surface is hidden and released. A failure
logs one line and leaves the rest to the game's own playback, as does the software renderer.

Video Test (development, `src/host/video_test.c`): F5 with the main menu alone in front opens an ImGui panel (drawn
into the back buffer before Present; elsewhere F5 does nothing) listing the four files with Original and HD. A click is a request; the main menu's next frame (`0x00417300`, before the game draws it) runs what the Options
window's Credits button runs (`0x004163D5`): menu music and its timer off, a `CVideoWnd` on the game's stack, `Play`
with the game's own name string, music and timer back. The request forces that one `Play`: ORIGINAL starts no HD
session (every present is the game's own), HD the normal one; the next `Play` is AUTO again.

## Window

`src/host/window_host.c`. The game creates one popup window at (0,0), 640x480, and from then on
treats it as the whole screen. `CreateWindowExA` is shimmed, so that popup is created as an
ordinary overlapped window (title bar, sizing frame, minimize/maximize/close), centered in the work
area of the Monitor setting's monitor, at the Window size setting (pixels) or, with Auto, at that
monitor's scale (640x480 at 100%, 1440x1080 at 225%), reduced to fit keeping its shape. With
Display mode Borderless Fullscreen it starts covering that monitor. The HWND is never recreated: the host subclasses it, and the game's own
window procedure still receives the messages it received before.

- DPI: the process is per-monitor v2 aware (`SetProcessDpiAwarenessContext`, before any window
  exists), so window, cursor and monitor coordinates are physical pixels. `WM_DPICHANGED` applies
  the suggested rectangle; frame size, work area and minimum size use the window's own DPI.
- Size: with Window size Auto and Letterbox, `WM_SIZING` keeps the client at the display's aspect
  ratio inside the monitor's work area (a fixed Window size is resized freely and letterboxed); `WM_GETMINMAXINFO` sets a minimum client width of 320
  pixels at 96 dpi. Maximizing fills the work area; the picture keeps its aspect ratio and the
  rest is black.
- Presentation surface: the swap chain belongs to a child window that fills the largest area of
  the client with the display's aspect ratio, centered (Presentation Stretch: the whole client;
  the cursor mapping follows the same rectangle). DXGI stretches the swap chain to it, so
  scaling needs no shader and no `ResizeBuffers`. The child answers `WM_NCHITTEST` with
  `HTTRANSPARENT`, so mouse messages reach the game's window. The rest of the client is painted black.
- F11 toggles windowed and borderless full screen. Entering saves the placement
  (`GetWindowPlacement`, which includes the maximized state) and the style, drops the frame and
  covers the current monitor's rectangle. The desktop mode never changes and there is no exclusive
  full screen. Leaving restores the style and calls `SetWindowPlacement`. Alt+Enter is disabled
  in DXGI. Each switch saves `display_mode` to `user\trespasser.ini`.
- What the game may no longer do to its window: `SetWindowLongA` of the style, `SetWindowPos`,
  `MoveWindow` and `SetWindowTextA` on that HWND are acknowledged and ignored (logged once each),
  so a mode change cannot undo the host's style, size or title. `ShowWindow(SW_RESTORE)` on a window
  that is not minimised (the game's reactivation) is ignored so a maximised window stays so. The
  game's `SetCapture` on its window (held for its whole run) is kept virtual, and its
  `EnableMenuItem` greying of Close and Move in the system menu is ignored, so the title bar, the X
  button and Alt+F4 work. Calls on other windows pass through. `WM_SYSCOMMAND` size, move,
  minimize, maximize and restore go straight to `DefWindowProc`.

### Keys

`F1 Hint | F2 Controls | F3 Objectives | F4 Show Objective | F5 Debug Menu | F10 Cheats | F11 Fullscreen | Esc Menu`
(logged once as `[hotkeys]`). F2, F3, F4, F5, F10, F11 without Ctrl and Alt+F4 belong to the host and never reach the
game (F10's key-up included, which would open the window's menu bar); F2 opens and closes the port's controls card in a level, as the gamepad's View does (`CGameWnd::OnKey`'s
table at `0x0040C220` gives F2, F3 and F4 no action: only Esc, Space, F1, F11, F12, `=` and `-` have one). F4 alone is
Show Objective in gameplay (`debug_menu_show_objective`, D-pad Down's own); with Alt it stays Exit. F12, which opened the game's `CControlsWnd` (still under Options >
Controls), is not passed on. Ctrl+F11 (the game's cheat box) and everything else go to the game unchanged. Esc and F1
open a modal window of the game on key-down (`CGameWnd::OnKey`, `0x0040BE70`), but `CUIWnd::OnKey`
(`0x0040AF50`) cancels a window on Esc key-up. The key-up of the press that opened a window (the
game's window stack grew between the key-down and it) is kept from the game, once per press; the
next key-down decides again, so a second Esc closes the menu normally, and a key-up the game
already flushed while opening (`sub_004131D0`) cannot make a later one disappear. Auto-repeated
key-downs are dropped so a held key cannot open, close and reopen it. Key lines are capped.

### Debug / Test Menu

`debug_menu.c` (policy in `debug_menu_policy.c`, ImGui panel in `debug_menu_panel.cpp`) puts the game's own debug paths
behind F5. The retail cheat table (`0x00660180`, dispatcher `0x0040B6A0`, jump table `0x0040B8DC`) keeps `LOC`
(toggles `0x0066DE50`; `CGameWnd` then prints the active camera's presence, `CWorld + 0x14`, vtable `+ 0x10`), `TNEXT`
(`PlayerTeleportToNextLocation`, `0x0060CB70`) and `DINOS` (`CAISystem::bBoring`, `[0x0066E77C] + 0x52`); `TELE`'s case
returns without acting (its body is `#if 0` in the source). Teleport is that body built from TNEXT's own calls:
`PurgeRenderCaches` `0x00548150`, `p3Placement` (vtable `+ 0x18`) and `Move` (`+ 0x38`) on the camera's attached mover
(`camera + 0x50`), `SetNoPageFrames(2)` `0x006264D0`. Previous Key Point puts TNEXT's iterator (`0x0073F614`, list
sentinel at `0x0073F600`, nodes `{next, prev, CTeleport*}`) on the node before the last one reached and runs TNEXT
itself. Restart Level is the death and pause-menu sequence (`0x0040BCC0` pause, `0x00414480` on `0x0066E338`: scene
reloaded in place by `bLoadScene`, `0x0040BD70(1)` resume). Reload and Level Select in a level are
`CWorld::DeferredLoad` (`0x00607E90`), the game's own level change: `CGameWnd`'s frame (`0x0040C4AD`) shows "Loading
Level" and runs the full load `0x00414100` (display reset, loading screen). From the main menu Level Select runs what
Direct Load's OK and New Game run (`0x00414D80`, `0x00414100`, the main menu's EndModal(2)); its list is Direct Load's,
`data\*.scn`. Requests run in the modal UI loop before it reads its cursor (`0x004178B5`); while the menu is open in a
level the `CInput` result is emptied after each read. Coordinates are the mover's `CPlacement3`; yaw is the heading of
`(0, 1, 0)` turned as LOC's `0x004CD090` turns it. The checkpoint holds a level id and a placement, never a guest
pointer, and is dropped when another level or the main menu is seen.

### One interface at a time

`overlay.c` (policy in `overlay_policy.c`) owns the host's interfaces over gameplay: the controls card (an entry of the
game's `CTextOverlay`, category `0x4A50`, so it stays on the frozen frame under any window unless it is expired
first), Objectives, the Debug Menu and Cheats. F2, F3, F5, F10, View, D-pad Left/Right are requests, run in the UI
loop: the interface up closes, then the next opens; the game's Hint (F1, or D-pad Left posting the game's own F1) is let
through one pass after the card has left the overlay. Keys a host interface used are kept from the game until released.
While any interface is up the CInput result (look, move, held commands, presses) is emptied, which the game reads as
Move Hand let go. When gameplay blocks or unblocks, and when it resumes after a window of the game (Hint, pause menu,
dialog), the pad's held buttons wait for their release (`pad_play_neutral`), each stick for its own centre, and the
SInput command bits held at that moment stay masked until released (`ov_commands_*`): the A that closed Objectives does
not jump, and a held LT or mouse button does not raise the hand again until it is pressed anew. A long frame with no
window keeps a held LT, as before. `[input-hand]` lines record Move Hand against LT and the mouse on each change.

### Compass and objectives

`compass_policy.c` (angles, distance, layout, the camera projection, the trail's progress) and the Debug Menu's sample
of the world drive the HUD: Anne is the camera's mover (the attached instance, or the one the Free Camera left), the
heading is `dm_yaw` of the active camera's presence (`camera + 0x30`, as `CInstance::Move` reads it), the projection
uses that presence and the main camera's vertical view (`widescreen.c`: stored tan(half width) over the 4:3 aspect).
World units are metres. The HUD is laid out on the 3D image the game draws, not on the whole surface: the D3D11 renderer
keeps the viewport of the last depth-tested scene (`d3dr_scene_view`), `cp_layout_in` sizes the strip by that image and glues it
to its top edge, and `cp_project_in` projects the marker and the trail on it. The image is the surface less the game's own
Screen Size margin (`XView`/`YView`, each step 6.6% of the size, centred: guest `0x00412BC0`, `0x005F7980`, `0x00504CC0`, the
same in the four builds), so it fills the surface for 0 and is 3588x2018 inside 3840x2160 for 1. The port hides the game's
legacy Video options, so the host answers both values 0 for every version (`registry_view_margin_name`) and a margin stored
by an earlier build is ignored: the image is always the resolution the launcher chose. `compass_tests` reproduces that arithmetic against the logged camera sizes and checks the four versions at
1920x1080, 2560x1440 and 3840x2160.

Objectives (`objective_policy.c`, `objectives.c`) follow `CSetHintAction::iCurrentHint` (`0x0066F01C`), the value
`CHintWnd` (`0x00416BF0`, opened by `CGameWnd::OnKey`'s F1 case `0x0040C002`) shows as `LoadString(g_hInst
[0x0066E05C], 18000 + hint)`. The levels' triggers set it (ActionType 28), every `CSetHintAction` saves and loads it,
so a loaded game resumes its objective; the F3 list shows the same string resource F1 shows. Each objective's target
is an instance named in the table and found after the level loads by the game's own `u4Hash` (`0x005F6100`) and
`CWorld::pinsFindInstance` (`0x00607C10`), then its `p3Placement`; positions are kept, never pointers, and are found
again after any window, load or level change (moving objects, such as the green keycard, every second). The table was
built from the retail level data with `tools/scn_triggers.py` (a `.scn` names its SZDD-compressed `.GRF`, whose value
table holds every trigger and action) and `tests/test_objectives_retail.py` checks every name, hint and route against
it. Route anchors are the levels' own `Teleport` points, used only where they close in on the target (or climb, on the
ascent levels). The objective's title is not drawn all the time: Show Objective (D-pad down, host side, edge press)
runs `cp_reminder_*` (4 s, the last 400 ms fading; a press or a new objective restarts it; "No objective" for 2 s) and
the title is drawn with a thin ring outline (`titled_text`, `cp_outline_px`). The game's text overlay (`[0x0066F03C]`)
is read without calls and each entry classified by `cp_text_kind`: the add routine's category (voice subtitles from
`0x004395A0` are category 2, laid out bottom-anchored, flags `0x11`; trigger texts from `0x00472B60` are category 3 with
the scene's own flags), the port's own categories (`0x4A50` card, `0x4A51` hint) and where its first laid-out line
landed in the raster (the top 30 %). Subtitles and small messages lower on the picture leave the HUD alone; the F2 card,
the port's hint and any other top text hide it.

### Cheats

F10 (`cheats.c`, policy in `cheats_policy.c`) is a second item list of the Debug Menu's policy: Teleport, Level Select
and Freeze Dinosaurs are the Debug Menu's items and actions. Its rows, labels and choices are the cheats of
`src/common/jpt_cheats.c`, the one list the launcher's Cheats page and the settings table read too. The launcher's
saved values start each session (`cheats_start`, from `jpt_config_t.cheat`); F10 only records its changes over them
(`jpt_cheat_session_t`) and never writes the settings file. The game's own flags (INVUL, WOO, DINOS) take the effective
values once per level, load or player, never every frame, so the game's console cheats still work over them. The cheats
are the game's own state:
`CPlayer::bInvulnerable` (`+ 0xDD`; retail still returns from `CPlayer::Process(CMessageCollision)` on it at
`0x004569D7`, though the INVUL console case was compiled out), `g_bUnlimitedAmmo` (`0x0066ECC1`, WOO, read by `CGun` at
`0x0043FA5B`), `CAnimate` hit points `+ 0x50` and maximum `+ 0x54`, the player's own `CPhysicsInfoPlayer`
(`[[player + 0x28] + 0x0C]`, vtable `0x0064D644`) `rSpeedMultiplier` `+ 0x70`, which only the walk request multiplies
(`0x004F0B27`), and the voluntary jump's `Pelvis_Jump.z` (`0x0067AFA8`, flag `0x0067AF5D`), scaled by the square root of
the height multiplier in the existing `Pelvic_IDOF` override before `Foot_IDOF` turns it into the starting speed. The
camera is a `CEntityAttached` (`+ 0x50` attached, `+ 0x54` animate, `+ 0x58` relative placement, followed by
`UpdateFromAttached` `0x00617F20`): Free Camera clears the attachment and moves the camera with `CInstance::Move` (`0x00614B10`) from the `CInput` result it takes from Anne. The
host-side cheats are written into the game only in a running level with no pending load (`CWorld + 0x38`) or save
(`+ 0x44`); otherwise the game's values are put back, so saves (which keep hit points and the camera's link) and
loads never see them. The free camera resets in another level or the main menu; the other cheats stay for the session.

Above 1x Movement Speed the right hand is carried with Anne. Pel (`0x6CF198 + 0x300 i`, 12 bytes per DOF: value,
velocity, acceleration) is world space. For a human, `Pelvic_IDOF` rebuilds BODY (DOF 0-2) every step as the foot
(DOF 7, the driven root) plus the crouch height along the body's up axis, with the foot's velocity, and sets BODY's
acceleration to the foot's (`0x004F448D`..`0x004F4737`); Foot_IDOF also changes the foot's velocity directly (the part
into the ground on contact, `0x004E2E6B`; the jump, `0x004E7903`). The hand (DOF 25-27) is the only world-space DOF that
chases the body: `Update_Right_Arm` (`0x005001D0`) springs it towards body + R(q) rest pose through `Mass_Solution`
(`0x004F94E0`, capped at 150 m/s^2), damping `v_hand - 1.25 v_body`; past 1.5 arm lengths from the shoulder it sets
HandDrop (`0x6CDA70`). The left foot (DOF 14) does not move for a human. With k = 1 - 1/m, the `Pelvic_IDOF` override
(`hand_carrier_step`) gives the hand, before the arm reads it, k times the body's measured velocity change and k times
the displacement its carried velocity did not already give it, so the carrier has always moved the hand by exactly k
times the body's displacement; the controller sees `v_hand - 1.25 v_body + 0.25 k v_body`, i.e. the 1x controller
relative to the carrier. The earlier version fed k times BODY's acceleration instead, which misses every direct velocity
change: on contact-bound ground the hand ran at the edge of the drop distance and was eventually let go (log 073: tens
of metres behind, then the 60 m query-region check deactivated the pelvis). 1x and below run the game's arithmetic.

### Closing

The game cannot close itself from the window: WinMain greys out `SC_CLOSE` (`0x0040DE2C`), its
`WM_CLOSE` handler is an empty function (`0x00476FE0`, `ret 4`) and its loops never look at
`WM_QUIT`. It leaves only through its own UI: every window runs `CUIWnd::DoModal` (`0x004175C0`),
whose loop (`0x00417840`) ends when `EndModal(code)` (`0x004176F0`: done byte `+8`, result `+0xC`)
is called; the screen state machine (`0x0040F2E0`) returns to WinMain on code -1; WinMain then
releases DirectDraw, destroys its window, posts `WM_QUIT`, clears registry `Safemode` and returns 1,
and the CRT calls `exit(1)`. The menu's Exit is `CYesNoMsgDlg` yes then `CMainScreenWnd` EndModal(-1)
(`0x0041577E`); the in-game Quit is `CInGameOptionsWnd` -1, then `CQuitWnd` 1, which sets
`[manager+0x20]` and `[0x0066DE4C]` and ends `CGameWnd` with -1 (`0x0040C1A2`).

The host leads the game down that same path. Alt+F4 (seen in `PeekMessageA` too, because the game's
pump drops system keys in some loops), the X button, the system menu's Close and a `WM_CLOSE` from
the taskbar or another program start it once (later requests are ignored):

1. `CloseRequested`: the cursor is released and never captured again; `[shutdown] guest threads
   before quit: N` with each worker (id, entry, creator); `WM_CLOSE` is offered to the game (which
   ignores it).
2. `GuestQuitRequested`: the window stays up and keeps presenting, but keyboard and mouse input no
   longer reach the game; activation messages do not either, and its `IsIconic`,
   `GetForegroundWindow` and `ShowWindow` see a foreground, restored window (it pauses and rebuilds
   its display only on those); F5 and F11 are refused.
3. Each time the game's modal pump `sub_00417760` calls `PeekMessageA`, one step for the window in
   front: `CGameWnd` gets the in-game Quit's flags and EndModal(-1) (`GameWndClosing`),
   `CMainScreenWnd` -1 (`MainScreenClosing`), `CInGameOptionsWnd` -1 (its Quit button, which opens
   `CQuitWnd`), `CQuitWnd` 1, any other dialog 0, Cancel (`ModalClosing`); a loader is waited for.
   The Smacker player's loop (`sub_00418AE0`) gets the key-up it skips on. If the game is inactive it
   is reactivated through its own `WM_ACTIVATEAPP` handler. The pump is recognised by the current
   lifted function: pcrecomp's calls push a dummy return address (`0xDEAD0000`), so the guest stack
   holds no real return addresses. (A first version matched return addresses and never took a
   step: the hidden game kept running.)
4. `WinMainReturning`: the game destroys its window or posts `WM_QUIT` on its way out of WinMain.
5. `CRTExiting`: the CRT called `exit`. The game's atexit handlers run there, on its thread, through
   msvcrt `_cexit` (`_c_exit` for `_exit`/`ExitProcess`, which skip them), so msvcrt has nothing left
   to run when it unloads. `GuestStopped` when they are done.
6. `HostCleanup`: `[shutdown] guest threads after quit: N` with any worker still running,
   summaries, the level swap file is removed if the game left it, the presenter is released. The
   exiting thread keeps pcrecomp's machine lock from here on, so no lifted code can run on any
   thread; `exit code 0` is the last line, then `ExitProcess`, which ends the remaining workers.

A watchdog thread follows the phases and every quit step: 250 ms without progress is logged with
the window in front, the current lifted function, the last native call and how often the quit pump
ran since the last step, then again after 1, 2, 4... s. If the game has not left its loops 60 s after
the request the machine state is logged and the run ends with exit code 8
(`HOST_EXIT_SHUTDOWN_TIMEOUT`), never 0. The game's own Exit goes through steps 4 to 6.

The game's worker threads: `sub_00435580` (created by `sub_00435390`) streams audio and is ended by
the game's own audio teardown (`TerminateThread`, `0x004354A3`); `sub_005FB070` (by `sub_005FA370`)
works through a list until done or until its abort flag (`+0x180`); `sub_004B08E0` (by
`sub_004B0000`) copies the level file into the new `.swp`.

Alt+Tab during a level load: the game's `WM_ACTIVATEAPP` handler (`0x0040EFE0`) releases the screen when deactivated
and, reactivated, recreates the display (`0x0040EB20`) and the renderer (`0x00412A00` -> `0x005061E0`), which rebuilds
the sky texture (`CSkyRender` at `[0x732A28]`, `0x005241A0`) pixel by pixel from its source raster. During a load
that raster's pixels live in the level's paged memory (`0x005FA370` reserves it, its loader thread `0x005FB070`
commits pages as it reads them), so a reactivation there read a page not yet committed (log 148, read of
`0x1BF80000` in `0x0050AEC0`). While the level load (`0x004142C0`) runs, the game is not sent `WM_ACTIVATEAPP`; when
it returns the game gets the last one if it differs from what it saw (`activation_defer_t`, `[focus]` lines).

### Cursor and the game's screen

While a virtual mode with exclusive full screen is set, the game's window is, for the game,
`(0,0)-(w,h)` of that mode at screen (0,0): `GetClientRect`, `GetWindowRect` and `ClientToScreen`
on it answer that way. The game's cursor is virtual and the real cursor is never moved:

- `SetCursorPos` sets the virtual position only. `GetCursorPos` returns it.
- Which window of the game is in front is read from the game's own window stack (`guest_ui.c`:
  the manager at `[[0x0066E010]+8]`, stack `+0x38..+0x3C`; `CGameWnd` on top and not stopped is
  gameplay, a window over a `CGameWnd` is a dialog, anything else a menu).
- Input states: `INPUT_MENU`, `INPUT_DIALOG`, `INPUT_GAMEPLAY_CAPTURED`, `INPUT_GAMEPLAY_RELEASED`,
  `INPUT_MOVING_WINDOW`, `INPUT_FULLSCREEN_TRANSITION`, `INPUT_SHUTDOWN` (`host_policy.c`),
  from that stack, focus, minimisation, the move/size loop, F11 and closing. Transitions are logged.
- Menus and dialogs: the virtual cursor follows the real one (`floor((p - picture origin) * display
  size / picture size)`, clamped to the display), so the game's own drawn cursor and its buttons work.
- Gameplay: the game re-centres its cursor every frame (`0x005F80B0`) and steers by the distance.
  `INPUT_GAMEPLAY_CAPTURED` (`CGameWnd` in front, not stopped, window focused and not minimised) is
  the only state with relative Raw Input (`WM_INPUT`; cursor deltas if Raw Input is unavailable), a
  hidden cursor and `ClipCursor` to the picture on the screen, windowed or full screen. The real
  cursor is never moved. Every other state calls `ClipCursor(NULL)`: Esc and F1 free it on
  their key-down (then `CInGameOptionsWnd`, `CHintWnd` make it `INPUT_DIALOG`), and
  so do Alt+Tab, focus loss, minimising, F11 (re-clipped to the new picture afterwards), a move or
  size loop and closing. Returning to `CGameWnd` recaptures. A capture that starts while a button
  is held outside the picture (the click on the title bar that activated the window) waits for the
  release.
- Mouse messages carry the virtual position in gameplay and the translated one elsewhere; a button
  press on a black bar is dropped.
- `ShowCursor` keeps the game's counter in the host; the system cursor is hidden only over the
  picture, and only when the game draws its own (menus) or mouse-look is engaged.
- The game's own `ClipCursor` calls are not applied (logged once); the host clips by its state.
- Non-client messages (`WM_NCHITTEST` and the non-client mouse messages) go straight to
  `DefWindowProc`, so caption, borders, minimize, maximize and close behave normally.
- With no virtual mode set (the game's own windowed mode) positions and rectangles pass through
  unchanged and the presentation surface still follows the last display's aspect ratio.

### Gamepad

`src/host/gamepad.c`, `gamepad_menu.c`, `gamepad_policy.c` (no SDL, no window: tested by `gamepad_tests`) and
`pad_glyphs.c`. SDL3 (the launcher's static SDL) is started for gamepads only (`SDL_INIT_GAMEPAD`, no SDL video and no
SDL window) on the guest thread, the first time it is needed; every SDL call stays on that thread. A failure leaves the
keyboard and mouse as they are. SDL's raw input joystick driver stays off: it reads the thread's whole raw input buffer,
the game's mouse included. Only connection events are taken from SDL's queue (the per-input events are turned off);
buttons and axes are read directly once per frame.

- Gameplay: `CInput::tinReadDefaultControls` (`0x005F80B0`, an optional override) runs as it is, then the gamepad is
  added to its `SInput` (`CInput + 8`: elapsed, rotate x/y, move x/y, state, hit). Movement is analog through the game's
  own `v2Move` (the player moves at `min(|v2Move|, 1)`): forward up to the keyboard's run (1.0), back to its 0.4,
  sidestep to its 0.6. The right stick adds radians to `v2Rotate` (rate x frame time, look curve, invert Y); while Move
  Hand is held it moves the hand, as the mouse does, at 0.6 of the speed. Buttons set the game's command bits (`state`,
  and `hit` on a new press), so the keyboard's remapped keys keep working beside them. Buttons held when gameplay resumes
  are ignored until released.
- Start posts Esc (pause menu) and the Jump button posts Space (the game's restart prompt after death) to the window as
  key messages with scan code `0xFE`, which never count as keyboard use. In the video player a press skips as a key-up
  would.
- Menus: the game's UI has no keyboard focus, so focus is the game's own pointer. In `CUIWnd::DoModal`'s loop
  (`0x00417840`), just before it reads the pointer, the front window's controls are read (buttons, checkboxes, sliders,
  list rows, the save-name box, hotspots; the Controls window's key-remapping hotspots are left out, as a click there
  would bind the mouse button). D-pad and left stick move between them, the pointer goes to the control's centre (the
  control shows its own hover picture), A is a left click there (`WM_APP + 0x47` turned into the game's mouse message),
  B and Start post Esc. Sliders are dragged a tenth of their track; lists select row by row and scroll with their own
  arrow buttons; A on the selected row is a double click (Load loads). In Save, a gamepad user's new save gets a name
  typed through the window's own `WM_CHAR` handling (the game saves only under a name). The right stick moves the
  pointer freely.
- Pointer, last input wins: while the gamepad moves the focus in a menu or dialog (`gamepad_menu_focus`), no pointer is
  shown: the system cursor is set to none over the picture (`input_pad_hides_pointer`, refreshed the frame the state
  changes, not only on `WM_SETCURSOR`), the HD layer drops its copy of the game's pointer, and the classic canvas
  draws `CUIManager`'s pointer (`0x00417410`) with its shown flag (`+0x20`) cleared for that call. The game's pointer
  still sits on the focused control, so its hover picture stays. The mouse (24 counts within 250 ms, or a button) or
  a key brings every pointer back at the real cursor; the right stick's free pointer is shown, as it is a pointer.
- Hover pictures: a plaque's hover picture (slot 1) only turns its stencil letters a deep red (about 76, 26, 11). The
  HD painter lifts them to a bright red before scaling (`FA_HOVER_LIFT`, `fa_hover_lift`): a pixel counts as
  lettering when it is redder than the normal picture there, red itself, of the letters' hue (green under 0.3-0.46 of
  red; the plates' rust is orange) and away from the rim, with a density test that drops lone specks; a 2-pixel red
  glow follows the letters. Some hover pictures draw their letters a few pixels off the normal ones, so the normal
  picture only tells where red is new. The plate, rust and pressed/disabled pictures are left as they are.
- The player's input (CPlayer::Process(const CMessageControl&) at `0x0044CB80`, CPlayer vtable `0x0064A8F4` slot 123;
  the message carries SInput at `+0x10`: rotate `+0x14/+0x18`, move `+0x1C/+0x20`, state `+0x24`, hit `+0x28`).
  Crouch is `state & 2` read every frame (`0x00450196`, `0x00455ACC`), a plain hold with no toggle, or the game's own
  reach crouch `[+0xE55]`, kept only while the pickup state `[+0xE30]` is 1 or 2, i.e. while Grab is held with the hand
  out (`[+0xE28] >= 2`). The pad's B is that hold; L3 is a host latch (`pad_crouch_*`) cleared by B or Jump presses,
  menus, focus loss and device switches. Rotation goes to exactly one consumer: the view only while the hand is down
  (`[+0xE28] == 0`, `0x00454611`); with the hand up, Rotate Wrist/Arm turn it (`0x004519C7`), else it is added to the
  hand's angle, clamped to the tuning at `0x0066ED7C/80` (65 and 75 degrees), and every frame the angle sits past
  `0x0066ED84` (35 degrees) the excess turns the body (`0x00451CA6`). The right stick is mouse motion: before CInput
  reads, its look becomes counts of 0.005 rad (`pad_mouse_counts`) moved into the same virtual cursor the raw mouse
  moves (`pointer_add_relative`, the one place both land), and CInput turns them into SInput rotation as it does the
  mouse's; the pad adds only command bits and the analog move after the read (`pad_play_before/after`). Move Hand is
  the left mouse button's key-map bit (`0x40`); each pad action adds only its own bit, as its key does (LB/RB are
  Shift/Ctrl, effective with Move Hand held). `CPlayer::Process` (`0x0044CB80`) reads nothing else: with `0x40` absent
  for one read, `+0xE28` drops to 0 (test `0x004512CF`, store `0x004544C4`), the arm relaxes and the rotation goes to the view; the next read
  with it starts the hand again from where it hangs, clamped into the reach box (`0x0066ED8C..98`). A held button
  must therefore never lapse: a pause in gameplay's reads (a frame over 0.25 s, a menu, a video, focus) keeps every
  button that counted before it and is still held, with no new press, as GetAsyncKeyState keeps the left button for
  CInput (`pad_edges_resume`); only a button pressed during the pause waits for its release, so a menu's A or B does
  not jump or crouch. Before JPT-HAND2F a frame over 0.25 s reset the edges and dropped a held LT until it was pressed
  again. `input_tests` runs the lifted CInput read with both devices and compares every frame.
- Crouch height: CPlayer's request reaches the pelvis as `bKontrol_Krouch[i]` (`0x67AE68`, handler `0x004F0960`);
  `Pelvic_IDOF` (`0x004F3FB0`) springs `Pel[i][MOUTH]` (`0x6CF294 + 0x300 i`) towards `Pel_Data[i][14] - 0.2` (times 0.1
  crouched) and clamps it to `0.09..1.1` times that, zeroing its speed; `Pelvis_integrate` then adds `dt * speed`.
  `CPhysicsSystem::Integrate` (`0x004CF8A0`) runs the physics with the x87 at 24-bit precision, where the clamped height
  stored as a float equals its bound. The lifted code computes in double whatever the control word, so the bound came
  out above the stored height, the clamp fired on every 10 ms step and the player stayed crouched at 0.072 (log 057).
  `physics_fixes.c` re-takes the two clamps at the game's precision (`pelvis_mouth_clamps`) when the control word asks
  for 24 bits.
- Water steps: `CEntityWater::CPriv::Integrate` (`0x0060FC10`) passes the time owed since the last render
  (`sRequireStep`) to `CWaveData::Step` (`0x00636FB0`), which splits it into steps no longer than the model's
  `sStepMax` (`sqrt(1.8 / (8 vmax / dx^2 + restoring))`, 0.05 s on the beach). The physics disturbs the water from its
  10 ms steps (`CreateDisturbance`, `-0.015 *` a box's vertical speed); a disturbance only moves the current heights,
  so each water step turns what landed since the previous one into speed over that step. Stepped once a frame, the
  water got 3 times the energy at 60 fps that it got at the 20 fps the model was built for, and the pattern at the
  grid's own scale, which does not travel, piled up. `water_cadence` pays the owed time in whole steps of just under
  `sStepMax` and carries the rest, so the water is the same at any frame rate.
- Controls (`controls.ddf`): when the game centres the window (`frontend_hd_window_centred`), the host moves it and
  every control into a 1.75x layout with wider label columns (`fe_controls_rect`), writing the game's own rects, so hit
  tests, gamepad focus and the remap hotspots follow; the HD painter scales its pictures and fits each label and field
  on one inset line (`FE_TEXT_FIT`), and joins the ddf's two "Replay" / "voice over" boxes. The game still draws its
  own copy into the canvas, sizing each picture's source rect from the control's rect; `RasterBlt` (`0x004128A0`,
  ui_canvas.c) clips that source rect to its raster (`raster_source_clip`). Without it, the full redraw after Alt+Tab
  read the 318x198 background through its larger rect past the end of the picture (log 055: fault in `sub_00412680`
  under the static control draw `0x00403AE0`).
- Triggers bound as buttons have hysteresis (`pad_buttons_with_trigger_hold`): down past the deadzone + 0.08, up only
  under the deadzone, so LT resting near its threshold does not lower and raise the hand every frame.
- The last device used decides the prompts: a new button press, a stick or trigger crossing its threshold (beyond the
  deadzone, with hysteresis) for the gamepad; a key, a mouse button or 24 counts of mouse motion within 250 ms for the
  keyboard and mouse. Using the mouse gives it the pointer back.
- Prompts: with the gamepad in use, the HD frontend draws the Controls window's binding fields as the gamepad's glyphs
  for the game's commands (the left stick for walking, half or full), and a Select/Back legend in the screen's lower
  right corner. Glyphs are vector shapes with analytic anti-aliasing at the layer's resolution (`pad_glyphs.c`), their
  text in the menu font.
- In-game prompts (`tutorial_prompts.c`, text rules in `src/common/jpt_prompt_text.c`): the beach tutorial's
  subtitles (in `data\Stream.tpa`, not the executable) name keys and mouse buttons, and so does BE's `TrigTutorHint`
  TEXT action ("You can always press F1 for a hint..."): its F1 becomes the Hint button with a gamepad, the keyboard
  keeps the line as it is. The text overlay's draw
  (0x0050EAF0) reads each laid-out text back, rewrites it for the device in use (each key or button an action
  resolved through the live key map at 0x0066AE78 or the gamepad bindings), and lays it out again with the game's
  own routine (0x0050E8C0) in a host buffer, each token a run of DEL characters whose advance width is set so the run
  is as wide as its glyph. `TextOutA` then draws the text pieces and paints opaque glyph pixels over the runs
  (`TransparentBlt` from a cached DIB), inside the box claimed for the text overlay. The rewrite is cached per text
  and redone when the device, glyph family, bindings or font change, so prompts switch on the next frame. View (F2 on
  the keyboard) adds a controls card to the same overlay (0x00468710, category 0x4A50) and removes it by setting its
  expiry to the game clock.
- The controls card and the one-time "View Controls" hint (category 0x4A51; the View glyph or the F2 keycap, whichever
  device was used last) are panels, not word-wrapped text: the
  game's 133 px font at 4:3 4K wrapped the card's lines across the screen and drew them over the game's own text at the
  top. `jpt_card_*` (in `jpt_prompt_text.c`) lays them out: a title, then two columns of rows (the body, then the arm),
  each a binding right-aligned against its action; text at 4.6% of the screen's height (never above the game's font),
  stepped down until the card fits the safe area, which is 4% in from the sides, two of the game's text lines clear at
  the bottom (subtitles) and below a top band. The band is one game line under the margin, or more: the overlay's
  entries (ring at `+0x54`: blob `+0x0C`, flags `+0x1C`, category `+0x24`) laid out at the top push it down, and it
  only grows while a panel is up. The panel's blob holds one single-character line per row at the row's corner;
  `TextOutA` recognises the line by its address and draws the row's pieces in a font of the game's face at the panel's
  size (aliased, so the overlay's write stays pure) with a wider drop shadow, glyphs as for subtitles. Opening the card
  ends the hint.
- Hotplug: controllers can come and go at any time; the one used last is in use, the first connected until then. When
  the one in use goes, the last used of the others takes over; with none left, the keyboard and mouse do.

### Title bar

`Jurassic Park: Trespasser Recompiled | x86 | D3D11 | FPS: 59.9 | 2560x1440`: host architecture,
current renderer, presentation rate and the selected resolution (docs/LAUNCHER.md, Resolutions): the render size with
D3D11, the gameplay mode with Software or Original. Menus and videos, which the game draws at 640x480, do not change it,
and neither does the window's size. FPS comes from one place: each `IDXGISwapChain::Present` that returns
`S_OK` (not a failure, not occluded) is timestamped with `QueryPerformanceCounter`; once a second
of them has passed the rate is computed over exactly that span and the title is updated, so with
both renderers it is the presentation rate, not the simulation tick and not the monitor's refresh.
A 250 ms timer only drops the value to 0 after two seconds without a Present and re-checks the
mouse policy; it never starts a new counting window. (Before, a 1 s `WM_TIMER` and the Present path
both reset the window; the timer usually ran in the same 15.6 ms tick right after the Present path
and published a rate measured over 0 ms, so the title showed 0.) Several front-buffer updates
inside one game frame (a `ReleaseDC` after a `Blt`, say) are each a Present and each count.

## Text translations

`src/host/l10n.c` loads the text of the chosen `language` before the guest runs. The text is compiled into the executable
(`assets/translations`, through `src/host/l10n_assets.cpp`): there is no installation and no file to find. One language
gives a game text and a subtitle language (`jpt_l10n_game_text_language`, `jpt_l10n_subtitle_language`; `docs/LAUNCHER.md`,
Language). With English nothing below is installed and the game runs as before.

- **String IDs.** Every retail string goes through the `LoadStringA` import (0x00645434): hints are 18000 + the hint
  (0x0040E820, also used by level TEXT actions that carry an ID), dialogs and key names their own IDs. The shim answers
  with the translation in the display code page when there is one and its printf conversions match the English
  string's, else the English resource. Objectives (F3) show the same string in UTF-8 for ImGui, with Arial merged into
  the panel's font for the letters it lacks. Objective titles are the port's own English.
- **Level TEXT actions** with inline English text (0x00472B60: ID at `+0x18`, text at `+0x14`): BE's F1 line is GameStrings
  19290, swapped in for the call.
- **Voice captions.** 0x004395A0 hands a playing sample's caption block (state at `+0x20`: shown flag, block at `+4`:
  length, count, then a 0.2 s time and a NUL-terminated line per caption) to the text overlay. At start a thread reads
  the game's own `Stream.tpa`, `Effects.tpa`, `Ambient.tpa` and `Menu.tpa` (directory and captioned headers only) and
  fingerprints each English block; the override swaps the block for the translated one of the same sample, with the
  translation's own times, and restores it after the call. Audio is untouched. A block without a translation, or one
  played before the scan ends, stays English.
- **Menu labels.** DDF files under `menu\` are opened with `CreateFileA`; the shim opens a copy in
  `user_data\cache\menu-text\<language>\` whose `"English" ; Initial Text` labels were replaced (GameStrings
  10000 + the DDF number, matched by their English text). The HD frontend joins "Replay" and "voice over" in their
  translated form.
- **Fonts.** `CreateFontA`/`CreateFontIndirectA` shims give the game's ANSI or default charset fonts the display code
  page's charset (1252, or 1251 when Russian is chosen), and so does the HD frontend's own font, so `TextOutA` and
  `DrawTextExA` draw the bytes as that code page. `MessageBoxA` is shown through `MessageBoxW`.
- **Word wrap.** The overlay's layout (0x0050E8C0) reads advance widths through a signed char, so every byte above
  0x7F took a width from before the font object. Text with such bytes is laid out by `src/host/text_layout.c`, the same
  algorithm with unsigned widths (0xFF measured, the table stopping at 0xFE); ASCII text keeps the lifted routine.

## Legacy hardware warning

At start-up the game asks the registry for `DDrawCertIgnore`; unset, it shows dialog 121,
"Direct DirectDraw NOT certified", a check of legacy display hardware that says nothing about
D3D11. While the compatibility layer is active, the registry shim answers that one value as set
(1) when no stored value exists, logs it as host policy and stores nothing. `--ddraw system` and
`--ddraw-cert-dialog` keep the game's own behaviour. Every other dialog and the game's error
reporting are untouched, and `GetCaps` still reports no `DDCAPS2_CERTIFIED`.

## Legacy Safe Mode prompt

"Trespasser was unstable the last time it ran. Do you wish to launch Trespasser in 'Safe Mode'?"
comes from registry `Safemode`, not from the `*.swp` files (their `FindFirstFileA` at `0x0040DA00` is
the free disk space check). WinMain's first call (`0x005F70E0`) reads `Safemode` into `[0x0073DBAC]`
and writes 1; its last (`0x005F70B0`) writes 0. A 1 at start-up, without Shift held, asks through
`0x0040D780` (string 11, `MB_YESNOCANCEL`): Yes stores conservative settings, No continues, Cancel
quits. Any run that does not reach the end of WinMain leaves the 1 behind. By default the host
answers `Safemode` as 0, never stores it, drops a stored value at start (`[host] legacy Safe Mode
dialog suppressed`), and if that prompt is still reached answers No for that call site only (the
helper `0x0040E4D0` called with type 0x23, title 1 and text 11).
`--legacy-safemode-dialog` restores the game's behaviour. Crashes are still reported in the log.

What the code shows (1.0.116, `tpassp6.exe`):

- Startup, `0x0040DB10`: `0x005053F0` creates DirectDraw, then `0x00505670` calls
  `IDirectDraw2::GetCaps` and tests `DDCAPS2_CERTIFIED`. Without it, and unless the registry
  value `DDrawCertIgnore` is set, dialog 121 (`0x0040E600`, `DialogBoxParamA`) says "Direct Draw
  NOT certified". OK continues, Cancel quits, and its check box stores `DDrawCertIgnore = 1`.
  `0x005056D0` does the same for `DDCAPS_NOHARDWARE` with `DDrawHardIgnore`.
- The menu screen, `0x0040EC61`: 640x480 at 16 bpp; a non-zero depth means full screen.
  `0x00503C10` sets the cooperative level and display mode, builds a flip chain (falling back
  to a primary plus an off-screen back buffer) and takes the screen raster's depth from the back
  buffer's `GetSurfaceDesc`.
- `0x0055EF00`, the renderer factory, accepts only a 16 bpp screen raster and otherwise returns
  null; `0x005F77C0` then constructs with that null and faults in `0x00519AF0`.
- `0x00504660` presents: `Flip(primary, back, DDFLIP_WAIT)` with a flip chain, else `BltFast` or
  `Blt` from the back buffer. The intro video (`0x004188C0`) locks the primary and writes its
  frames straight into it when `lpSurface` is set.
- The game's own default is software rendering (`Use D3D` = 0); the host now answers that value
  from `--renderer` (see "Renderers"). Direct3D goes through `IDirectDraw4`, `IDirect3D3` and
  `IDirect3DDevice3`.

## Failing loudly

- The host refuses a lift that is not the one this program was built to carry for the selected version (its embedded
  metadata: lift id, source profile and build, generator format, image layout); no file of the player's is compared.
- Any unresolved import stops the run before the entry point.
- pcrecomp answers an unknown indirect target by printing a line and continuing with
  `eax = 0`. Here a target inside `.text` that has no lifted entry stops the run and names the
  address to add to `config/extra_entries.json`.
- `tools/run_lift.py` exits non-zero if any function failed to lift, any instruction was
  unimplemented, or any constant jump target is unresolvable.
- A fault in lifted code or in `SelfMod` produces a report with the lifted function, the last
  native call, the registers and the recent indirect calls.

## Logs

One file per run, `build\logs\trespasser_NNN.log`: `build.ps1` writes the build into it and passes
it to the host with `--log-file`, and the host appends (opened with `FILE_APPEND_DATA`, stderr
unbuffered, fault reports written with `WriteFile`, so a crash loses nothing). Started on its own,
the host creates the next number itself. The host's own lines are also echoed to the console.
Tags: `host`, `target` (fingerprint), `loader`, `selfmod`, `registry`, `imports`, `stage` (first
call to a boot-stage import), `file`, `msgbox`, `dialog`, `ddraw`, `d3d11`, `window`, `exit`,
`guestdbg`, `guest`, `fault`. A fault report names the last DirectDraw call.
`input`, `hotkeys`, `renderer`, `d3d` and `perf` cover the input state machine, the keys, the
renderer and Direct3D translation, and a performance summary every 5 seconds: FPS, frame time, the
guest's share (frame time minus host work) and per-frame milliseconds of translation, draws,
texture uploads, readbacks, overlays and present, plus one readback line
(`readbacks N | MB | ms | top sub_X`, by reason, then accesses served without reading: write-only,
clears recreated, front copies) and a hybrid line per frame (Direct3D draws and triangles, CPU
accesses to the target, CPU pixels composited and how many black). The session ends with the
readback callers (count, MB, ms), CPU pixels composited in gameplay and menus with the black ones
over known GPU pixels, the draw state classes (blended ones are the
water and glass candidates), dark-draw counts, unknown texture formats and the refused-state
counters. `shutdown` and `thread` cover closing and the game's own threads.
Per-call lines are capped so loops cannot flood the file. `--native-trace` and, in a
`-Trace` build, `--calltrace FILE` / `--firsthit LO HI` give full detail when asked for.

Misses under the game folder that its `data\` subfolder answers (`game\menu\x.tga`, then
`game\data\menu\x.tga`) are the game's own path search: they are counted and summarized at exit,
not logged, and so are repeats of the previous successful open. Every other miss is logged.

### Exit codes

0 normal, 1 setup failed before the game started, 2 reached a function that is not lifted, 3 fault,
4 watchdog, 5 unresolved target in `.text`, 6 the game reported that it did not finish starting up,
7 the game exited with an error code of its own, 8 the game did not leave its loops within 60 s of
a close request (see "Closing").

The guest's CRT start-up (`0x00626BE2`) passes WinMain's return value to `exit()`. WinMain
(`0x0040DB10`) returns 1 when the game ran (it stores 1 at `0x0040E006` on the way to its cleanup)
and 0 on every early-out, so a normal quit used to surface as the host's code 1, "setup failed".
`exit`, `_exit` and `ExitProcess` are shimmed. `exit(1)` from the CRT start-up is a normal exit and
becomes 0; `exit(0)` there becomes 6; `exit` from anywhere else (about 300 call sites: the STL's
length-error handler calls `exit(1)`, for one) is an error and becomes 7, with its caller logged.
`ExitProcess` passes its code through, logged. The log line also says how the quit started: the
game's own menu, the title bar's close button or system menu, or Alt+F4. Faults, the watchdog and
unresolved targets keep their reports and codes.
