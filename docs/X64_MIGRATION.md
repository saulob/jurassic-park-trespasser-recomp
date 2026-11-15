# Native Windows x64 host: migration plan and groundwork

Status: groundwork only. Nothing here runs the game. The x86 build (`trespasser`, native32) is
untouched: no existing file was changed, and the new code builds only for x64.

Every statement is marked **[verified]** (done on this machine, with the command or test named)
or **[inferred]** (read from code or disassembly, not executed).

## 1. Target architecture

```
64-bit host process (MSVC x64)
 ├─ guest window: one 4 GB VirtualAlloc reservation; guest VA = offset into it
 │    trespass.exe at 0x00400000, guest DLL images, guest heap, guest stacks, guest TIB
 ├─ lifted x86 code (src/recomp/gen) compiled as C for x64, based memory:
 │    ADDR(va) = g_mem_base + va   (or RECOMP_MEM_BASE, a constant)
 ├─ SelfMod lifted too (patched disp32 fields read from guest memory), no native x86
 ├─ imports and COM methods: 32-bit thunk VAs (0x7F000000-0x7FFF0000) -> host shims
 │    shims read stdcall args from the guest stack, marshal, call x64 Win32 / D3D11 / audio
 ├─ host -> guest callbacks (WndProc, DlgProc, thread start, qsort, enum callbacks):
 │    explicit calls into the dispatch table on a guest stack
 └─ renderers: software (lifted rasteriser + SelfMod) and D3D11 (existing src/host/ddraw)
```

Invariants: guest pointers are 32 bits; a host pointer is never written to guest memory; no
guest byte is ever executed; no 32-bit DLL is loaded into the process.

## 2. pcrecomp evaluation

### 2.1 Can the lifted C compile for x64? Yes **[verified]**

* All 73 generated files (`recomp_0000.c` .. `recomp_0071.c`, `recomp_dispatch.c`) compile with
  MSVC x64 (VS 18 Community, `cl` 19.51.36257) with `RECOMP_LOCAL_REGS` and **without** `RECOMP_FLAT_MEMORY`:
  0 errors at `/Od` (5 s with `/MP8`) and at `/O2` (92 s with `/MP8`). `dumpbin` reports
  machine 8664.
* Linking them with an empty `main` leaves exactly 21 unresolved symbols, all runtime state the
  host must define: `g_eax g_ebx g_ecx g_edx g_esi g_edi g_ebp g_esp g_st g_fp_top g_fpu_cw
  g_fs_base g_mem_base g_cur_func g_icall_count g_icall_from g_icall_trace g_icall_trace_idx
  recomp_lookup recomp_lookup_import recomp_lookup_manual`. Nothing native32-specific.
* The lifted code contains no inline asm, no `uintptr_t` casts and no `sizeof(void*)`; memory
  goes through `MEM8/16/32/64/F/D` -> `ADDR()`. 7 uses of `FS_BASE` (SEH chain via fs:[0]).
  Imports are called as `RECOMP_ICALL(MEM32(iat_slot))` (2,004 sites), so binding is purely a
  matter of what the IAT slots hold.

So the same generated sources serve both builds; only defines and the runtime differ.

### 2.2 `runtime/win32hle` as-is on Windows x64: no **[verified]**

Compiling the win32hle sources with MSVC x64 fails at the first include: `guest_mem.c`,
`host_lite.c`, `dsound.c` (pthread.h), `pe_loader.c`, `kernel32.c` (unistd.h), `module.c`,
`user32.c`, `advapi32.c` (strings.h). It is a POSIX + SDL2 layer (mmap, pthreads, `__thread`,
`__sync_*`, file descriptors). Only `ddraw.c` compiled standalone. `native32/x87crt.c` fails
on x64 (one `__asm fnstcw`); `native32/eh32.c` compiles for x64 (not linked or run).

What win32hle gets right and this plan adopts **[inferred from its source]**:

* the shim ABI (`A32`/`APTR`/`RET`, stdcall pops) and synthetic import VAs in 0x7F000000..
  0x80000000 resolved by `recomp_is_import` / `recomp_lookup_import`;
* based memory (`hle_ptr`/`hle_va`, fatal on a host pointer handed to the guest), guest heap,
  per-thread scratch, `hle_guest_format` for guest varargs;
* host COM objects whose vtables are guest memory full of shim VAs (`hle_com_vtable`);
* explicit native -> guest calls (`hle_call_guest`) instead of native32's exec-fault trampoline.

Decision: on Windows the host does not need win32hle's reimplemented window manager, files or
registry, because the real x64 APIs are present. `src/host64` therefore implements the same
model natively on Win32 (VirtualAlloc, SRW locks) and will forward most imports to the x64 API
after marshalling. win32hle code is reused where it is portable C (heap size classes and
reservation logic, adapted with credit; `pe_format.h` included directly). Porting win32hle's
POSIX layer to Win32 upstream in pcrecomp remains possible and would bring its ddraw/dsound
software paths for free, but is not on the critical path.

### 2.3 What changes relative to native32

| Area | native32 (today) | x64 host |
|---|---|---|
| Import binding | real 32-bit DLLs via LoadLibrary; `native_bridge` copies args and measures the purge (x86 asm) | IAT slot = thunk VA; one shim per import with argc + convention; `recomp_lookup_import(va)` returns a shared trampoline that calls `ht_invoke` for that VA |
| Shim calling convention | real stdcall/cdecl/thiscall on the host stack | args read from guest stack; pointers converted with range checks; out-structs copied back; varargs formatted from guest memory |
| Callbacks | guest code pages non-executable; VEH redirects to lifted code | host x64 procs (WNDPROC, DLGPROC, thread start, enum callbacks, qsort comparator) call the lifted function through the dispatch table on a guest stack; window messages with pointer lParams marshalled into guest scratch |
| COM objects given to the guest | host C structs with host vtables (`src/host/ddraw`: `STDMETHODCALLTYPE` methods, host `this`) | guest block `[vtable VA][handle]`, vtable of thunk VAs; handle table to the host object (`guest_thunks.c`, tested) |
| Memory the guest dereferences | anything | must live in the guest window: surface pixels (Lock), DSound buffer locks, DIB section bits, CRT `FILE`, `errno`, `_acmdln`, `__p__fmode` results |
| x87 | lifted FPU is `g_st` (double); SelfMod and native DLLs use the real FPU | lifted only; `_CI*`/`_ftol` from x87crt logic (pure C except `_controlfp`, which must compute the control word in C) |
| SEH / C++ EH | eh32.c over the guest TIB; `_except_handler3`, `_XcptFilter` real msvcrt | eh32.c (compiles for x64) plus HLE of the msvcrt SEH entry points; guest TIB in guest memory (`g_fs_base`) |
| Handles | real 32-bit handles | kernel, USER and GDI handles are 32-bit significant on 64-bit Windows: store zero-extended, convert back sign-extended (`(HWND)(intptr_t)(int32_t)h`, exercised in `x64_tests`) |
| Threads | real threads, machine lock | same machine lock; each guest thread gets a guest stack and TIB; `SuspendThread`/`ResumeThread`/`TerminateThread` must be emulated against the lock |

## 3. Guest memory model (src/host64/guest_memory.c) **[verified by x64_tests]**

| Guest VA | Use |
|---|---|
| 0x00000000-0x0000FFFF | null guard, never committed |
| 0x00400000-0x00746FFF | trespass.exe (no relocations: must be exact) |
| 0x01000000-0x7EFFFFFF | "anywhere": heap (1 MB carves, size classes), stacks, relocated DLLs |
| 0x10000000-0x1001AFFF | SMACKW32.DLL at its preferred base (if mapped before the heap reaches it) |
| 0x780A0000-0x780B1FFF | MSVCIRT.DLL at its preferred base |
| 0x7F000000-0x7FFEFFFF | thunk VAs: never committed, a read faults |
| 0x80000000-0xFFFFFFFF | unused (trespass.exe is not large-address-aware) |
| 4 GB + 64 KB | host tail guard so `MEM64(0xFFFFFFFC)` faults instead of reaching host memory |

* Reservation is free (MEM_RESERVE of 4 GB + 64 KB); pages are committed when the loader, heap or
  a guest `VirtualAlloc(MEM_COMMIT)` asks. Guest page = host page (4 KB) on Windows x64.
* Nothing is ever executable (`PAGE_READWRITE`/`PAGE_READONLY`/`PAGE_NOACCESS` only).
* A VirtualAlloc(NULL, 4 GB) landed at 0x0000026C_F8E30000 here; a fixed reservation at
  0x0000100000000000 also succeeded, so a `RECOMP_MEM_BASE` build is possible (`gm_init(base, 1)`).
* A lifted guest DLL assumes the base it was lifted at. SMACKW32.DLL and Processor.dll both
  prefer 0x10000000; the probe maps Processor.dll relocated to 0x01000000. Map guest DLLs before
  the heap grows, and lift a DLL at the base it will really occupy.
* Pixel memory the guest locks (DirectDraw surfaces, DSound buffers, DIB sections) must be in the
  window. For GDI's `CreateDIBSection`, the host cannot place a section view inside a
  VirtualAlloc reservation without placeholder APIs (`VirtualAlloc2` + `MapViewOfFile3`,
  Windows 10 1803+) **[inferred]**; the alternative is a guest buffer copied at GDI calls.

## 4. SelfMod inventory **[verified: disassembly of game/trespass.exe; logs]**

Produced by `py -3 tools/x64_selfmod_scan.py` (capstone + pefile; reads the exe, the lifted
sources and `build/logs/*.log`).

* SelfMod is 0x0063B000-0x00644B75 (VirtualSize 0x9B75). 46 routine starts
  (`push ebp; mov ebp, esp` on 16-byte boundaries): 33 rasteriser routines and 13 stubs.
* Runtime: 24 distinct entries appear in `build/logs/trespasser_001..004.log`
  (`first native entry at ...`); every one is a routine start.
* Static: 267 lifted call/jump sites (`RECOMP_ICALL(0x0063....)`) in 104 lifted functions reach
  the 33 rasteriser routines; two `.data` pointers (0x0065EA7C, 0x0065EA80; the C++ static initializer table
  **[inferred]**) reach two stubs; 9 stubs are destructors the routines register with
  `atexit` (`call 0x00626B57` after a once-guard) **[the atexit identity is inferred]**.
* No code outside SelfMod writes into it: 0 absolute-address stores into the range in SelfMod,
  no `MEMx(0x0063....) =` in the lifted code, and all 42 apparent `.text` dwords pointing into
  SelfMod are byte coincidences inside other instructions (checked against the lifted listing).

### 4.1 Self-modifying writes

All 44 patches follow one pattern inside the routine's own prologue, before its loop:

```
lea  eax, [label]          ; label = the instruction after the patched one
mov  [eax-4], ecx          ; overwrite that instruction's trailing disp32
```

Every patched field is the **disp32 of a memory operand** and is the **last 4 bytes of its
instruction** (44/44), holding a placeholder in the file (0xDEADBEEF, or 0xDEADC0DE in four
routines). The value written is a guest address: texture base, colour-lookup (CLUT) base, or a
lighting table base. No opcode, immediate, branch displacement or jump target is ever patched.
The full list (writer, store, patched instruction, site, placeholder, source register) is the
second table printed by the scanner; the probe checks that all 44 still hold their placeholder.

### 4.2 Routines

Columns: x87 = x87 instructions / fdiv count; "persp" = has 4 `fdiv` (perspective
correction per sub-span) **[inferred]**, "linear" = no x87. All inner loops step texture
coordinates with the carry chain `add ecx, du; sbb ebp, ebp; adc edx, [ebp*4 + step]`.
Classification is from the inner loop's disassembly **[inferred]**.

| Entry | Size | Seen | Lifted callers (e.g.) | x87/fdiv | Patches | What it is |
|---|---|---|---|---|---|---|
| 0x0063B000 | 1808 | yes | sub_005E5530, sub_005875D0 | 181/4 | 0 | persp, 16-bit texels copied, zero texel skipped |
| 0x0063B710 | 1904 | yes | sub_0059ACD0, sub_005609C0 | 180/4 | 0 | persp, 16-bit texels, opaque |
| 0x0063BE80 | 464 | yes | sub_0059B320, sub_005614F0 | 0 | 0 | linear, 16-bit texels, opaque |
| 0x0063C050 | 464 | yes | sub_00588100, sub_005E6A90 | 0 | 0 | linear, 16-bit texels, zero skipped |
| 0x0063C240 | 656 | | sub_00564E70, sub_005B0180 | 0 | 0 | interpolated index -> 16-bit colour table, pixel stride in ebp |
| 0x0063C4D0 | 288 | yes | sub_0049FEE0, sub_004A1380 | 0 | 0 | interpolated byte into the high byte of each 16-bit pixel |
| 0x0063C5F0 | 2608 | yes | sub_005C37B0, sub_00571A30 | 240/4 | 2 | persp, 8-bit texel -> CLUT -> 16 bpp, index 0 skipped |
| 0x0063D020 | 2608 | yes | sub_00568EF0, sub_005B5F40 | 240/4 | 2 | persp, 8-bit texel -> CLUT, opaque |
| 0x0063DA50 | 528 | yes | sub_0056F400, sub_005BFFD0 | 0 | 2 | linear, 8-bit -> CLUT, index 0 skipped |
| 0x0063DC60 | 528 | yes | sub_005668C0, sub_005B20C0 | 0 | 2 | linear, 8-bit -> CLUT, opaque |
| 0x0063DE90 | 2672 | yes | sub_00572560, sub_005C4930 | 240/4 | 2 | persp, 8-bit texel + interpolated intensity selects CLUT row, index 0 skipped |
| 0x0063E900 | 2656 | yes | sub_00569A20, sub_005B6E30 | 240/4 | 2 | same, opaque |
| 0x0063F360 | 624 | yes | sub_005705D0, sub_005C0DE0 | 0 | 2 | linear, lit CLUT, index 0 skipped |
| 0x0063F5E0 | 608 | yes | sub_00567A90, sub_005B3E10 | 0 | 2 | linear, lit CLUT, opaque |
| 0x0063F850 | 2720 | | sub_0057E680, sub_005D6D40 | 240/4 | 3 | persp, 16-bit texel: 10-bit field minus a light angle -> byte table -> CLUT row, 6-bit colour; skipped if top 6 bits are 0 (bump-mapped lighting) |
| 0x006402F0 | 2672 | | sub_0057DB50, sub_005D5E70 | 240/4 | 3 | same, opaque |
| 0x00640D60 | 640 | | sub_0057C980, sub_005D4150 | 0 | 3 | linear, same, skipped |
| 0x00640FE0 | 592 | yes | sub_005D3340, sub_0057B7B0 | 0 | 3 | linear, same, opaque |
| 0x00641230 | 416 | yes | sub_00496370, sub_00497280 | 0 | 1 | linear, 8-bit texel stored unconverted as a 16-bit value, 0 skipped |
| 0x006413D0 | 416 | yes | sub_00498FA0, sub_0046BE90 | 0 | 1 | same, opaque |
| 0x00641570 | 368 | yes | sub_00535D20, sub_00538120 | 0 | 0 | span fill: keep each pixel's low byte, OR a constant high byte (2 px per dword) |
| 0x006416E0 | 432 | yes | sub_00534F10, sub_00532F40 | 0 | 1 | linear, 8-bit texel into the high byte of each 16-bit pixel, 0 skipped |
| 0x00641890 | 432 | | sub_00530160, sub_00531070 | 0 | 1 | same with a 0xFC mask test |
| 0x00641A40 | 480 | | sub_005946A0, sub_00595790 | 0 | 0 | read-modify-write: (pixel & mask) \| value through a 64 K-entry table at 0x6F2570 |
| 0x00641C20 | 2640 | yes | sub_00591270, sub_00590020 | 257/4 | 0 | persp, 16-bit texel > 0x4000 -> low 12 bits index a colour table, 2 px per iteration |
| 0x00642670 | 688 | yes | sub_005918C0, sub_005927D0 | 0 | 0 | linear, same |
| 0x00642930 | 2192 | yes | sub_0059E100, sub_0059EC40 | 180/4 | 3 | persp, (texel & mask) \| light[(i >> 12)] -> CLUT (Gouraud-lit) |
| 0x006431D0 | 752 | yes | sub_005A03A0, sub_005A15C0 | 0 | 3 | linear, same |
| 0x006434E0 | 528 | yes | sub_005A3830, sub_005A4D40 | 0 | 2 | flat colour \| light table -> CLUT (Gouraud-shaded, untextured) |
| 0x00643710 | 2048 | | sub_005A8720, sub_005A8FA0 | 180/4 | 2 | persp, texel combined with destination bits -> blend table (translucency) |
| 0x00643F10 | 560 | | sub_005626C0, sub_005ACD80 | 0 | 2 | linear, same |
| 0x00644140 | 2000 | yes | sub_005A80D0, sub_005A6E80 | 197/4 | 0 | persp, 16-bit texel copy, 2 px per iteration |
| 0x00644910 | 608 | | sub_005A9FA0, sub_005AAEB0 | 0 | 0 | linear, same |

Stubs: 0x0063C220 and 0x0063DE70 (static initializers, each calling an empty function at
0x0063C230 / 0x0063DE80); atexit-registered: 0x0063F5D0, 0x0063F840, 0x00642920, 0x006431C0,
0x006434C0, 0x006434D0, 0x006436F0, 0x00643700, 0x00644B70 (5 bytes, at the section's end).

### 4.3 Lift strategy

Preference 1 (recommended): **lift SelfMod with the existing lifter.** pcrecomp's `lift32.py`
already supports this: `Lifter(patch_sites=...)` makes an instruction whose trailing dword is in
`patch_sites` read that dword from guest memory (`MEM32(site)`) instead of emitting the file's
placeholder, for an immediate or a displacement (it was written for the same pattern in
Gunman's sw.dll and POD). All 44 Trespasser sites satisfy its precondition (trailing dword,
placeholder equal to the decoded disp32) **[verified by the scanner]**. Steps:

1. In a future `tools/run_lift.py` change: add the 46 SelfMod routine starts as entries and pass
   `patch_sites` = the 44 VAs (scanner output); keep `precise_carry`/`precise_sbb` (already on;
   the carry-chained steppers need them).
2. The prologue stores (`mov [eax-4], ecx`) stay ordinary guest stores into the image bytes,
   which are guest data in the x64 host; the lifted loop reads them back. No host state, no
   code generation, no executable memory.
3. Validate before switching: `external/pcrecomp/tools/lift/difftest.py` (Unicorn as the
   reference, not a runtime component) per routine on synthetic spans, and in the x86 build a
   side-by-side run of lifted vs native SelfMod on the same inputs, comparing framebuffers.
4. Then `selfmod.c` is not linked into the x64 host; "SelfMod native x86 = 0" by construction.

Cost: one guest load per patched operand per pixel (the compiler cannot hoist it past guest
stores). Acceptable first; a later optimisation can load each site once after the prologue.

Preference 2 (fallback): hand-written C for a routine whose lift misbehaves. The inner loops
are 9-21 instructions and provable, but the perspective setup (180-257 x87 instructions) is
large, so this is a last resort per routine, not a plan for all 33.

## 5. DLL inventory and replacement plan

From `pefile` on `game/` and `build/logs/trespasser_004.log` **[verified]**; the probe binds all
297 imports (257 to host thunks, 40 to guest DLL exports) **[verified]**.

| DLL | Source | Used by trespass.exe | x64 plan |
|---|---|---|---|
| KERNEL32 | system | 59 imports | HLE shims forwarding to x64 kernel32: files (CreateFileA/ReadFile/WriteFile/SetFilePointer/Find*/attributes/times), memory (VirtualAlloc/Free -> guest window; GlobalMemoryStatus/GetSystemInfo answered as a 2 GB 32-bit process), sync (critical sections: guest CRITICAL_SECTION is 24 bytes, host 40 -> map by guest address), events/mutex/semaphore handles pass through, threads (guest stack + TIB), LoadLibraryA/GetProcAddress/FreeLibrary over a module table, resources from the guest image's .rsrc, WinExec refused |
| USER32 | system | 75 | forward to x64 user32 with marshalling: MSG (28 vs 48 bytes), RECT/POINT (same layout, copy), WNDCLASSA (lpfnWndProc -> host proc + guest proc table), dialogs (templates are pointer-free; DLGPROC adapter), `wsprintfA`/`wvsprintfA` varargs, `SetWindowLongA(GWL_WNDPROC)` |
| GDI32 | system | 30 | forward; handles 32-bit; `CreateDIBSection` bits must be guest memory (see section 3) |
| DDRAW | system | 2 (+ `LoadLibraryA("ddraw.dll")`/`DirectDrawCreate`) | existing `src/host/ddraw` + D3D11 renderer, re-plumbed to guest COM objects (thunk vtables), surface memory in the guest window, guest layouts for DDSURFACEDESC(2) (lpSurface is 4 bytes in the guest), DDCAPS, D3DDEVICEDESC; D3DTLVERTEX is pointer-free |
| DSound.dll | system, dynamic | `DirectSoundCreate` via GetProcAddress | HLE DirectSound on XAudio2 or WASAPI: IDirectSound, IDirectSoundBuffer, 3D listener/buffer; IID_IDirectSound3DListener/Buffer, IKsPropertySet and the EAX listener property-set GUID are present in the image **[references not traced]**; `Lock` must return guest pointers |
| A3d.dll | absent | `A3dCreate` via LoadLibrary | answered "not found" by the host without a LoadLibrary |
| smackw32.dll | game dir, 32-bit, base 0x10000000, relocs | 14 | lift SMACKW32.DLL as a guest DLL at 0x10000000 (its structs and callbacks come for free); HLE its own imports (USER32 41, WINMM 11 incl. `timeSetEvent` callback and waveOut, KERNEL32 43, GDI32 19). Later option: host Smacker decoder behind the 14 exports with the guest `Smack` struct layout |
| MSVCIRT.DLL | game dir, 32-bit, base 0x780A0000, relocs | 26 incl. data (`cout`, `cerr`, `filebuf::openprot`) | lift as a guest DLL (data exports and thiscall vtables need guest memory); HLE its imports (MSVCRT 43, KERNEL32 5) |
| MSVCRT | system (not redistributable) | 79 incl. data (`_acmdln`, `_adjust_fdiv`) | HLE: heap on the guest heap; stdio/`FILE` in guest memory; varargs (`sprintf`, `sscanf`, `printf`, `fprintf`, `vfprintf`) from guest memory; `_open/_read/_write/_lseek/_close`, `_find*` with guest `_finddata_t`; x87 helpers (`_ftol`, `_CI*`) from the lifted FPU stack (x87crt logic); `_initterm`, `_onexit`, `__dllonexit`, `qsort` call guest code; `_except_handler3`/`_XcptFilter` with eh32; `__RTtypeid`/`__RTDynamicCast` over the guest image's RTTI; `_errno`, `__p__fmode`, `__p__commode` return guest addresses |
| WINMM | system | 1 (`joyGetPosEx`) | forward (JOYINFOEX is pointer-free) |
| ADVAPI32 | system | 6 registry | port `src/host/registry.c` logic; HKEYs 32-bit; value buffers copied |
| LZ32 | system | 4 | forward to x64 lz32 (OFSTRUCT is pointer-free) |
| COMCTL32 | system | ordinal 17 | no-op |
| Processor.dll | game dir, 32-bit, base 0x10000000, relocs | `LoadLibraryA` + `bGetProcessorInfo` | small HLE reimplementation (CPU identification) **[signature not yet derived]**; it collides with SMACKW32's base, a second reason not to lift it |

Marshalling hazards by family: window procs (pointer lParams: WM_CREATE, WM_GETMINMAXINFO,
WM_WINDOWPOSCHANGING, WM_NCCALCSIZE, WM_SETTEXT; wParam/lParam widen to 64 bits), dialog procs,
registry (byte buffers and lengths), file APIs (WIN32_FIND_DATAA is pointer-free; OVERLAPPED is
not used), CRT `FILE*` and `errno` identity, COM vtables (every method a thunk, `this` a guest
object, out-pointers written as guest VAs only), DirectDraw (surface descriptors with embedded
pointers; locked pixels), DirectSound (DSBUFFERDESC.lpwfxFormat; Lock's two pointers),
Smacker (if not lifted: the `Smack` struct the game reads).

## 6. What exists now

| File | What |
|---|---|
| `src/host64/guest_memory.h/.c` | 4 GB window (optional fixed base), 64 KB reservations with null guard, thunk hole and tail guard, commit/decommit/protect, VA <-> host with range and commit checks, guest heap (size classes) and per-thread scratch, qword scan for leaked host pointers, PE32 mapping (fixed or relocated), IAT binding with a resolver, export lookup |
| `src/host64/guest_ptr.h/.c` | `guest_ptr_t`, `gp_to_host`/`gp_to_guest`, `gp_copy_in/out`, `gp_store_ptr` (refuses host pointers outside the window), `gp_store_u32` (refuses values > 0xFFFFFFFF), leak counter, checked guest strings, `GP_GUEST_LAYOUT` size asserts |
| `src/host64/guest_thunks.h/.c` | thunk registry (VA -> host function, argc, convention), `ht_invoke` (stdcall/cdecl against the guest stack, eax/edx), guest vtables of thunks, host objects behind guest COM pointers with forgery/stale checks |
| `src/host64/probe_main.c` | `trespasser_x64_probe`: maps trespass.exe, SMACKW32, MSVCIRT, Processor.dll, binds all imports, checks the SelfMod placeholders; headless |
| `tests/x64_tests.c` | 346 headless checks (311 + 1 skip without game files) |
| `tools/x64_selfmod_scan.py` | the SelfMod inventory in section 4 |

Build and results **[verified]** (scratchpad build, `vcvarsall amd64`):

```
cl /nologo /W3 /WX /O2 /MD /D_CRT_SECURE_NO_WARNINGS /Isrc\host64 /Iexternal\pcrecomp\runtime\win32hle ^
   tests\x64_tests.c src\host64\guest_memory.c src\host64\guest_ptr.c src\host64\guest_thunks.c /Fe:x64_tests.exe
x64_tests                -> 346 passed, 0 failed, 0 skipped  (exit 0)
x64_tests C:\nonexistent -> 311 passed, 0 failed, 1 skipped  (exit 0)
trespasser_x64_probe     -> 297 bound, 0 unresolved; 44/44 patch sites hold placeholders (exit 0)
```

Also no warnings from a `/W4` syntax pass (`cl /W4 /Zs`). Tests cover: window placement and fault isolation (null page,
unmapped image range, last byte, tail guard all fault under SEH), VA conversion and bounds,
read-only and decommitted pages, reservation rules, heap reuse/realloc/large blocks, scratch,
refused host-pointer and >32-bit stores, the leak scan, guest MSG <-> host MSG, trespass.exe
mapping (base, entry, sections, SelfMod placeholder, zeroed .bss), SMACKW32/MSVCIRT at preferred
bases, Processor.dll relocation checked against the file, IAT binding (297) with every slot a
thunk or a guest export, and a COM object called through a guest vtable (QueryInterface returns
the guest object, a method that tries to return its host pointer is refused, forged/stale
objects resolve to nothing, edx:eax returns, stdcall stack balance).

Not done: no lifted code is linked into a 64-bit binary, no import shim is implemented, nothing
has been executed as guest code.

### CMake (to paste; x64-only, needs neither native32 nor the lifted code)

Checked **[verified]**: pasted verbatim into a scratch project (CMake + Ninja, `vcvarsall amd64`), it
configures and builds `host64`, `trespasser_x64_probe` and `x64_tests`, and both programs pass.
The current `CMakeLists.txt` stops any 64-bit configure at its "host must be a 32-bit process"
check, so this block goes **before** that check; it ends with `return()`.

```cmake
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(PCRECOMP "${CMAKE_SOURCE_DIR}/external/pcrecomp" CACHE PATH "pcrecomp checkout (tools/fetch_pcrecomp.py)")
  add_library(host64 STATIC src/host64/guest_memory.c src/host64/guest_ptr.c src/host64/guest_thunks.c)
  target_include_directories(host64 PUBLIC src/host64 ${PCRECOMP}/runtime/win32hle)
  target_compile_options(host64 PRIVATE /W3)
  target_compile_definitions(host64 PRIVATE _CRT_SECURE_NO_WARNINGS)
  add_executable(trespasser_x64_probe src/host64/probe_main.c)
  target_link_libraries(trespasser_x64_probe PRIVATE host64)
  target_compile_options(trespasser_x64_probe PRIVATE /W3)
  target_compile_definitions(trespasser_x64_probe PRIVATE _CRT_SECURE_NO_WARNINGS
    TRESPASSER_DEFAULT_GAME_DIR="${CMAKE_SOURCE_DIR}/game")
  add_executable(x64_tests EXCLUDE_FROM_ALL tests/x64_tests.c)
  target_link_libraries(x64_tests PRIVATE host64)
  target_compile_options(x64_tests PRIVATE /W3)
  target_compile_definitions(x64_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  return()
endif()
```

## 7. Remaining blockers, in priority order

1. **Runtime spine for x64**: define the 21 runtime symbols, the machine lock, guest TIB and
   stacks, `recomp_lookup_import` -> `ht_invoke` trampoline, and host -> guest calls; then link
   the lifted code into a 64-bit binary (compiles today, links with a stub only).
2. **Import shims**: 257 imports across KERNEL32/USER32/GDI32/MSVCRT/ADVAPI32/LZ32/WINMM/
   COMCTL32/DDRAW, each with argc/convention and marshalling; MSVCRT is the largest and
   includes varargs, `FILE`, RTTI and SEH entry points.
3. **DirectDraw/Direct3D layer as guest COM objects**: move `src/host/ddraw` objects to thunk
   vtables and guest-memory surfaces/descriptors (the D3D11 renderer itself stays host-side).
4. **SelfMod lift** with `patch_sites` and its differential validation; until then the software
   renderer has no x64 path (the D3D11 renderer does not use SelfMod **[inferred]**).
5. **Guest DLLs and audio**: lift SMACKW32.DLL and MSVCIRT.DLL (their own imports need shims;
   SMACKW32's `timeSetEvent`/waveOut callbacks), HLE DirectSound (+3D/EAX as used) and
   Processor.dll; window procs, dialogs and threads (Suspend/Resume) under the machine lock.

## 8. Acceptance criteria for the 64-bit process

* Process Explorer (or `IsWow64Process` = FALSE on x64 Windows) shows a 64-bit process; the
  module list contains no 32-bit DLL (no SMACKW32, MSVCIRT, Processor, msvcrt-x86, ddraw-x86).
* Game data loads (`*.tpa`, `*.smk`, menu `.ddf`/`.tga`, `.swp`), the intro movie plays.
* Main menu works; New Game starts; the Beach level loads.
* Mouse and keyboard input work in menu and gameplay.
* Audio plays (music, effects, 3D positioned sounds).
* Both renderers draw: software (lifted rasteriser + lifted SelfMod) and D3D11.
* SelfMod native x86 entries = 0 (no native execution path exists in the binary).
* Guest pointer leaks = 0: `gp_leak_count()` = 0 and `gm_bad_va_count()` = 0 at exit, and a
  qword scan of guest memory for host-module and host-heap addresses finds nothing.
