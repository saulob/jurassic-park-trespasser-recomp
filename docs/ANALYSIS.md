# Analysis of the English disc

Everything here was read from the disc image and the executable with headless tools.
Nothing was installed, launched or opened. Reproduce with `tools/extract_game.py` and
`tools/analyze.py`.

## Disc image

| | |
|---|---|
| File | `Trespasser_EN.iso`, 652,992,512 bytes |
| SHA-256 | `04e26e7a8fc88bd3956e0854cc71335e408e064e9e56a907bb7a15b9373b6afd` |
| Volume | `Trespass_UK_116`, created 1998-10-19, ISO 9660 with Joliet names |
| Contents | 1,698 files, 650,641,485 bytes |

| Directory | Files | Bytes | What |
|---|---:|---:|---|
| `/` | 9 | 1.7 MB | `SETUP.EXE` (7 KB launcher), `Ip.exe`, `Autorun.inf`, readme, help |
| `setup/` | 15 | 10.2 MB | three game builds, `setup95.exe` installer, runtime DLLs, DirectX setup stubs |
| `setup/DirectX/` | 1,424 | 47.4 MB | DirectX 6 redistributable |
| `data/` | 53 | 514.2 MB | levels (`.scn` `.grf` `.pid` `.spz` `.wtd`), audio packs (`.tpa`) |
| `data/menu/` | 198 | 77.2 MB | 175 `.tga`, 18 `.ddf` dialog definitions, 4 Smacker videos, `mouse.bmp` |

Levels on the disc: `as`, `as2`, `be`, `ij`, `it`, `jr`, `lab`, `sum`, `TestScene`, `TestScnNght`.
The executable also names `pv.scn`, which is not on the disc.

## Executables

| File | Size | SHA-256 |
|---|---:|---|
| `setup/tpassp6.exe` (Pentium Pro/II, **the target**) | 2,568,192 | `d88bb548d8f7c893de2d5a2ffb69fa513a3d75b7e5c336990b63ec5cad5679ed` |
| `setup/tpassp5.exe` (Pentium) | 2,592,768 | `9c2f46098307fac2a3689616657c615f67a2113584f09290d5e489e538991816` |
| `setup/tpassk6.exe` (K6 / 3DNow!) | 2,367,488 | `701701f7b6bedd263bc36c4ed3e2379aed743af929075c6490b22138ce6acbd3` |
| `setup/setup95.exe` (installer) | 1,851,392 | `a6daf9f2b7384d3a9167f5d461dacee66f8d184db55ec9f4f62ba47a64175f41` |
| `setup/MSVCRT.DLL` | 254,005 | `fb6260b07c05bdbcb6928156da514be5b6b69f095b335692ba46c561d85bd5f8` |
| `setup/MSVCIRT.DLL` | 77,878 | `647f7534aaaedffa93534e4cb9b24bfcf91524828ff0364d88973be58139e255` |
| `setup/SMACKW32.DLL` | 96,256 | `f9b2fdb5ebc8e659c7ac132c213fcfd2eb059a1195a129121bb68ca21699e5e1` |
| `setup/Processor.dll` | 32,768 | `c421754f95961bde4a843dec7c3170bbb22bda256f543c3ae05a5104af9a72fd` |

## The target: `tpassp6.exe`

```
Format        PE32, i386, Windows GUI, MSVC 6.0 linker
Version       1.0.116 "Trespasser Pentium Pro build", OriginalFilename trespass.exe
Timestamp     1998-10-09 17:06:31 UTC
Image base    0x00400000, SizeOfImage 0x00347000
Entry point   0x00626BE2 (CRT startup) -> WinMain 0x0040E2E0 -> 0x0040DB10
Stack / heap  1 MB reserve each
Relocations   none: the image only works at 0x00400000
Exports, TLS  none
```

| Section | VA | Virtual size | Flags |
|---|---|---:|---|
| `.text` | 0x00401000 | 0x239DB0 | code, execute, read |
| `SelfMod` | 0x0063B000 | 0x009B75 | code, execute, read, **write** |
| `.rdata` | 0x00645000 | 0x017474 | read |
| `.data` | 0x0065D000 | 0x0E4408 | read, write (0x11000 on disk, the rest zero-filled) |
| `.rsrc` | 0x00742000 | 0x0044B8 | read |

No packer and no copy protection: ordinary entropy (6.57 on `.text`), entry point in `.text`,
no protection sections. C++ exceptions are not used (no `__CxxFrameHandler` import); structured
exception handling is (`_except_handler3`, around the body of `WinMain`).

### Imports: 297 functions, 11 DLLs

| DLL | Count | Resolved on this Windows 11 by |
|---|---:|---|
| MSVCRT.dll | 79 | `SysWOW64\msvcrt.dll` (a KnownDLL, so the disc copy is never used) |
| USER32.dll | 75 | system |
| KERNEL32.dll | 59 | system |
| GDI32.dll | 30 | system |
| MSVCIRT.dll | 26 | the disc's `MSVCIRT.DLL`, placed in `game/` (Windows no longer ships it) |
| smackw32.dll | 14 | the disc's `SMACKW32.DLL`, placed in `game/` |
| ADVAPI32.dll | 6 | registry only |
| LZ32.dll | 4 | system |
| DDRAW.dll | 2 | system (`DirectDrawCreate`, `DirectDrawEnumerateA`) |
| WINMM.dll | 1 | system (`joyGetPosEx`) |
| COMCTL32.dll | 1 | system (ordinal 17) |

Checked statically against the export tables of the 32-bit DLLs installed here: **297 of 297
resolve**, and so do the imports of `MSVCIRT.DLL` (48/48), `SMACKW32.DLL` (114/114) and
`Processor.dll` (45/45). Loaded at run time by name rather than imported: `processor.dll`,
`DSound.dll`, `A3d.dll`, `ddraw.dll`.

Five imports are data, not functions (`_acmdln`, `_adjust_fdiv`, `cout`, `cerr`,
`filebuf::openprot`). `_ftol` and the `_CI*` math helpers take their operands on the x87 stack.

### SelfMod

39,797 bytes, 10,245 instructions of which 2,615 are x87. It is not generated code: the
routines are complete in the file, and they write constants into their own instructions
(`lea eax, [label]` followed by stores) before running the inner loop.

- 33 routines are entered from `.text`, all by direct `call` (272 call sites). No `jmp` enters it.
- `.text` holds 18 distinct absolute addresses inside it and `.data` 10.
- Its only calls out go to `0x00626B57` (`atexit`, nine sites: static-local registrations).
- No `fs:` access, no MMX/SSE.

### Instruction set

Pure x86-32 integer plus x87. No MMX, SSE or 3DNow! in the P6 build. Nine sites use `DC D8`,
an undocumented alias of `fcomp st(0)` that capstone refuses to decode; see
`config/decoder_fixups.json`.

### C++ metadata

RTTI is intact: 513 type descriptors, 363 vtables, 362 classes, 1,467 virtual methods.

## What the game needs to start

Traced statically from `WinMain` (`0x0040DB10`):

1. Registry key `HKLM\Software\DreamWorks Interactive\Trespasser`, opened with full access and
   created if missing. About fifty values live there (video, audio, key map).
2. One instance only: mutex `DWI Trespasser MUTEX`.
3. `processor.dll` must report a Pentium Pro class CPU, otherwise a message box and exit.
4. Value `Installed` must be non-zero, otherwise an error dialog and exit.
5. **The media check**: `<Data Drive>data\menu\tpassintro.smk` must exist
   (`GetFileAttributesA`). If not, a "insert the CD" dialog loops. This is a presence test on a
   file, not copy protection.
6. Free disk space on the drive of `Installed Directory`, plus existing `*.swp` files, must
   cover `SwapSpaceMb`.
7. Then DirectDraw and video-driver selection, the window, and the menu.

`Data Drive` and `Installed Directory` are path prefixes written by the installer. Level data
is read from `<Data Drive>data\`, menus from `data\menu\`. The game writes `*.swp`,
`AutoSave.scn`, `error.log` and several optional `*.txt` logs into its working directory.

So a working layout is: the executable and its three DLLs in one directory, the disc's `data\`
tree beside them, and both registry paths pointing at that directory. `tools/extract_game.py`
builds exactly that in `game/`.

## Differences from the reference project

[sp00nznet/trespasser](https://github.com/sp00nznet/trespasser) documents its disc in
`docs/RECON.md`. It publishes no hash, so identity is established field by field.

| | Reference | This disc |
|---|---|---|
| ISO size | 744,597,504 | 652,992,512 (volume `Trespass_UK_116`) |
| `UPDATE\tresp1_1\PatchEXEOnly.exe` (1.1 patch) | present | **absent** |
| `tpassp6.exe` size | 2,568,192 | 2,568,192 |
| Timestamp | 1998-10-09 17:06:31 UTC | identical |
| Image base, entry point | 0x00400000, 0x00626BE2 | identical |
| Section table (5 sections, VAs, sizes) | as in RECON.md | identical |
| Imports | 297 from 11 DLLs, same per-DLL counts | identical |
| RTTI type names | 513 | 513 |
| SelfMod references from `.text` / `.data` | 18 / 10 distinct | 18 / 10 distinct |
| `tpassp5.exe`, `tpassk6.exe` sizes | 2,592,768 / 2,367,488 | identical |

Every published property of the target matches. The discs differ around it: this one is a
different pressing and carries no 1.1 patch, so only retail 1.0.116 is available here.

The reference's function count (11,122) is lower than ours (17,912 entries: 11,785 function
starts and 6,127 alias entries, 145 of them inside SelfMod) because pcrecomp's recovery was
improved after that count was published.
