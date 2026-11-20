# Linux x86_64 native host

Status: **bring-up. The lifted guest runs on Linux x86_64, headless, through the CRT and C++ start-up, the window, DirectDraw and into
the game's own loop: against a virtual window it draws the original main menu (measured: 4 161 distinct colours at frame 400 against 1
at frame 5).** The SDL3 window backend (`--run`) opened the window and the main menu on WSLg/X11 in a first manual run; New Game then stopped at
`DrawTextExA`. After the text, file, audio and `GetDC` work below, the headless scripted New Game (click NEW GAME, 700 frames) loads
Beach and renders it, with the DirectSound layer on SDL's dummy driver; Smacker video is not decoded (no FFmpeg in the build here),
so `NoVideo` is answered 1. Every statement below is something a headless command measured, or is marked as not done.

Target: a native ELF 64-bit x86-64 program (no Wine, Proton, Winelib, PE loader or 32-bit runtime) for a guest that
stays a logical 32-bit Win32 program. Baseline: Ubuntu 20.04, glibc 2.31.

## Build and test

```bash
tools/build-linux.sh            # build/linux/bin/trespasser_recomp, the lifted guest, one log in build/logs
tools/build-linux.sh --tests    # also the headless suites
tools/test-linux.sh             # build, run the suites, check the executable, list what no suite proves
CC=gcc-13 tools/test-linux.sh --build-dir /tmp/jpt_build   # another compiler, another build folder
```

Needs cmake 3.20+, ninja or make, gcc or clang, python3 and `external/pcrecomp` (`tools/fetch_pcrecomp.py`). The scripts
never start the program, a window, a sound device or the launcher; suites run with SDL's dummy drivers. The build tree is
`build/linux` (ignored by git) unless `--build-dir` says otherwise; a Linux file system is much faster than `/mnt/c` in WSL.
`cmake/JptLinux.cmake` is included by the top-level `CMakeLists.txt` on Linux only. Debug info and `__FILE__` use
`-ffile-prefix-map`, so no machine path is left in the binary.

## Call spine (`src/linux/linux_spine.*`)

Lifted code keeps the guest's registers in the `g_*` globals (locals inside a function, written back at every call and
return), pushes a dummy return address on the guest stack and calls the C function of its target.

* **Guest -> host.** A target in the thunk range `0x7F000000-0x7FFF0000` resolves (`recomp_lookup_import`) to one shared
  trampoline that runs `ht_invoke_ex`: the arguments are read from the guest stack as 32-bit values, the host function runs,
  `eax` (`edx` for 64-bit results) is set, and `esp` is popped as the convention says: stdcall pops the arguments and the return
  address, cdecl the return address only, thiscall takes `this` from `ecx` (`HT_THISCALL`) and pops the stack arguments.
  A target that is neither lifted code nor a thunk is `LX_BAD_DISPATCH`, never a silent `eax = 0`.
* **Host -> guest.** `lx_call_guest(va, convention, argc, args, this, &edx)` builds the same frame, runs the lifted function,
  and checks that `esp` ends where the convention puts it; a stdcall function called as cdecl (or the reverse), or one that
  returns without popping, is `LX_FATAL_ABI`. The guest's `ebx`, `esi`, `edi`, `ebp`, `esp` and flags are restored afterwards,
  so a host function can call back into the guest, and guest -> host -> guest -> host -> guest nests to any depth.
* **Faults are results.** `lx_run` wraps guest code in a recovery frame: a missing import, a bad dispatch, an unlifted
  function, a stack fault or `ExitProcess` returns a status and a diagnostic (`module!function`, the lifted function that made
  the call). The bootstrap reports them as `BOOTSTRAP_MISSING_IMPORT`, `BOOTSTRAP_REACHED_USER32`, `BOOTSTRAP_FATAL_ABI`,
  `BOOTSTRAP_BAD_DISPATCH`, `BOOTSTRAP_NOT_LIFTED` or `BOOTSTRAP_EXITED`.
* **Guest threads.** Each has its own register file, x87 stack, guest stack and TIB. One guest thread runs at a time (the
  machine lock); blocking host waits (`Sleep`, `WaitFor*`) release it with `lx_blocking_begin/end`, which save and restore the
  registers. `CreateThread` itself is not implemented yet.
* No guest byte is executed: the spine, the HLE and the lifted C are ordinary compiled code.

`tests/linux_call_spine_tests.c` calls guest functions written with the lifter's own macros (`tests/linux_fake_guest.c`).

## SelfMod (`tools/lift_selfmod.py`)

SelfMod (`0x0063B000-0x00644B75`, 39,797 bytes, section flags code+writable) holds 46 routines: the rasteriser's inner loops,
the stubs the static-initializer table points at, and the `atexit` destructors. The 33 loops patch the trailing dword of 44 of
their own instructions in their prologues (`lea eax, [label]; mov [eax-4], ecx`): a texture base, a colour table base or a
lighting table base. These are stores of data into the operand field of an instruction, never a change of opcode, branch or
target, and no absolute-address store into the section exists (`tools/x64_selfmod_scan.py` measures both).

* **Method.** The same lifter as `.text`, with `patch_sites` set to the 44 fields: each patched operand is read from guest
  memory (`MEM32(site)`), the prologue's store is an ordinary guest store into the mapped image, and the loop reads it back.
  Entries are the 46 routine starts; the 11 undocumented `DC D8` (`fcomp st(0)`) encodings the decoder cannot read are
  listed in `config/decoder_fixups.json` with the others. Output: `recomp_selfmod.c` in the lift's generated folder, with its own
  table, `lift116_selfmod_dispatch_table` (46 entries). Lifted code enters it through `RECOMP_ICALL`; its one call out
  (`atexit`, `0x00626B57`) is a direct call into lifted `.text`.
* **Official process.** `tools/run_lift.py` calls `lift_selfmod.emit` for every lift it generates; running it for the
  baseline reproduced all 75 files byte for byte (`recomp_selfmod.c` included) and `tools/lifts.py pack lift116` produced the
  archive in `lifts/`. `py -3 tools/lift_selfmod.py --check` compares the committed file with the executable.
* **Windows.** The Windows host still runs the section's original bytes (`src/host/selfmod.c`); the lifted file compiles
  with MSVC x86 and is not called there. One implementation for both platforms needs a differential run of the lifted and the
  native routines on a Windows x86 build, which was not done. **The lifted rasteriser has not been compared against the
  native one.**
* **Other lifts.** Only `lift116` carries SelfMod. `lift_es117` and `lift_ptbr117` get it when regenerated with
  `tools/generate-lift.ps1` (the es-117 executable was not available here).
* **Tests.** `tests/test_selfmod_lift.py`: the range, 46 sorted entries equal to the executable's routine starts, every
  SelfMod target the lifted `.text` calls has a dispatch entry, every call out reaches lifted code, nothing is left unlifted,
  44 patch sites are guest loads, no placeholder constants remain.

## Guest DLLs (`tools/lift_module.py`, `src/linux/linux_modules.*`)

A guest DLL is lifted to C like the executable; the DLL is a development input and is never executed (no Wine, `dlopen`,
`LoadLibrary` or emulator). MSVCIRT.DLL is the first and `config/guest_modules.json` is the list; nothing about it is hard-coded in
the lifter or the runtime.

* **MSVCIRT, as supported.** `MSVCIRT.DLL` 6.0.8168.0, PE timestamp 1998-06-17 18:45:46 UTC, SHA-256
  `647f7534aaaedffa93534e4cb9b24bfcf91524828ff0364d88973be58139e255` (identical in the three installed versions), base `0x780A0000`,
  size `0x12000`, entry (DllMain) `0x780A1000`, sections `.text` (`0x71E2` bytes), `.rdata`, `.data`, `.rsrc`, `.reloc`. 445 exports,
  all named: 390 code, 55 data, 0 forwarders. 889 relocations. No TLS directory. Imports: MSVCRT 43, KERNEL32 5.
* **Lift.** Starts are the 390 code exports, DllMain, the 141 code addresses held in `.rdata`/`.data` and the 81 held as immediates in `.text` (both found through the relocation table:
  vtable entries, callbacks, the exit handlers `push offset f` passes to `_onexit`) and the closure of every direct call and jump target: 578 functions. A seed that lies
  inside another function's body is a label, not a function. Output: `recomp_msvcirt.c` (the functions, `msvcirt_dispatch_table`,
  `msvcirt_meta`) and `recomp_msvcirt.json` in the lift's folder, inside the `lift116` archive; symbols are `msvcirt_sub_<va>`, so they
  never collide with `lift116_*`. `tools/run_lift.py` runs it for every lift that lists the module; a full regeneration reproduced all
  77 files byte for byte. The manifest records the module (SHA-256, base, size, entry, text range, entry and export counts, export
  fingerprint) and `tools/lifts.py verify --content` checks it. Only `lift116` carries MSVCIRT for now.
* **Runtime.** The installed DLL (`versions/<id>/MSVCIRT.DLL`, then the game folder) is read, its SHA-256 compared with the lift's and
  mapped by the same PE loader as the executable at its preferred base, before anything else allocates in the window; a different DLL
  is refused. The lifted code has its absolute addresses baked in, so the module must sit at `0x780A0000`. The DLL is a runtime input
  like `guest.img`: no byte of it is in the repository, and what the program needs from it at run time is its data (vtables, vbtables,
  strings), which stays in guest memory where the guest reads it. The final executable therefore needs the installed game's copy.
* **Dispatch.** The spine keeps several address-sorted tables (the executable, SelfMod, each module) and tries the ones whose range
  holds the address. Calls between modules are ordinary guest calls: executable to MSVCIRT through the import table, MSVCIRT to the
  host's msvcrt and kernel32 through the same resolver as the executable.
* **Module semantics.** The handle is the guest base. `GetModuleHandleA` returns it without a reference; `LoadLibraryA` returns the same
  handle and counts one; `FreeLibrary` drops one and calls `DllMain(DLL_PROCESS_DETACH)` at zero; loading again attaches again. The
  imported module starts at one and `DllMain(DLL_PROCESS_ATTACH)` runs once, before the executable's entry point. `GetProcAddress` takes
  a name or an ordinal and returns the 32-bit guest address (a lifted entry for code, an address inside the image for data); exports
  are matched exactly, case and decoration included, and never demangled.
* **Objects.** The C++ objects the lifted constructors build live in guest memory with their vtables and virtual base tables in the
  DLL's own mapped data, and the guest reads them directly. `linux_module_tests` calls `ofstream::ofstream()` and its destructor through
  the call spine: `this` gets a vbptr into the DLL's `.rdata`, the table starts with 0, its second entry is the offset of the `ios` base,
  which holds a file buffer built by the constructor and its own vtable. No file is opened. RTTI structures are in the mapped `.rdata`
  as the DLL has them; none is implemented on the host.
* **Static initializers.** DllMain runs the DLL's initializer table through `_initterm` and the spine, builds the standard stream
  objects with the lifted constructors and registers their destructors with `_onexit`/`__dllonexit`.
* **Tests.** `tests/test_guest_module_lift.py` (config, manifest, table, targets, every code export and every code address held in data
  lifted, no TLS, no forwarders) and `tests/linux_module_tests.c` (42 checks: lookup, exports by name and ordinal, the 390/55 split,
  data exports, `GetModuleHandleA`, `GetProcAddress`, `LoadLibraryA`/`FreeLibrary` reference counts, attach once and detach, the object
  layout, and refusal of a DLL with another hash).

## Bootstrap (`src/linux/linux_boot.*`, `--bootstrap-headless`)

Maps `guest.img` (SHA-256 checked against the lift's source) and the lifted DLLs, binds the imports of both, attaches the DLL and calls
the lifted entry point `0x00626BE2` on a guest thread. The host window is the virtual backend: no window, no audio. The run stops, with a
status, when the guest presents its first frame (`BOOTSTRAP_FRAME_PRESENTED`, the default), after `frame_limit` frames, when it polls
`PeekMessageA` 200 000 times with nothing to deliver (`BOOTSTRAP_IDLE`), at the first import with no implementation, or when it exits.
Run by `tests/linux_boot_tests.c` (needs the installed game, else `DATA UNAVAILABLE`); log lines: `Linux guest: entry ...`,
`Linux HLE: resolved N/297`, `Linux USER32: registered class ...`, `Linux USER32: created window ...`, `Linux GDI: framebuffer ...`,
`Linux DDRAW: primary surface ...`, `Linux presentation: ...`.

Measured with the game installed here, in the order the guest does it: CRT start-up and the static initialisers of the executable and of
MSVCIRT; the registry (`ClipCursor`, `RegOpenKeyExA`, `RegQueryValueExA`, `RegSetValueExA`); `RegisterWindowMessageA`;
`CreateMutexA`; `LoadLibraryA("Processor.dll")` and `bGetProcessorInfo`; `GetDiskFreeSpaceA` and `FindFirstFileA`; `LoadIconA`,
`GetStockObject`, `RegisterClassA` (class `DWI - Trespasser`, window procedure `0x0040F4B0`); `LoadStringA`, `CreateWindowExA` (640x480
popup at 0,0, style `0x90080000`), `ShowWindow`, `UpdateWindow`; `DirectSoundCreate` and `CreateThread`; `DirectDrawCreate`, then
`QueryInterface` for `IDirectDraw4`, `GetCaps`, `GetDeviceIdentifier`, `EnumDisplayModes`; `qsort`, `FindResourceA`, `__RTDynamicCast`,
`GetDC`/`CreateFontA`/`GetTextExtentPointA`, `SetCooperativeLevel(FULLSCREEN | EXCLUSIVE)`, `SetDisplayMode 640x480x16`, `CreateSurface`
(primary with one flip back buffer, RGB565), a colour-fill `Blt`, and `Flip`: the first frame. 213 of 297 imports are implemented
(239 with the 26 that resolve into the lifted MSVCIRT), 112 distinct imports were called up to that frame, and two runs in one process
stop at the same place. The retail `trespass.exe` is not needed: the run uses `guest.img`.

## Shutdown

One way out (`linux_shutdown.*`, `linux_guest_quit.*`). The window's close button, Alt+F4, SIGINT and SIGTERM only *ask*: an atomic
`lx_shutdown_request` (safe in a signal handler; the first reason is kept, later requests are counted). The states are `RUNNING`,
`QUIT_REQUESTED`, `SHUTTING_DOWN`, `STOPPED`; cleanup starts once.

* **Why nothing closed.** The game ignores `WM_CLOSE` (its handler is a bare return) and consumes Alt+F4 as a plain key, so a request that
  only posted `WM_CLOSE` left it running; SDL's own SIGINT/SIGTERM handlers produced the same event. The Windows host leads the game out
  through its own quit path; the Linux host now does the same (`lx_guest_quit_step`): at the points where the game's loops read messages
  (`0x00417760`, the video player `0x00418AE0`) it ends the window in front with its `EndModal` (main menu Exit, in-game Quit with the
  "to Windows" flags, other windows Cancel, a playing video is skipped with an Escape key-up). Measured headless: the game leaves from the
  menu and from the level in under a second, no host kill.
* **Signals.** `SDL_NO_SIGNAL_HANDLERS=1` keeps SDL off SIGINT/SIGTERM; both set the request and nothing else. A second signal asks for the
  host to end the guest at once, a third ends the process (`_exit`). Without a second signal the host ends the guest after
  `LX_SHUTDOWN_GRACE_MS` (5 s) if the game has not left. The crash handler never sees them.
* **Alt+F4** is a close event of the SDL backend (the window manager's close request is the other); both converge on the same request.
* **Guest threads** wait in 50 ms slices; at the end `hle_join_guest_threads` marks the run over, opens the gates of suspended threads and
  waits (3 s) until every guest thread has returned, then guest memory is freed. If one does not end, guest memory stays mapped.
* **Order.** guest stops, machine released, guest threads joined, DirectSound (SDL audio stopped first, then the buffers), DirectDraw, GDI,
  user32, the SDL window, modules, paths, guest memory, `STOPPED`. The backend's `destroy` quits SDL video once.
* **Exit code** 0 when the user asked to quit (SIGTERM included), whatever the game returned on its way out; else the guest's.
* **The display resets seen after Ctrl+C** were focus changes: leaving the window for the terminal sends `WM_ACTIVATEAPP(0)`, coming back
  `(1)`, and the game restores and sets its display mode again each time (`linux_shutdown_tests` reproduces it without any signal).
* The MSVCIRT lift now seeds the code addresses its code takes as immediates (`push offset f`), which include the `_onexit` handlers the
  game runs on a normal exit: 578 functions instead of 514 (the first normal exit stopped at `0x780A1ADC`).

## Crash report and core files

`linux_crash.*` installs handlers for SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT (`sigaction`, alternate stack, handler reset so the
default action, the core dump, still runs). Before that it writes, with `write(2)` from static buffers, to stderr and the log file:
the signal, `si_code`, fault address, host `rip` and its offset from the load base, the host thread id, the guest thread (id and entry;
`0` is the main one), the guest function (`g_cur_func`), `esp` and `ebp`, the last import thunk and its name, the last present stage
(`PRESENT_BEGIN`, `TEXTURE_LOCK`, `TEXTURE_UPLOAD`, `RENDER_CLEAR`, `RENDER_TEXTURE`, `RENDER_PRESENT`, `EVENT_PUMP`, `FRAME_HOOK`,
`PRESENT_END`), the last audio stage (`AUDIO_DEVICE_OPEN`, `GUEST_AUDIO_THREAD`, `BUFFER_LOCK`, `BUFFER_UNLOCK`, `DSOUND_METHOD`) and the
words on the stack that point into the executable's code. Stages are single atomic stores, never log lines.

* `tools/linux_symbolize.sh crash.log` turns those addresses into functions and lines (`addr2line`; the default build keeps debug info).
* A core file: WSL writes it under `%LOCALAPPDATA%\Temp\wsl-crashes\` (`core_pattern` is `|/wsl-capture-crash`), a native system by its own
  `core_pattern`. `gdb build/linux/bin/trespasser_recomp core -ex "bt" -ex "p $_siginfo"` reads it without running anything.
* The unwinder in libgcc is used only when the faulting `rip` is inside the executable: it faults on a wild `rip`, which is the case
  that matters most.
* `cmake -DJPT_LINUX_SANITIZE=ON` builds the host and the suites with ASan and UBSan (the lifted guest stays plain). The suites pass
  under it except `linux_guest_ptr_tests`, whose "nothing mapped below 2 GB" check meets ASan's shadow memory.

## Window, GDI, DirectDraw and the presenter

* **user32** (`src/linux/linux_user32.*`): windows are guest objects behind 32-bit tokens, `(generation << 8) | slot`, so a stale or
  invented token fails and a host pointer can never reach the guest. Classes, window procedures (called with `lx_call_guest`), the
  message queue (posted messages, then input, then `WM_QUIT`, `WM_PAINT`, timers), `SetTimer`, capture, focus and activation,
  `ClipCursor`, `ShowCursor`, cursor position, key state, `GetSystemMetrics`, rectangle helpers, `LoadStringA` (the executable's own
  resource directory, `linux_resource.*`), `wsprintfA`, `MessageBoxA` (logged; answers the default button, since nothing is shown).
  Creation and showing send the messages Windows sends (`WM_GETMINMAXINFO`, `WM_NCCREATE`, `WM_NCCALCSIZE`, `WM_CREATE`,
  `WM_SHOWWINDOW`, `WM_WINDOWPOSCHANGING`, `WM_ACTIVATEAPP`, `WM_NCACTIVATE`, `WM_ACTIVATE`, `WM_SETFOCUS`, `WM_WINDOWPOSCHANGED`).
  `DrawTextExA` and `DrawEdge` call into gdi32 (below). Dialogs and menus are not implemented.
* **Logical and real windows are separate.** The guest's window is a table entry; the real window is a backend (`linux_window.h`) opened
  when the main window is first shown. The **virtual backend** (`linux_window.c`) has no window: it counts presents, keeps the last
  frame's geometry and checksum, and returns events a test queues. The **SDL3 backend** (`linux_sdl_window.c`, only installed by
  `--run`) is one window, one renderer and one streaming texture; keys map to Win32 virtual keys, F11 toggles fullscreen in the backend,
  the mouse is reported in client pixels.
* **gdi32** (`src/linux/linux_gdi32.*`): device contexts, bitmaps, brushes, pens, fonts, regions and stock objects are tokens with a
  generation. DIB sections (8, 16, 24, 32 bpp, bottom-up or top-down, `BI_BITFIELDS` 565) live in guest memory. A window's context draws
  into that window's client surface (RGB565, the display depth reported by `GetDeviceCaps`); `BitBlt`, `StretchBlt`, `FillRect` and
  `EndPaint` present it. Text is measured and drawn by one rasteriser (`linux_text.*`: stb_truetype from `external/imgui` over the embedded
  Droid Sans, whole-pixel advances, grayscale coverage blended in the surface's own format): `TextOutA` and `DrawTextExA` share it
  (`DT_LEFT/CENTER/RIGHT`, `DT_VCENTER/BOTTOM` with `DT_SINGLELINE`, `DT_WORDBREAK`, `DT_CALCRECT`, `DT_NOCLIP`, `DT_NOPREFIX`,
  `DT_EXPANDTABS`, `DT_END_ELLIPSIS`, `DRAWTEXTPARAMS` margins), `DrawEdge` draws the raised/sunken button edges, and
  `Ellipse`/`LineTo` only move the pen. Strings are code page 1252 (1251 is selectable); no underline for `&` prefixes.
* **DirectDraw** (`src/linux/linux_ddraw.*`): `DirectDrawCreate`, `DirectDrawEnumerateA`, `IDirectDraw`/`2`/`4`, `IDirectDrawSurface`
  `1`..`4`, clipper and palette, as guest COM objects: 16-byte faces in guest memory, vtables of thunks in the standard method order,
  one reference count per object. The semantics follow `src/host/ddraw` (the virtual 16 bpp RGB565 display, the same mode list and caps,
  flip chains with pixel exchange, Lock with a real guest address, colour fill, source colour keys, stretch, surface loss when the mode
  changes). No Direct3D: `QueryInterface` for it is refused and the game keeps its software renderer. Pixels live in guest memory, so the
  guest's own rasteriser (SelfMod) writes straight into the surface the presenter shows.
* **Presenter** (`linux_screen.*`, `linux_window.*`): frames go to the backend unconverted (RGB565 stays RGB565; other formats become
  XRGB8888). `jpt_presentation` decides the view: letterboxed, a new guest screen size takes effect only with the first frame of that
  size (`tests/linux_present_tests.c`: 640x480, 800x600, 1280x720, 2560x1440), and the mouse maps back through the same view. Window
  size policy (Auto, fixed, fullscreen) and the F3/F5/F6/F7/F8/F10 hotkeys of the Windows host are not ported; F11 is. ImGui is not
  involved: nothing on Linux draws an overlay yet.
* **Processor.dll** is answered in the host (`linux_processor.*`): the DLL's probe uses 16-bit EFLAGS code the lifter cannot take, and
  the game reads only three things of its 77-dword record (feature word with the Pentium Pro bit, MHz, the name), so `bGetProcessorInfo`
  fills them from the host's CPUID. Disk space is reported capped at 2 GB because the game multiplies it in 32 bits.
* **Threads** (`linux_hle.c`): `CreateThread`/`ResumeThread` run guest threads on host threads that hold the machine lock only between
  blocking calls; `TerminateThread` is cooperative (the thread ends at its next blocking call); `SuspendThread` only counts. A run's
  threads are retired when it ends. The audio thread the game starts without a sound device runs but finds nothing to do.
* **Audio** (`linux_dsound.*`): `DirectSoundCreate` installs the portable DirectSound compatibility layer (`src/host/audio/dsound_compat`)
  over the SDL3 backend on first use; its guest objects are guest memory and every method is a thunk that runs `dsound_compat_call`.
  SDL picks PipeWire, PulseAudio or ALSA (`SDL_AUDIODRIVER` overrides it). The headless bootstrap (virtual window) sets
  `SDL_AUDIODRIVER=dummy`, so no test plays a sound. `dsound_compat_install` after `dsound_compat_shutdown` starts a new session.
* **Smacker** (`linux_smack.*`): the 14 `smackw32.dll` imports are bound to the portable HLE (`smacker_compat`), with file sources over
  the game's virtual paths and over a guest file handle. The HLE needs a decoder: FFmpeg's (`libavformat`, `libavcodec`, `libavutil`)
  when the build finds it. Without it nothing is installed and the registry answers `NoVideo = 1`, so the game skips its videos.
  This machine's WSL has no FFmpeg development files (`libavformat-dev libavcodec-dev libavutil-dev` are the packages); no video was decoded.
* **lz32** (`linux_lz32.*`): `LZOpenFileA`, `LZRead`, `LZSeek`, `LZClose` over a whole-file buffer; a file with the SZDD header is expanded
  when it is opened, any other is read as it is. The game reads its scenes and worlds this way.
* **Guest threads and the machine lock**: the lock is not fair, so a guest thread that only polls would keep the others from ever
  running. The import path hands the machine over (100 us, every 64th import) while another guest thread waits for it.

## Next blocker

After the first manual run (log `trespasser_linux_002.log`): `DrawTextExA` (done: `0x00405EC0` is the text method of a UI element,
flags `0x400` then `0x0`/`0x25`/the element's own, `SetBkMode(1)`, text and shadow colours), then `lz32!LZOpenFileA`, `GetFileTime`,
`_findfirst`, `printf`, and `IDirectDrawSurface::GetDC` (the game retries it forever on an error: that was a hang, not a stop).
The older text below describes the state before those.


The headless run does not stop on a missing import any more: it runs the game's loop. What the first manual `--run` will show cannot be
known from here. Known gaps on the path to gameplay: Smacker (14 imports, the intro and the menu videos), the remaining msvcrt and
kernel32 imports (`fopen`, `sscanf`, `printf`, `ctime`, `_findfirst`, `_except_handler3`, the file-time functions, `WinExec`), lz32 (4),
winmm (`joyGetPosEx`), dialogs, the audio layer, and the hotkeys and window policy of the Windows host.

## Pointer assumptions

The code the Linux executable is built from (`src/linux`, `src/host64`, the portable `src/common` files) has no cast of a
guest address to a host pointer or the reverse (`tests/test_linux_native_rules.py`), and the guest window sits above 4 GB with
nothing of the process mapped in `0x1000-0x7FFFFFFF` (`tests/linux_guest_ptr_tests.c`), so such a mistake faults. The
bootstrap reaches none of `src/host`: its 188 `uintptr_t` and 78 `GUEST_PTR` sites in 39 files are Windows-host code that is not
compiled for Linux, and none was changed. They matter when gameplay code (`patch117*`, `engine_limits`, `cpu_polygons`, ...) is
shared.

## What exists

| Part | Where | State |
| --- | --- | --- |
| Guest address space (4 GB window, commit/decommit/protect, guest heap, PE mapping, import binding) | `src/host64/` | tested on Linux and Windows x64 (`x64_tests`, 346 checks) |
| Call spine, guest threads, recovery frames | `src/linux/linux_spine.*` | tested (`linux_call_spine_tests`, 22 checks) |
| Guest DLL lift and loader (MSVCIRT) | `tools/lift_module.py`, `src/linux/linux_modules.*` | 578 functions lifted, tested (`linux_module_tests`, 42 checks) |
| Lifted `lift116` with SelfMod, Linux bindings for the 70 stable entries and 27 hooks (originals run; the Patch 1.1 and Community Edition hooks are not installed) | `src/linux/linux_lift116.c` | links into the executable |
| Guest path resolution, registry store, synchronisation, file interface | `src/common/jpt_vpath.*`, `jpt_reg_store.*`, `jpt_sync.*`, `jpt_fs*.c` | tested |
| kernel32 HLE: 46 of 59 imports | `src/linux/linux_hle.c` | tested (`linux_hle_tests`) |
| msvcrt HLE (start-up, `_EH_prolog`, exit, heap, x87 helpers, descriptors and `FILE`, `sprintf`, strings, numbers) and the data exports `_acmdln`, `_adjust_fdiv`, `_iob`, `__pioinfo`, `__badioinfo` | `src/linux/linux_msvcrt*.c` | reached by the bootstrap; no unit test of its own yet |
| Folders and logging | `src/linux/linux_paths.*`, `linux_log.*` | tested |
| user32, gdi32, DirectDraw, presentation policy | `src/linux/linux_user32.*`, `linux_gdi32.*`, `linux_ddraw.*`, `linux_screen.*` | tested (`linux_user32_tests` 68, `linux_gdi_tests` 35, `linux_ddraw_tests` 58, `linux_present_tests` 23 checks) |
| Window backends (virtual, SDL3) | `src/linux/linux_window.*`, `linux_sdl_window.c` | virtual tested; SDL3 built, never run |
| Processor.dll, dsound (no device), PE resources | `src/linux/linux_processor.*`, `linux_dsound.*`, `linux_resource.*` | exercised by `linux_boot_tests` |
| Bootstrap | `src/linux/linux_boot.*` | runs the guest headless to its first frame, and to the main menu (`linux_boot_tests`, 14 checks) |

Not done: Smacker decoding on this machine (the binding exists, FFmpeg is missing), winmm (1); user32 dialogs and menus,
`WinExec`; msvcrt `fopen` and the rest of stdio for the executable, `sscanf`,
`printf` and `fprintf`, `ctime`, `_findfirst`, SEH helpers; audio.

## Measured facts

* **Imports** (`tools/linux_inventory.py`, `linux_boot_tests`): the executable imports 297 functions. 213 are implemented by the host and
  the 26 msvcirt imports resolve to the lifted DLL's guest code, so 239 of 297 are answered; the DLL's own 48 imports bind through the
  same resolver.
* **First frame and main menu**: the run presents its first frame after 112 distinct imports; 300 frames in, the virtual window shows the
  original main menu (NEW GAME, LOAD, OPTIONS, EXIT) drawn by the guest's own software renderer. `JPT_LINUX_FRAMES=n` and
  `JPT_LINUX_DUMP_PPM=file` (environment of `linux_boot_tests`) run n frames and write the last one out.
* **Lifted guest link**: after the runtime and bindings only 12 C library symbols are left (`tools/linux_lifted_link_report.py`);
  the executable links.
* **Suites on Linux** (`tests/linux_suites.json`): 12 of the Windows gate's 36 test programs compile and pass on Linux, and
  3 new portable and 9 Linux-specific suites run there; the other 24 need a Windows header, DLL or device.
* **Binary**: position-independent ELF64 x86-64; `NEEDED` is `libc.so.6` and `libm.so.6` only; no RPATH or RUNPATH; no machine
  path; no Wine, Proton or Windows DLL reference (SDL3 holds two strings that name Wine to avoid its Steam gamepad path).
* **Window system libraries**: none is `NEEDED` (the Ubuntu 20.04 build adds only `libpthread` and `libdl`). SDL3 opens `libX11`,
  `libwayland-client`, `libxkbcommon`, `libdecor`, `libEGL`/`libGL` with `dlopen` when `--run` creates the window, so a system without
  them can run everything except `--run`; which backends exist depends on the development packages of the build machine (both builds
  here contain X11 and Wayland).
* **glibc**: see "Baseline build" below.

## Baseline build (Ubuntu 20.04, glibc 2.31)

A build on Ubuntu 24.04 (glibc 2.39) requires `GLIBC_2.38` (`strlcpy`, `fmod`, the C23 `strto*` and `scanf` redirects, all from
SDL3's own detection and from glibc 2.38 headers) and `GLIBC_2.34` (`dlopen`, `sem_*`, `__libc_start_main`):
`tools/linux_elf_check.py` fails there, and the failure is not masked.

The same sources built in an existing Ubuntu 20.04 WSL environment (glibc 2.31.0ubuntu9.9, gcc 13.1, cmake 3.31) with
`CC=gcc-13 tools/test-linux.sh --build-dir /tmp/jpt_build`: 0 warnings, 25 of 25 checks pass, and the executable requires at
most `GLIBC_2.30`. That is the baseline evidence; a release still has to be built on that system or an equivalent toolchain.
The build found a real difference from the 24.04 kernel's `mmap`: the guest window was only page aligned, so the 64 KB
granularity VirtualAlloc promises did not hold; `host_os_posix.c` now aligns it.

## Running

`trespasser_recomp --bootstrap-headless` runs the headless bootstrap against the virtual window and prints one of the `BOOTSTRAP_*`
results; `--dry-run` maps the image and reports the HLE coverage; `--run` runs the guest in an SDL3 window until the guest exits (SDL
picks X11 or Wayland; `SDL_VIDEODRIVER` overrides it). `--run` is built and has not been started: it is the first thing to try by hand,
and its log (`Linux presentation: SDL3 window ...`) says what the window system did.

```
trespasser_recomp [--game DIR] [--version ID] [--user_data_root DIR] [--log-dir DIR] [--log-file FILE]
                  [--dry-run | --bootstrap-headless | --run]
```

Defaults: `game/`, `user_data/` and `logs/` next to the program (`TRESPASSER_GAME_DIR`, `TRESPASSER_USER_DATA_ROOT`,
`TRESPASSER_LOG_DIR` override them, arguments override those). The guest image is `<game>/versions/<id>/guest.img`.
