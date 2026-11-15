# Building and running

## Requirements

- Windows 10/11 x64
- Visual Studio 2022 or later with "Desktop development with C++" (MSVC x86/x64 tools, the
  Windows SDK, and the bundled CMake and Ninja)
- Python 3.10+ through the `py` launcher, with `capstone` and `pefile`
  (`py -3 -m pip install capstone pefile`)
- Git
- Your own English *Trespasser* disc image

About 1.5 GB of disk: 565 MB of game data, 80 MB of generated C, the rest build output.

The host is a 32-bit process on pcrecomp's `native32` runtime, so the only toolchain is the
x64-hosted x86 MSVC. `tools\build.ps1` finds the newest Visual Studio with those tools and
switches to it for the session.

## Build

```powershell
.\tools\build.ps1 -Run      # normal command: build what is out of date, then start the launcher and the game
.\tools\build.ps1           # build only
```

Neither needs another option: the configuration is MSVC x86, RelWithDebInfo, with the lifted C compiled at `/Od`
for fast incremental builds. `.\tools\run.ps1` is an optional shortcut that starts the existing
`build\cmake\msvc-x86\trespasser.exe` without building or checking anything; its arguments go to the game unchanged.

Point it at your disc image the first time, by `-Iso "E:\path\to\Trespasser_EN.iso"`, the
`TRESPASSER_ISO` variable, or `config\local.json` (see `config\local.example.json`). The image
is only read; `tools\extract_game.py` verifies the executable's SHA-256 and refuses a disc that
carries a different build. (Players install from the launcher's Play page instead; the build script
needs the files earlier, for the analysis.)

### What it does, in order

| Stage | Output | Runs again when |
|---|---|---|
| pcrecomp | `external\pcrecomp` | the checkout is missing, not at the commit in `config\target.json`, or `patches\pcrecomp` (applied in `series` order, recorded in `external\pcrecomp.patches`) changed |
| Game files | `game\` | `trespass.exe` is missing or has another SHA-256, or a DLL or `data\menu\tpassintro.smk` is missing |
| Analysis (about 15 min) | `analysis\` | the executable's SHA-256 or the pcrecomp commit changed, an output is missing, or `-Reanalyze` |
| Lift (about 1 min) | `src\recomp\gen\` | the catalog, the RTTI seeds, `config\decoder_fixups.json`, `config\extra_entries.json` or `tools\run_lift.py` changed (by content), an output is missing, or `-Relift` |
| Launcher sources | `external\imgui`, `external\trg-launcher`, `external\SDL` | missing, at another commit than `config\launcher_deps.txt`, or `launcher\patches\` changed |
| CMake configure | `build\cmake\msvc-x86\` | the cache is missing, or the configuration, `-GenOpt`, `-Trace` or the source folder changed |
| Build | `build\cmake\msvc-x86\trespasser.exe` | always asked of Ninja, which compiles only what changed |

The analysis and the lift record a digest of their inputs in `analysis\stamps\`. Output made by
an earlier manual run is adopted, not redone, when its recorded fingerprint matches. Ninja
reruns CMake by itself when `CMakeLists.txt` changes.

Nothing is cleaned or rebuilt in full unless asked. After `analyze.py` or `run_lift.py` change
the generated C, every generated file is recompiled, as it must be.

Each run of the script writes exactly one log, `build\logs\trespasser_NNN.log`, numbered one past
the highest already there. It holds the configuration, every stage, CMake and Ninja output, and
with `-Run` the game's own output and its exit code. The terminal shows the stage lines, Ninja
progress every 20 files, warnings and errors; a failure also prints the last 40 lines of the log
and the script exits with Ninja's exit code. The game is never started when a stage fails. Logs
from before this layout stay in `logs\`.

### Options

| Option | Effect |
|---|---|
| `-Run` | after a successful build, start `trespasser.exe --run` and wait for it |
| `-VerboseRuntime` | with `-Run`, show the runtime output in the terminal too |
| `-Arch x86` / `-Arch x64` | `x86` (default) is the playable 32-bit host in `build\cmake\msvc-x86`. `x64` builds only the 64-bit host groundwork (`trespasser_x64_probe`, `x64_tests`) in `build\cmake\msvc-x64` with the native x64 tools; it cannot run the game, so `-Run` is refused (`docs\X64_MIGRATION.md`) |
| `-Clean` | delete this configuration's build folder first; `game\`, the image, `analysis\`, `src\recomp\gen\` and `logs\` are kept |
| `-Config Release` | build `build\cmake\msvc-x86-release` instead of RelWithDebInfo |
| `-GenOpt /O2` | advanced: optimisation of the lifted C (compiler flags only, it never regenerates the C, but changing it recompiles all of it). Default: what the build folder already uses, else `/Od` for RelWithDebInfo and `/O2` for Release. Every lifted file runs during gameplay, so with `/Od` each is still compiled with `/O2 /Ob1`; `/Ob1` is added to any other value |
| `-Trace` | build with `RECOMP_TRACE` (`--calltrace`, `--firsthit`); default: what the build folder already uses |
| `-BuildDir`, `-Jobs N` | another build folder (`-Clean` only removes folders under `build\`); parallel compile jobs |
| `-Iso PATH` | disc image, when the game files must be extracted |
| `-Reanalyze`, `-Relift` | force those stages |
| `-GameArgs a,b,...` | with `-Run`, passed to `trespasser.exe` in order after `--run --log-file`, for example `-GameArgs "--renderer","d3d11"` |

Equivalent manual build, from an "x64_x86 Cross Tools" developer prompt:

```bat
cmake -S . -B build\cmake\msvc-x86 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=cl
cmake --build build\cmake\msvc-x86
```

The stages can also be run by hand: `py -3 tools\fetch_pcrecomp.py`,
`tools\extract_game.py --iso ...`, `tools\analyze.py`, `tools\run_lift.py`.

## Running

`.\tools\build.ps1 -Run` is the development loop: it builds, then starts `trespasser.exe` and stays attached until the
game exits. `.\tools\run.ps1` starts the current build without the build step. Neither starts a second copy if `trespasser.exe` from that build folder is
already running, and it cannot rebuild while one is.

The script closes its log, starts the game with `--log-file` naming that same file, and reopens
the file for appending once the game has exited, so only one process writes at a time. By default
the terminal stays concise and the runtime output is in the log only. On exit the script records
the exit code in the log and prints it with the log's path; when the exit is not 0 it names the
meaning of the host's code and prints the log's last 30 lines. The script's own exit code is the
game's. `--log-dir` or `--log-file` among the trailing arguments are ignored, with a warning.

Without the script, from the repository root:

```powershell
# Dry run: fingerprint, map, bind imports, then stop. Executes no game code, opens no window.
.\build\cmake\msvc-x86\trespasser.exe --dry-run

# The launcher (docs/LAUNCHER.md), then the game. The desktop mode is never changed.
.\build\cmake\msvc-x86\trespasser.exe
```

Settings come from `user\trespasser.ini`, which the launcher writes; the options below override
them for one run. `--run` is still accepted and changes nothing: starting without `--dry-run` runs
the game.

| Option | Effect |
|---|---|
| `--run` | accepted for older scripts; the game runs unless `--dry-run` is given |
| `--dry-run` | fingerprint, map and bind, then stop |
| `--launcher` / `--skip-launcher` | show the launcher even when it is turned off / skip it once (it still opens when the game files are not ready); holding Shift while starting also shows it |
| `--game DIR` | game directory (default `game\`, or `TRESPASSER_GAME_DIR`) |
| `--user_data_root DIR` | user data root: `launcher\trespasser.ini`, `saves\` (the game's registry store and saves) (default `user_data\` next to the program, or `TRESPASSER_USER_DATA_ROOT`; `build.ps1 -Run` passes `build\user_data`; see `docs\user-data.md`) |
| `--log-file FILE` | append to FILE (what `build.ps1 -Run` passes) |
| `--log-dir DIR` | without `--log-file`, create the next `trespasser_NNN.log` in DIR (default `build\logs\`) |
| `--ddraw system` | call the system `ddraw.dll` instead of the compatibility layer, observing it (diagnostics only) |
| `--ddraw-cert-dialog` | show the game's legacy "DirectDraw NOT certified" dialog, which the host skips by default |
| `--original-engine-limits` | d3d11: keep the 1998 draw distance, texture sizes and texture memory (comparison runs) |
| `--debug-d3d-output texture\|vertex-color\|depth` | d3d11 diagnostics: draws show only their texture, vertex colour or depth |
| `--legacy-safemode-dialog` | keep the game's "unstable last time, Safe Mode?" prompt and its registry `Safemode` flag (off by default) |
| `--renderer software\|d3d11` | `d3d11` (default): the game's Direct3D 6 path on the GPU; `software`: its SelfMod rasteriser |
| `--render-resolution original\|720p\|1080p\|1440p\|4k\|native\|WxH` | d3d11 render resolution (default `native`; `auto` means `original`, the compatibility size) |
| `--aspect-ratio 4:3\|16:9\|16:10\|21:9\|32:9` | gameplay shape, Hor+ (default 4:3) |
| `--texture-filter original\|bilinear\|anisotropic4\|anisotropic8\|anisotropic16` | d3d11 texture filtering (default anisotropic16; `point`, `trilinear` for diagnostics) |
| `--msaa 0\|2\|4\|8` | d3d11 multisampling (default 4; reduced to what the GPU supports, and logged) |
| `--vsync on\|off`, `--show-fps on\|off` | VSync (default on), FPS in the title (default on) |
| `--display-mode windowed\|borderless`, `--monitor N`, `--window-size auto\|WxH`, `--presentation letterbox\|stretch` | window settings (defaults windowed, 1, auto, letterbox) |
| `--watchdog N` | stop after N seconds and report where execution was |
| `--perf-log` | performance lines every 5 s, renderer summaries every 60 s and a sampled guest CPU profile (diagnostics; off by default) |
| `--lifted-water` | run the lifted water, occlusion and polygon-submission functions instead of their host kernels (comparison) |
| `--native-trace` | one log line per native call (large) |
| `--callbacks` | one log line per Windows-to-guest call |
| `-- ARGS` | arguments passed to the game itself |

Exit codes of the host: 0 normal, 1 setup failed before the game started, 2 reached a function
that is not lifted, 3 fault, 4 watchdog, 5 jump to an address with no lifted entry, 6 the game
reported that it did not finish starting up, 7 the game exited with an error code of its own, 8 the
game did not leave its loops within 60 s of a close request. The
game's own `1` (WinMain's success value) is reported as 0; see "Exit codes" in `ARCHITECTURE.md`.

## Headless tests

Small programs check the host without a window, a GPU device, the game or audio; they are not
part of the default build:

```powershell
ninja -C build\cmake\msvc-x86 launcher_tests host_tests gamepad_tests com_tests water_tests x87_tests override_tests_incremental override_tests_non_incremental
.\build\cmake\msvc-x86\override_tests_incremental.exe      # host overrides through stable entries: direct, alias and dispatched calls, refusals, counts
.\build\cmake\msvc-x86\override_tests_non_incremental.exe  # the same fixture linked /INCREMENTAL:NO, as Release links
py -3 -m unittest discover -s tests -p "test_stable_entries.py"  # the lift's stable-entry renaming and check
py -3 -m unittest discover -s tests -p "test_lift_staging.py"    # staging, publishing changed files only, cleanup retries when a scanner holds a file (TRESPASSER_TEST_LIFT=1 also lifts a small closure twice)
py -3 -m unittest discover -s tests -p "test_renderer_fixed.py"  # no runtime renderer switch: the renderer is set once at start, F5 is only the Video Test
.\build\cmake\msvc-x86\launcher_tests.exe  # settings and their migration, render sizes, Hor+, presentation, monitors, MSAA, ISO reader, disc identity, installer staging/rollback/Cancel, Play gate, pages and icons, launcher assets
$env:TRESPASSER_TEST_ISO = 'D:\images\Trespasser_EN.iso'; .\build\cmake\msvc-x86\launcher_tests.exe  # also identifies a real disc, read only
.\build\cmake\msvc-x86\host_tests.exe   # FPS counter, input/capture state machine, hotkeys, shutdown phases and quit steps, Safe Mode decisions, readback classes, render-target locations (GPU/CPU picture, captures, composites, the hybrid frame), call detection, texture formats, D3D6 -> D3D11 translation, shader compilation
.\build\cmake\msvc-x86\gamepad_tests.exe  # last device used, deadzones and drift, controller families, glyph styles, bindings and prompts (keys and gamepad), hotplug, gameplay input for CInput, menu focus
.\build\cmake\msvc-x86\com_tests.exe    # DirectDraw/Direct3D 6 COM objects through their vtables, against stubs
.\build\cmake\msvc-x86\water_tests.exe  # host kernels (water, occlusion, polygon submission) against the lifted functions, byte for byte; bump scanlines against the game's own routine (needs game\trespass.exe)
.\build\cmake\msvc-x86\x87_tests.exe    # the patched runtime: x87 ring, inline rounding against the C library, per-site call cache
ninja -C build\cmake\msvc-x86 user_data_tests
.\build\cmake\msvc-x86\user_data_tests.exe  # the portable user_data root, --user_data_root, an unwritable root (an error, no fallback), migration of the old user folder
ninja -C build\cmake\msvc-x86 l10n_tests
.\build\cmake\msvc-x86\l10n_tests.exe         # the language choice, `language` and the migration of the old settings, GameStrings, code pages 1252/1251, captions, DDF labels, the overlay's word wrap, and every language's coverage: GameStrings, captions and interface keys complete, placeholders and tokens kept, no English left, code page and Arial glyphs (--inventory lists the game's menu files)
ninja -C build\cmake\msvc-x86 stream_tests
.\build\cmake\msvc-x86\stream_tests.exe    # the embedded restored audio (manifest, sizes, SHA-256, durations), the virtual Stream.tpa over a made-up file and the player's own, the captions' timings in every language, and the lint that keeps development paths out of the project (the retail checks skip themselves without game\data\Stream.tpa)
ninja -C build\cmake\msvc-x86 stream_fix_pack
.\build\cmake\msvc-x86\stream_fix_pack.exe report|generate <retail Stream.tpa> <English Stream TPA fix> assets   # developer tool: the delta report, and assets\restored_audio and en.subtitles.txt (never run by the build)
ninja -C build\cmake\msvc-x86 translation_pack
.\build\cmake\msvc-x86\translation_pack.exe 'D:\downloads\translations' assets\translations  # developer tool: makes assets\translations from the extracted TresCom downloads (never run by the build)
.\tools\test-release-check.ps1              # tools\verify-release.ps1 refuses user data, downloaded archives and game files in a release
```

The x64 groundwork has its own, from an x64 developer prompt (or after `build.ps1 -Arch x64`):

```powershell
ninja -C build\cmake\msvc-x64 x64_tests
.\build\cmake\msvc-x64\x64_tests.exe     # guest address space, pointer marshalling, leak detection, guest COM handles
```

Settings the game would keep in the registry are in `user_data\saves\recomp-registry.txt` (copied once from
`game\` when an older layout had it there). Deleting the file resets them. The port's own settings
are in `user_data\launcher\trespasser.ini`. Development runs (`build.ps1 -Run`, `run.ps1`) use `build\user_data`.

## When the run stops

| Log says | Do |
|---|---|
| `indirect transfer to 0x........, inside .text but not a lifted entry` | add the address to `config\extra_entries.json`, then run `.\tools\build.ps1` (it relifts and rebuilds) |
| `reached sub_........, which is not lifted` | `.\tools\build.ps1 -Relift` |
| `MISMATCH` under `target` | `game\trespass.exe` is not the binary the C was lifted from; delete it and run `.\tools\build.ps1` to extract again |
| `[bind] ... unresolved` | a DLL or export is missing on this machine; the line names it |
| `=== fault ... ===` | a real defect; the report below it is the starting point |

After changing only `src\host\`, `.\tools\build.ps1` recompiles just that.
