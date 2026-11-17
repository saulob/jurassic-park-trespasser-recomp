# Game versions

The launcher imports a version from the player's own disc image into `game\versions\<id>`
([LAUNCHER](LAUNCHER.md#import-game)); `src/common/jpt_game_version.c` is the one table of versions, shared by the
host, the launcher and the tests. A disc is recognised by file digests, never by its file name or volume label.

## Known versions

| Volume | What it is | Version |
|---|---|---|
| `Trespass_UK_116` | DreamWorks Interactive retail, UK pressing, 1998-10-19 | `en-116` |
| `Trespasser_116d` | DreamWorks Interactive retail, US pressing, 1998-10-12: the UK disc's game files byte for byte, plus EA online registration (`ereg\`) and a shorter EA help file | `en-116` |
| `TRESPASSER_S` | Official Spanish (Spain) release, build 117 (string table, menus and readme in Spanish) | `es-117` |
| `Trespasser` (Brazilian Portuguese release) | Official Brazilian Portuguese release, build 117; English readme; a 1998 developer `data\error.log` mastered on the disc. The analyzed disc image contains five damaged level files with a one-bit error each (byte 470 of a sector); this describes that copy, not the release | `ptbr-117` |
| `TRESPASSER_116D` | Fargus Russian localization/repack of the retail 1.0.116 release: InstallShield 5 installer, Russian text, menus, subtitles and videos | `ru-116` |

No Japanese release is present among the analyzed versions.

## Executables

The original executables are development inputs only and are not required to install or play the recomp. `setup/tpassp6.exe`
(Pentium Pro/II build) is what the importer turns into each version's `guest.img`; the port's lifted code was generated from
the 1.0.116 one. The build number is the one the executable prints in "Trespasser Build: 1, 0, N".

| Executable | Size | PE timestamp (UTC) | Build | SHA-256 |
|---|---|---|---|---|
| retail `tpassp6.exe` (UK, US, and inside the Russian cabinet) | 2,568,192 | 1998-10-09 17:06:31 | 116 | `d88bb548d8f7c893de2d5a2ffb69fa513a3d75b7e5c336990b63ec5cad5679ed` |
| retail `tpassp5.exe` | 2,592,768 | 1998-10-09 16:40:30 | 116 | `9c2f46098307fac2a3689616657c615f67a2113584f09290d5e489e538991816` |
| retail `tpassk6.exe` | 2,367,488 | 1998-10-09 17:31:50 | 116 | `701701f7b6bedd263bc36c4ed3e2379aed743af929075c6490b22138ce6acbd3` |
| Spanish `tpassp6.exe` | 2,584,576 | 1998-11-07 22:19:21 | 117 | `cfc95e60fcf8ea505ed586e24002858324308774cc5197bac85205633360f753` |
| Brazilian `tpassp6.exe` | 2,592,768 | 1998-11-17 00:44:01 | 117 | `d5de43a68f57caf7644d8045bc8c27936f03357f453e326e846c1f745f1f0040` |
| Fargus `SETUP/TRESPASS.EXE` | 2,592,768 | 1998-10-09 16:40:30 | 116 | `274007448ea4815f3a7ff99df5ed750db831c8eba21415bdd9f8bca375dff299` |
| official Patch 1.1 `trespass.exe` | 2,555,904 | 1998-11-24 03:25:05 | 117 | `7b17f534b982e4797c33d8d1047d944bc660e0859cdd4567558e7649ff57bf61` |

- **Fargus**: the retail `tpassp5.exe` with identical `.text`, `.rdata` and `SelfMod`; only `.rsrc` (the string table,
  Russian as code-page-1251 bytes in UTF-16 units) and one `.data` string (the font face `Arial` became `Tresp`)
  changed. The hints are written for its `Tresp` font, which draws Latin keys as the Cyrillic letters of the Russian
  keyboard (`Ult z?` is «Где я?»). The port runs the retail `tpassp6.exe` from the same disc and reproduces both
  changes without touching guest code: the string table comes from `text\strings.dat` (made by the importer from the Fargus resources), and the font is loaded
  privately and replaces the game's only face (`src/host/l10n.c`).
- **Spanish and Brazilian**: separate compiles of the 117 line, 2-3 weeks before the official 1.1 patch, with the
  menu labels moved into the string table (10000+, as in 1.1). Their code is not the retail code at other addresses:
  `tools/guest_map.py` finds 84 % of the retail functions byte-identical after masking addresses; the rest
  changed. Patch 1.1's fixes differ between them: `CPartition::bAddChild` (`0x00475FA9`, `push 0` in 116) is still the
  116 code in the Spanish build and already the 1.1 code in the Brazilian one.

## Game data

| Version | Differs from the retail data |
|---|---|
| `en-116` | the reference (256 files, 594,182,171 bytes) |
| `ru-116` | `Stream.tpa`, `Effects.tpa` (Russian), `be.grf` (7.2 MB instead of 2.7 MB), 4,664 bytes of `lab-130.spz`, the menu art and 5 DDFs, the three videos re-encoded |
| `es-117` | `Stream.tpa`, `Effects.tpa`, BE's `be.grf`/`be.wtd`, the menu art and DDFs, the videos (Spanish) |
| `ptbr-117` | the same files as Spanish, in Portuguese; no `testscnnght-130.spz` |

## Lifts and the one program

The recomp uses one executable, `trespasser_recomp.exe`, and selects the appropriate lifted build for the installed game
version. One lift is generated from one executable; a version names its lift in the table (`en-116`, `ru-116`:
`lift116`; `es-117`: `lift_es117`; `ptbr-117`: `lift_ptbr117`), never by language, image or file name. All three are
linked into the program, every symbol of a lift carries its prefix (`lift116_sub_<va>`, `lift_es117_sub_<va>`, ...), and
the version the launcher selects chooses one, once, before any guest code runs. Only the active lift executes: there is
one guest address space, reserved once.

The original executables are development inputs only and are not required to install or play the recomp. The lifts are
project artifacts: `lifts/<lift>.tar.xz` with `lifts/manifest.json` (archive and content SHA-256, profile, the SHA-256 of
the build each was generated from, the generator format), unpacked by CMake at configuration, so a clean checkout builds
without any executable. Maintainers regenerate one with `tools/generate-lift.ps1 -Profile <id> -Source <executable>`;
the same sources pack to the same archive. The source SHA-256 is metadata embedded in the lift and in the build's
expectations; nothing compares it with a file on the player's machine.

| Step | Tool |
|---|---|
| facts of a build's executable (hash, entry, SelfMod, decoder fixups) | `py -3 tools/make_profile.py <id> --exe <trespass.exe> --gen-dir gen_<x> --lift lift_<x>` writes `config/profiles/<id>.json` |
| catalogue, RTTI and lift of a build | `tools/generate-lift.ps1` (maintainers): `py -3 tools/analyze.py --profile <id>`, `py -3 tools/run_lift.py --profile <id>` (`JPT_PROFILE_EXE` names the executable) into `analysis/<id>/` and `src/recomp/gen_<x>/` (not committed), then `py -3 tools/lifts.py pack <lift>` |
| every host guest address in another build | `py -3 tools/guest_map.py <id> --write` writes `config/guest_maps/<id>.json` |
| the stable entries and hooks of a lift | `guest_override_list.h` and `lift_hook_list.h` through the profile's guest map: only addresses the map proves are lifted |
| structure layouts the host reads | `py -3 tools/class_layout.py <id>` (evidence), `py -3 tools/host_offsets.py --write` (`config/host_offsets.json`), `py -3 tools/gen_guest_va.py` (the layout table in `src/host/profiles`) |
| displacements changed in aligned code (catalogue) | `py -3 tools/layout_diff.py <id>` |
| rules and the feature matrix | `py -3 tools/check_profiles.py --matrix`, `py -3 -m unittest tests.test_profiles` |

Each lift's `recomp_dispatch.c` carries its metadata (lift id, source profile and executable, generator version, entry
point, image and SelfMod layout). When it selects the version the host compares it with what the build expects
(`src/host/profiles/guest_profile_data.c`) and refuses a stale lift, logging both, before any guest code runs.

## Guest address maps

Both executables are decoded, functions are aligned (unique bodies, link order, the call graph, the same virtual slot of
a class whose other slots agree, and as a last step unmatched runs and similar bodies) and every address in `src/host`
is classified (341 real addresses; comments, strings, colours and `config/guest_va_exclude.json` are not addresses).
**Proven**: identical function, a function matched by its code or its class, a global that aligned code pairs with a
clear majority, or a decision in `config/guest_decisions/<id>.json` (PROVED_CHANGED_EQUIVALENT, with the diff evidence).
**Absent**: a decision NO_EQUIVALENT, only for the 116-to-1.1 hand, jump and tuning port that the build has natively.
Candidates and unresolved addresses block a version.

| | functions matched | host addresses | proven | absent | candidate | unresolved | required overrides |
|---|---|---|---|---|---|---|---|
| `es-117` | 17,360 of 17,912 | 341 | 321 | 20 | 0 | 0 | 54 of 54 |
| `ptbr-117` | 17,020 of 17,912 | 341 | 312 | 29 | 0 | 0 | 54 of 54 |

## The active profile and guest addresses

`src/host/active_profile.c` holds the profile the process runs, chosen once from the selected version (`jpt_profile_activate`,
called by `lift_activate` in `lift_registry.c`). `src/host` reaches every guest address through `GVA(0x<116 address>)`: an
index into `jpt_gva`, the active lift's table copied at activation, so an access costs one load. `GSUB` is the lifted
function at such an address in the active lift (its table is copied the same way) and `GHOOK` the host function the
lift calls at a hook site, named by the 1.0.116 address in every lift. The tables, the patch groups the host ports onto
each lift (`JPT_P117_*`, run-time values), the layout fields that moved (`GLAYOUT_*`) and each lift's expected metadata are
generated into `src/host/profiles` by `tools/gen_guest_va.py`; a guest address the host uses but no lift has is an error
of the generator, and an address missing from the active lift without a patch group that explains it refuses to start.
`CMakeLists.txt` builds the single `trespasser_recomp` target from the same sources and all the lifts. `py -3
tools/gva_convert.py --check` fails on a raw guest address in `src/host`. The profile tables and the refusals are tested
without starting a process (`tests/profile_dispatch_tests.c`, `tests/test_single_host.py`).

## Structure offsets and calling conventions

`tools/abi_check.py` compares, for every host-used guest function whose body changed, the stack bytes each `ret`
releases and the use of ecx/edx as arguments (`config/abi_results`, decisions in `config/abi_decisions.json`):
`es-117` 131 identical, 9 changed, 0 mismatched; `ptbr-117` 123 identical, 16 changed, 0 mismatched (two recorded: one
function that only renames registers, and `0x004E85A0`, which gains an argument but is only used by the box-box port
this build does not apply).

`config/host_offsets.json` lists every structure offset `src/host` hard-codes: the 388 named `#define NAME 0x..` and the 92 raw
`+ 0x..` literals inside guest memory accessors (`@0x..`; `config/host_literals_allow.json` classifies the ones that are host
buffers, file formats, stack arguments or types of the language). Per build each is `unmoved`, `moved` (with its value),
`not_guest` (COM vtable slot, file format, host constant, the Smacker DLL's own structure), `gated` (every use is in code
that runs only when a patch group the build does not apply is on) or `unproven`. It says: `es-117` 405 unmoved, 38 not a guest
structure, 37 gated, **0 unproven**; `ptbr-117` 381 unmoved, 1 moved, 38, 60 gated, **0 unproven**. A version cannot be marked
playable while any is unproven, and `tests/test_layout.py` fails on an offset the manifest does not classify.

What proves a field unmoved is evidence from the code that owns it (`config/layout_structs.json` assigns each name to an RTTI
class or a set of guest functions; `tools/class_layout.py` compares those functions with their counterparts): an identical
function, or an aligned instruction of a changed one, accesses that displacement and none moves it; an array's stride is
built by the same address arithmetic (`lea`, `shl`, `imul`); a static table is byte-identical; a hooked function's entry
sequence reserves the same frame; a vtable slot holds a function matched by its code; a raw offset in a file that overrides
guest functions is read by those functions. A displacement that differs in one aligned instruction of a restructured
function does not move the field: `0x40BCC0` reads `[object+8]`/`[object+0xC]` in 116 and `[object+0x7C]`/`[object+0x80]` in
both 117 builds because 1.1 clips the pause cursor to the display size instead of the raster size (the same instruction
sequence, another field of the same object; `RASTER_DISPLAY_W/H` at `+0x7C/+0x80` is unmoved in every build).

The one field that moved is the box pool stride, `0x1090` in 116 and `es-117`, `0x1064` in `ptbr-117`
(`GLAYOUT_BOX_POOL_STRIDE`, a field of the layout table of each lift in `src/host/profiles`, copied into `jpt_layout` when
the profile is activated). The pelvis, biotag and kontrol strides, the terrain quality rows and the fonts' width tables are the same in
every build.

## Patch 1.1 in the 117 builds

The 117 builds are earlier compiles of the 1.1 code. Compared function by function with the 116 and the official 1.1
executables:

| Fix (116 address) | `es-117` | `ptbr-117` |
|---|---|---|
| hand: empty reach, stow handler, impulse (`0x45B470`, `0x460C4B`, `0x456903`) | native (identical to 1.1) | native |
| jump hand-off (`0x4F0960`) | native | native |
| Foot_IDOF, failed jump check (`0x4E784D`) | neither 116 nor 1.1 (0.96 like 1.1): to analyse | native (identical to 1.1) |
| `CPlayer::Process` | to analyse (0.47 like 116) | native (identical to 1.1) |
| `CPartition::bAddChild` (`0x475F60`) | **116 code: needs the fix** | native |
| box-box (`0x4E85A0`, `0x4E86B0`, `0x4EDFA0`, `0x4E9780`) | **116 code: needs the fixes** | native |
| pair wake (`0x4DC600`) | to analyse | to analyse |
| level tolerances and object placements | the level files are the retail ones: apply as in 116 | the same |

`ptbr-117` is the official 1.1 code with its own text: 16,549 of its 17,992 functions are identical to the official
executable. `es-117` is an intermediate build (15,934 of 17,975). A fix is applied to a build only where it is not already
there; never twice.

## Structure layouts

The 117 compiles reordered code, and `tools/layout_diff.py` finds displacements that differ in aligned instructions of
108 of 402 changed functions of `es-117` (for example `0x34` to `0x28` in 22 functions): almost all are stack locals
(`tools/class_layout.py` skips esp- and negative-ebp-based operands), a register reallocation, or another field of the same
object. A displacement is a field only when the code that owns the structure agrees (see above). New host code that reads a
structure field names it (`#define FIELD 0x..` with a rule in `config/layout_structs.json`) or, when it moved, reads it
through a `GLAYOUT_` descriptor (`config/layout_descriptors.json`); `py -3 -m unittest tests.test_layout` fails otherwise.

## What runs

| Version | Runs | Why |
|---|---|---|
| `en-116` | yes | the lifted engine's own executable |
| `ru-116` | yes | the same executable; the Russian text and font are data |
| `es-117` | yes | its own lift in the one program; addresses, ABI and every host structure offset proven (the box-box and bAddChild fixes are applied; hand/jump is native) |
| `ptbr-117` | yes | the same; it is the official 1.1 code, so no patch group is applied |

A version is playable when its `unplayable` text in `src/common/jpt_game_version.c` is empty; `tests/test_profiles.py`
allows that only when the map has nothing unresolved or unproven, every required override is resolved, the ABI check is
clean, no host offset is unproven and the host headers exist.

## Saves

`en-116` keeps `user_data\saves` (the folder earlier versions used). Every other version has
`user_data\versions\<id>\saves`: the Russian levels differ from the retail ones (`be.grf`, `lab-130.spz`), and the
117 builds are other executables with other level data, so a save is never assumed to load in another version.
