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
| Registry key `HKLM\Software\DreamWorks Interactive\Trespasser` | a text file, `game\recomp-registry.txt` | **host implementation** |
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

- `SetDisplayMode` sets the virtual mode (16 bpp, 320x240 up to the desktop's size); the
  desktop's mode is never touched. Exclusive full screen makes the game window a borderless
  window at the primary monitor's origin, sized to the mode, which keeps the game's screen
  coordinates (`GetCursorPos`, `ClipCursor(0, 0, w, h)`) right without translating input.
- Surfaces are DIB sections in the requested format, so `Lock` and `GetDC` share the pixels. A
  primary with back buffers is a flip chain; `Flip` exchanges buffers.
- Every change to the front buffer (`Flip`, `Blt`/`BltFast` to it, `Unlock`, `ReleaseDC`) is
  presented by `d3d11_present.c`: a windowed DXGI swap chain (flip model where available), the
  frame expanded once from the surface's masks, through the gamma ramp, into a staging texture.
- Methods the game never calls return `DDERR_UNSUPPORTED` or the documented error and log why.
  Direct3D interfaces are refused with `E_NOINTERFACE`, which keeps the software renderer.

`--ddraw system` restores the system `ddraw.dll`; `src/host/ddraw_trace.c` then observes its
calls. The game itself drops DirectDraw failures (`0x005053A0` reports to `0x00471710`, an empty
function), so the `ddraw` log lines are the only place they show.

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
- Software rendering is the default (`Use D3D` = 0). Direct3D goes through `IDirectDraw4`,
  `IDirect3D3` and `IDirect3DDevice3`.

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
call to a boot-stage import), `file`, `msgbox`, `dialog`, `ddraw`, `d3d11`, `guestdbg`, `guest`,
`fault`. A fault report names the last DirectDraw call.
Per-call lines are capped so loops cannot flood the file. `--native-trace` and, in a
`-Trace` build, `--calltrace FILE` / `--firsthit LO HI` give full detail when asked for.

Exit codes: 1 setup failure, 2 reached a function that is not lifted, 3 fault, 4 watchdog,
5 unresolved target in `.text`; otherwise the game's own exit code.
