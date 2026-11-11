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
.\tools\build.ps1           # build what is out of date
.\tools\build.ps1 -Run      # the same, then start the game
```

Point it at your disc image the first time, by `-Iso "E:\path\to\Trespasser_EN.iso"`, the
`TRESPASSER_ISO` variable, or `config\local.json` (see `config\local.example.json`). The image
is only read; `tools\extract_game.py` verifies the executable's SHA-256 and refuses a disc that
carries a different build.

### What it does, in order

| Stage | Output | Runs again when |
|---|---|---|
| pcrecomp | `external\pcrecomp` | the checkout is missing or not at the commit in `config\target.json` |
| Game files | `game\` | `trespass.exe` is missing or has another SHA-256, or a DLL or `data\menu\tpassintro.smk` is missing |
| Analysis (about 15 min) | `analysis\` | the executable's SHA-256 or the pcrecomp commit changed, an output is missing, or `-Reanalyze` |
| Lift (about 1 min) | `src\recomp\gen\` | the catalog, the RTTI seeds, `config\decoder_fixups.json`, `config\extra_entries.json` or `tools\run_lift.py` changed (by content), an output is missing, or `-Relift` |
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
| `-Clean` | delete this configuration's build folder first; `game\`, the image, `analysis\`, `src\recomp\gen\` and `logs\` are kept |
| `-Config Release` | build `build\cmake\msvc-x86-release` instead of RelWithDebInfo |
| `-GenOpt /Od` | optimisation of the lifted C; `/Od` compiles several times faster and runs slower. Default: what the build folder already uses, else `/O2` |
| `-Trace` | build with `RECOMP_TRACE` (`--calltrace`, `--firsthit`); default: what the build folder already uses |
| `-BuildDir`, `-Jobs N` | another build folder (`-Clean` only removes folders under `build\`); parallel compile jobs |
| `-Iso PATH` | disc image, when the game files must be extracted |
| `-Reanalyze`, `-Relift` | force those stages |
| trailing arguments | passed to `trespasser.exe`, for example `-Run --watchdog 120` |

Equivalent manual build, from an "x64_x86 Cross Tools" developer prompt:

```bat
cmake -S . -B build\cmake\msvc-x86 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=cl
cmake --build build\cmake\msvc-x86
```

The stages can also be run by hand: `py -3 tools\fetch_pcrecomp.py`,
`tools\extract_game.py --iso ...`, `tools\analyze.py`, `tools\run_lift.py`.

## Running

`.\tools\build.ps1 -Run` builds, then starts `trespasser.exe --run` and stays attached until the
game exits. It does not start a second copy if `trespasser.exe` from that build folder is
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
.\build\cmake\msvc-x86\trespasser.exe

# Boot attempt: enters the game's entry point and opens the game window. The desktop mode is never changed.
.\build\cmake\msvc-x86\trespasser.exe --run
```

| Option | Effect |
|---|---|
| `--run` | execute the game; without it nothing runs |
| `--game DIR` | game directory (default `game\`, or `TRESPASSER_GAME_DIR`) |
| `--log-file FILE` | append to FILE (what `build.ps1 -Run` passes) |
| `--log-dir DIR` | without `--log-file`, create the next `trespasser_NNN.log` in DIR (default `build\logs\`) |
| `--ddraw system` | call the system `ddraw.dll` instead of the compatibility layer, observing it (diagnostics only) |
| `--watchdog N` | stop after N seconds and report where execution was |
| `--native-trace` | one log line per native call (large) |
| `--callbacks` | one log line per Windows-to-guest call |
| `-- ARGS` | arguments passed to the game itself |

Exit codes of the host: 1 setup failed, 2 reached a function that is not lifted, 3 fault,
4 watchdog, 5 jump to an address with no lifted entry. Otherwise the game's own code.

Settings the game would keep in the registry are in `game\recomp-registry.txt`. Deleting the
file resets them.

## When the run stops

| Log says | Do |
|---|---|
| `indirect transfer to 0x........, inside .text but not a lifted entry` | add the address to `config\extra_entries.json`, then run `.\tools\build.ps1` (it relifts and rebuilds) |
| `reached sub_........, which is not lifted` | `.\tools\build.ps1 -Relift` |
| `MISMATCH` under `target` | `game\trespass.exe` is not the binary the C was lifted from; delete it and run `.\tools\build.ps1` to extract again |
| `[bind] ... unresolved` | a DLL or export is missing on this machine; the line names it |
| `=== fault ... ===` | a real defect; the report below it is the starting point |

After changing only `src\host\`, `.\tools\build.ps1` recompiles just that.
