# Architecture

## What runs where

`build\trespasser.exe` is a 32-bit Windows process. It runs on Windows 11 x64 through WoW64,
like any 32-bit program. It is not an emulator and it does not start the original executable:
`game\trespass.exe` is read as a data file, mapped, and never executed as a program.

| Part of the original | How it runs here | Kind |
|---|---|---|
| `.text`: 17,767 entry points (11,737 function bodies, 6,030 alias entries into them): all game and engine code, CRT startup, `WinMain` | C generated from the x86 by pcrecomp's `lift32`, compiled by MSVC into the host | **statically recompiled** |
| `SelfMod`: 39,797 bytes of rasteriser inner loops | the original bytes, at their original address, executed by the CPU | **original machine code, native** |
| `.rdata`, `.data`, `.rsrc` | mapped at their original addresses, used in place | original data |
| `MSVCRT.dll`, `USER32`, `KERNEL32`, `GDI32`, `ADVAPI32`, `LZ32`, `WINMM`, `COMCTL32` | the real 32-bit system DLLs | **Windows** |
| `DDRAW` (DirectDraw) | a virtual 16 bpp display in the host, presented with D3D11 (`src/host/ddraw/`); the system `ddraw.dll` is loaded but its exports are routed away | **host implementation** |
| `MSVCIRT.DLL`, `SMACKW32.DLL`, `Processor.dll` | the original DLLs from the disc, loaded by the Windows loader | **original binaries** |
| `DSound.dll`, `A3d.dll` | whatever `LoadLibrary` finds on the machine (A3D normally absent) | Windows, optional |
| Registry key `HKLM\Software\DreamWorks Interactive\Trespasser` | a text file, `user\recomp-registry.txt`; `Installed Directory` (saves, level swap files) is `user\`, `Data Drive` is `game\` | **host implementation** |
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
host's `recomp_lookup_manual`. There `src/host/ddraw/dd_dispatch.c` routes the addresses of
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
  reused rings. Blend, depth, rasterizer and sampler states are cached; shaders are compiled once.
- Textures are converted to B8G8R8A8 on upload, only the changed rectangle and only when the
  game wrote to them (Lock, Blt, `Load`). Texture memory is allocated write-watched
  (`MEM_WRITE_WATCH`), so rows changed without any of those are found before each use, uploaded and
  logged (`changed without Lock/Unlock`). `--texture-filter` (default anisotropic, 16x) applies where
  the game asks for linear filtering; the game's textures have one level and stay so unless
  `--generate-mips` is given. Each distinct sampler is logged once with the D3D11 filter and
  `MaxAnisotropy` actually bound.
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
- `guest_overrides.c` replaces two hot lifted functions through their incremental-link thunks with
  host kernels (`water_kernels.c`): the water texture rows (`0x0060E3F0`) and the CSimWaveVar2D step
  (`0x00638670`). `tests/water_tests.c` runs them against the lifted C copied out of `src/recomp/gen`;
  `--lifted-water` keeps the lifted functions.
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
- If the D3D11 device, feature level 10.0 or `d3dcompiler_47.dll` is missing, the host logs it and
  falls back to the software renderer before the game starts.
- F5 switches between Software and D3D11 while the game runs. The game re-reads `Use D3D` from the
  registry each time it builds its display (`sub_005F6D70`), and its own `WM_ACTIVATEAPP` handler
  (`sub_0040EFE0`) releases the display when the window is deactivated and rebuilds it on
  reactivation (`sub_00412A00`). The host posts itself a message, and at that message boundary
  between frames it calls the game's handler with deactivate, switches the renderer, and calls it
  with activate: the game rebuilds surfaces and device with the other renderer while scene,
  player, physics, AI and sound stay as they are. If the game kept its display (not every screen
  releases it) the switch is logged as not possible there and nothing changes.
  Going to software, the game's display build (`0x00503D8D`) releases its IDirectDraw4 (`[0x006D0E4C]`)
  and shuts its Direct3D driver object (`0x006D0E98`) down; only its DirectDraw start-up
  (`sub_00505480`, reached in game through a driver change, `sub_00414200`) sets them up again. So
  before reactivating into D3D11 the host queries IDirectDraw4 from the game's IDirectDraw2
  (`[0x006D0E48]`) and runs the game's driver set-up `sub_00505AF0`; the log says whether the game
  then created its Direct3D device again. (Without this, F5 back to D3D11 rebuilt a software
  display: no `CreateDevice`, a flip chain without `DDSCAPS_3DDEVICE`, SelfMod rasterising.)
- A switch is a state machine (`RENDERER_SWITCH_REQUESTED`, `_DRAINING`, `_REBUILDING`,
  `_ACTIVE_NEW`, `_SWITCH_FAILED`, `_SWITCH_DEFERRED`). It runs only at a boundary of the game's pump
  with no surface locked, no DC out and no scene open (`dd_census`, any thread); otherwise it retries at
  the next boundary, up to 30. The census of DirectDraw objects, surfaces, devices, viewports and GPU
  textures is logged before and after.
- Direct3D to software is not done in place while a level is in memory (`CGameWnd` in the window
  stack). In Direct3D the screen renderer (`CScreenRenderDWI`, slot 10 `0x0055F540`) sends every
  polygon with a render type (`+0x74`) to the auxiliary Direct3D renderer at `0x00738290`
  (`sub_00555BB0` -> `sub_00556CE0`), which uses the game's IDirect3DDevice3 at `[0x006D0E9C]`.
  Polygon set-up (`sub_0053A680`) only types untyped polygons, so typed ones keep their type, and
  the software fallback (`sub_00505D70`) releases and clears that device: the first software frame
  after an in-place switch read a null device in `sub_00556CE0`. The game itself changes renderer
  only when its level loader restarts DirectDraw (`sub_004142C0` -> `sub_00414200`, requested by
  `[0x0066E341]`, set by `CGameWnd`'s constructor and cleared by its stop). So F5 there marks the
  change pending, keeps that restart requested, and the host applies "Use D3D" = 0 when the game
  next calls `DirectDrawCreate` (New Game, Load, a level change). F5 again cancels it. In menus and
  from Software to D3D11 the switch is immediate.
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

`F1 Hint | F5 Renderer | F11 Fullscreen | Ctrl+F11 Cheat | F12 Controls | Esc Menu | Alt+F4 Exit`
(logged once as `[hotkeys]`). F5, F11 without Ctrl and Alt+F4 belong to the host and never reach the
game; Ctrl+F11 (the game's cheat box) and everything else go to the game unchanged. Esc, F1 and F12
open a modal window of the game on key-down (`CGameWnd::OnKey`, `0x0040BE70`), but `CUIWnd::OnKey`
(`0x0040AF50`) cancels a window on Esc key-up. The key-up of the press that opened a window (the
game's window stack grew between the key-down and it) is kept from the game, once per press; the
next key-down decides again, so a second Esc closes the menu normally, and a key-up the game
already flushed while opening (`sub_004131D0`) cannot make a later one disappear. Auto-repeated
key-downs are dropped so a held key cannot open, close and reopen it. Key lines are capped.

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
  cursor is never moved. Every other state calls `ClipCursor(NULL)`: Esc, F1 and F12 free it on
  their key-down (then `CInGameOptionsWnd`, `CHintWnd`, `CControlsWnd` make it `INPUT_DIALOG`), and
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

- The host hashes `game\trespass.exe` and refuses to run anything but the binary the C was
  lifted from.
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
