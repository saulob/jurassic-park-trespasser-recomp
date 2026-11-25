# Linux x86_64 native host

Linux feature parity with the Windows build (launcher, portable folders, application identity, AppImage contract, settings) is the chapter
"Linux feature parity (JPT-LNX4K)" below and the inventory [LINUX_PARITY.md](LINUX_PARITY.md); the rest of this file is the bring-up record.

Status: **bring-up. The lifted guest runs on Linux x86_64, headless, through the CRT and C++ start-up, the window, DirectDraw and into
the game's own loop: against a virtual window it draws the original main menu (measured: 4 161 distinct colours at frame 400 against 1
at frame 5).** The SDL3 window backend (`--run`) opened the window and the main menu on WSLg/X11 in a first manual run; New Game then stopped at
`DrawTextExA`. After the text, file, audio and `GetDC` work below, the headless scripted New Game (click NEW GAME, 700 frames) loads
Beach and renders it, with the DirectSound layer on SDL's dummy driver. With FFmpeg in the build the game's own Smacker videos play
(headless: `newgame.smk` and the intro decoded and drawn by the guest); without it `NoVideo` is answered 1 and the game skips them.
Every statement below is something a headless command measured, or is marked as not done.

Target: a native ELF 64-bit x86-64 program (no Wine, Proton, Winelib, PE loader or 32-bit runtime) for a guest that
stays a logical 32-bit Win32 program. Baseline: Ubuntu 20.04, glibc 2.31.

## Build and test

```bash
tools/build-linux.sh            # build/linux-work/bin/trespasser_recomp (a work product), the lifted guest, one log in build/logs
tools/build-linux.sh --tests    # also the headless suites
tools/test-linux.sh             # build, run the suites, check the executable, list what no suite proves
CC=gcc-13 tools/test-linux.sh --build-dir /tmp/jpt_build   # another compiler, another build folder
tools/test-linux.sh --native-build --cc gcc-13             # ~/.cache/jpt-recomp/<system>-<compiler>: the Linux file system, much faster in WSL
tools/test-linux.sh --sanitize --native-build              # the Linux-specific suites under ASan + UBSan (a separate tree)
```

From Windows nothing of this is typed: `.\tools\test.ps1` finds the WSL distribution (JptFocalBuild, Ubuntu 20.04 with glibc 2.31,
is preferred; a newer one is used only for development and the glibc baseline is then reported as not proven), converts the
repository path with `wslpath`, runs `tools/test-linux.sh --native-build --machine` and prints each row as it arrives, then compares
the result with the Windows half feature by feature (`tests/feature_contract.json`). Supported disc images found through
`TRESPASSER_ISO_DIR`, `config/local.json` or a development downloads folder are imported headlessly before the suites
(`--import-dir`); an existing version is never replaced.

Needs cmake 3.20+, ninja or make, gcc or clang, python3 and `external/pcrecomp` (`tools/fetch_pcrecomp.py`). The scripts
never start the game, a window, a sound device or the launcher (the binary itself only answers `--build-info` and `--self-check`, headless); suites run with SDL's dummy drivers. The build tree is
`build/linux-work` (ignored by git) unless `--build-dir` says otherwise; a Linux file system is much faster than `/mnt/c` in WSL.
`cmake/JptLinux.cmake` is included by the top-level `CMakeLists.txt` on Linux only. Debug info and `__FILE__` use
`-ffile-prefix-map`, so no machine path is left in the binary.

## Linux feature parity (JPT-LNX4K)

The Linux build is a native x86_64 program: **not Wine, not Proton**, no Winelib, no PE loader and no 32-bit runtime. It runs the
same lifted game, the same launcher sources and the same settings file as the Windows build. Steam Deck is a target of this same
Linux build; it has **not been validated** on hardware (that remains the next step; the AppImage is packaged by `tools/package-linux.sh`). The Windows / Linux
inventory of every feature, with its status, evidence and justification, is [LINUX_PARITY.md](LINUX_PARITY.md), written from
`tests/feature_contract.json` by `tools/linux_parity_report.py`.

### Requirements

* 64-bit x86 Linux with glibc 2.31 or newer (the release baseline is Ubuntu 20.04; `ldd --version` shows it).
* An X11 or Wayland desktop. SDL3 picks the window system; `SDL_VIDEODRIVER` overrides it.
* For the Vulkan renderer, a Vulkan 1.1+ driver (Mesa RADV or ANV, NVIDIA). The Software renderer is the functional fallback and needs none.
* `zenity` or `kdialog` for the native disc image picker on desktop Linux, and `xdg-open` for its folder buttons. When neither
  is installed, or under Gamescope (Steam Deck Gaming Mode), the launcher opens its built-in controller-friendly disc image
  browser (`launcher/disc_browser.cpp`, `launcher/file_browser.cpp`), which lists the program folder, Home, Downloads and
  mounted drives such as `/run/media` SD cards. `JPT_FILE_CHOOSER=builtin` or `native` overrides the choice.
* To build: `libssl-dev` (OpenSSL with its static libraries), or let `tools/build-linux.sh` build the pinned static OpenSSL once into
  `~/.cache/jpt-recomp/openssl` (`tools/openssl-bootstrap.sh`: no sudo, nothing installed, the tarball is verified). The Bug Report sends
  through OpenSSL linked statically into the executable, so
  the program stays self-contained; the update download uses `curl` at run time (as TRG Launcher does). A **Release** build refuses to configure
  without OpenSSL (`tools/build-linux.sh --config Release` checks first and says so), and `-DJPT_LINUX_REQUIRE_BUG_REPORT=ON` asks the same of any
  build. A development build without it still compiles, the page then reports the service as unreachable, the executable declares the transport
  `none`, its self-check capability `bug_report_https` fails and the Linux gate refuses to stage it: it never counts as the shipped feature.
  The update installs only inside an AppImage, replacing the AppImage file itself ([Updates](LAUNCHER.md#updates)).
* Your own Trespasser disc image; no game file is part of the program.

### The launcher

`trespasser_recomp` with no mode option opens the launcher (TRG Launcher over SDL3 and Dear ImGui, the sources `launcher/` of the
Windows build), then starts the game when **Play** is pressed. Every page and setting is the Windows one and is saved to
`user_data/launcher/trespasser.ini`; the runtime reads the same file (`config-settings`, `tests/test_setting_parity.py`).

| Option | Effect |
| --- | --- |
| `--launcher` | show the launcher even when it is turned off (Shift at start is not available on Linux) |
| `--skip-launcher` | skip it once, unless the game files are not ready |
| `--launcher-renderer auto\|software` | what draws the launcher's own window, for this run only; `software` needs no Vulkan (docs/LAUNCHER.md). The game's renderer does not change |
| `--run` | start the game at once, without the launcher |
| `--dry-run`, `--bootstrap-headless` | the headless modes of the earlier bring-up |
| `--renderer`, `--graphics-card` | override the renderer and the Vulkan card for this run |
| `--aspect-ratio`, `--msaa`, `--vsync`, `--display-mode`, `--monitor`, `--window-size`, `--presentation`, `--show-fps`, `--skip-intro`, `--video-subtitles`, `--ambient-occlusion` | override that setting for this run (the same spellings as on Windows) |

The launcher is usable with mouse, touch, trackpad and keyboard, and with a gamepad (SDL3 navigation: D-pad and left stick move, A
presses, B goes back). Its **Graphics card** list is the Vulkan device list the game uses (`jpt_vk_probe_headless`), with the sample
counts of each card. The Windows-only **Direct3D 11** renderer is not offered. All four supported releases run: English and
Russian use `lift116`, Spanish `lift_es117` and Brazilian Portuguese `lift_ptbr117`; the installed version selects one lift,
once, before guest code runs. The headless boot validates all four through their first frame; gameplay and the interactive
display, audio and input remain manual. The earlier published v0.1.0 AppImage predates this fix and ran English and Russian only;
v0.1.1 includes all four versions. **Install Game** copies the game files from the disc image into `game/` with the same
reader, checks and SHA-256 manifest as Windows (`tests/linux_launcher_tests.cpp`: PE string resources, Joliet UTF-16 names, part
files, checksums).

### Where things are

| What | Where |
| --- | --- |
| Game files | `game/` next to the program (`--game DIR`, `TRESPASSER_GAME_DIR`) |
| Logs | `logs/` next to the program (`--log-dir DIR`, `TRESPASSER_LOG_DIR`); `user_data/logs` when the one beside an AppImage cannot be written |
| Settings, saves, caches, pipeline cache | `user_data/` next to the program (`--user_data_root DIR`, `TRESPASSER_USER_DATA_ROOT`) |

"Next to the program" is the folder of the executable, or, when the `APPIMAGE` variable names a file, the folder of **that AppImage
file**, never its read-only mount. Nothing is ever written to `~/.config`, `~/.local/share`, the home folder or the source tree, and
nothing is chosen for the player when a folder cannot be written: the program says which one and stops. One implementation serves the
launcher, the runtime, Windows and Linux (`src/common/jpt_port.c`, `tests/port_tests.c`, `tests/linux_paths_tests.c`). The crash
report is in the log, which the launcher's **Logs folder** button opens.

### Graphics

* **Vulkan** is the GPU backend; **Software** is the functional fallback (menus, gameplay, video, subtitles, pause, hotkeys). VSync
  maps to the present mode: Off allows immediate presentation (tearing), On uses mailbox or FIFO. The 1280x800 (16:10) display of a
  handheld is a tested layout (`linux_present_tests`: the picture is centred with bars, a 16:10 picture fills it).
* **GPU choice.** The launcher's **Graphics card** (or `--graphics-card`) names a Vulkan device; until one is chosen the runtime
  prefers the dedicated GPU, and the usual selection variables `DRI_PRIME`, `MESA_VK_DEVICE_SELECT`, `VK_LOADER_DEVICE_SELECT` and
  `__NV_PRIME_RENDER_OFFLOAD` take precedence when set. The D3D-specific adapter option is Windows only.
* **When Vulkan cannot start** the program does not stop silently: the renderer policy falls back to Software for this run, an SDL
  message box (no external tool) and stderr say why, name the log, and state that the saved renderer setting was not changed. The
  launcher still opens and shows the Vulkan device list (empty when there is none).
* **Window.** `Monitor`, `Display mode` (borderless starts fullscreen on that monitor) and `Window size` reach the SDL window; F11
  switches fullscreen. The SDL window carries the port icon and the application ID below.

### Application identity

The Wayland `app_id`, the X11 window class (`WM_CLASS`) and the `.desktop` file are one name, **`io.github.saulob.jurassic-park-trespasser`**
(`JPT_APPLICATION_ID`, `src/common/jpt_port.h`). SDL receives it as its application identifier before any window exists
(`jpt_app_identity_apply`), and the icon is embedded in the executable (`packaging/icons/app-icon-256.png`), so neither depends on the
AppImage's desktop integration. `packaging/linux/` holds the `AppRun`, the `.desktop` file and the AppStream metadata the package uses.

### The AppImage

The Linux release is one native x86_64 file, `dist/JurassicParkTrespasserRecompiled-v<version>-x86_64.AppImage`, with
`dist/JurassicParkTrespasserRecompiled-v<version>-x86_64.AppImage.sha256` beside it. The version is `project(... VERSION ...)` in
`CMakeLists.txt`; the asset prefix and the Linux suffix are `config/release.json`, and the Linux program is
`trespasser_recomp`. The packaging script reads them and writes neither.

From Windows one command prepares the complete release (Windows ZIP and Linux AppImage, each with its `.sha256`):

```powershell
.\tools\package-release.ps1
```

`-SkipBuild` packages the existing builds of both platforms; `-Clean` rebuilds both; `-WslDistro` names another WSL distribution. Nothing
else is typed: no `ci-deps.sh`, no WSL path, no copying and no hashing. `package-release.ps1`

* removes stale artifacts of the asset prefix from `dist\`, runs `tools\package.ps1` (Release x86 build, ZIP, `.zip.sha256`) and checks the ZIP;
* creates the WSL2 distribution **`JptFocalBuild`** when it is missing (the Ubuntu 20.04.5 base image, SHA-256
  `60e216b60947653dc8989be3821380268315b99b2959c29882781541bfe5a426`, `wsl --import` into `%LOCALAPPDATA%\WSL\JptFocalBuild`) and, when a build
  tool is missing, runs `packaging/linux/ci-deps.sh` in it as root (`tools\release-inputs.ps1`). Only `JptFocalBuild` is ever created or changed;
  a distribution named with `-WslDistro` is used as it is, and the sibling ports' distributions are never touched;
* takes the reviewed AppImage runtime from the ignored `cache\appimage-runtime\runtime-x86_64` when it has the SHA-256 pinned in
  `patches/appimage-runtime/baseline.env` (any other file is deleted), otherwise copies it from a WSL packaging cache that holds the same file
  (`tools/import-appimage-runtime.sh` in `JptFocalBuild`, then a read-only look into the sibling ports' distributions), and hands it to
  the build as `JPT_APPIMAGE_RUNTIME`;
* runs `tools/package-release-linux.sh` in WSL, which mirrors the sources (not `.git`, `game`, `user`, `user_data`, `cache`, `logs`, `saves`,
  `build`, `dist`, `out`, `.venv`) to `${JPT_WSL_BUILD_DIR:-~/.cache/jurassic-park-trespasser-recomp-build}`, calls `tools/package-linux.sh`
  there, starts the AppImage headless in every start mode (`packaging/linux/smoke-test.sh`: `--build-info` through the extracted `AppRun`,
  `--appimage-extract-and-run` and the file itself) and copies the AppImage, its `.sha256` and the debug symbols to `dist\`;
* prints Windows OK / Linux OK, or the problems; nothing is published, tagged or uploaded.

The Windows ZIP is `JurassicParkTrespasserRecompiled-v<version>-windows-x86.zip` (`trespasser_recomp.exe` is a 32-bit PE: the host is the
32-bit process the lifted guest needs, so the suffix stays `x86`). The Windows Release link writes no PDB; one that exists would go to
`dist\<name>-symbols\`, never into the ZIP. On a Linux checkout run `tools/package-linux.sh` directly (on Ubuntu 20.04 after
`sudo bash packaging/linux/ci-deps.sh`).

Creating the distribution by hand is the same two commands:

```powershell
wsl --import JptFocalBuild <folder> ubuntu-base-20.04.5-base-amd64.tar.gz --version 2
wsl -d JptFocalBuild -u root --exec /bin/bash packaging/linux/ci-deps.sh
```

`tools/package-linux.sh` builds the Linux **Release** gate into `build/linux-release`, stages the canonical artifact
`build/linux/bin/trespasser_recomp` with its manifest, verifies the manifest, the source fingerprint, the feature contract and the
headless self-check, then creates a clean AppDir under `build/appimage/AppDir`. `--skip-build` (and `package-release.ps1 -SkipBuild`)
uses the existing canonical artifact and refuses a stale one. An ELF from `build/linux-work` or any other path is never packaged.

The AppDir follows the other ports:

```
AppDir/
  AppRun
  io.github.saulob.jurassic-park-trespasser.desktop
  io.github.saulob.jurassic-park-trespasser.png
  .DirIcon
  usr/
    bin/trespasser_recomp
    bin/assets/launcher/{header-art,sponsor-heart}.png
    lib/                         (empty: every library below is static)
    share/applications/io.github.saulob.jurassic-park-trespasser.desktop
    share/metainfo/io.github.saulob.jurassic-park-trespasser.appdata.xml
    share/icons/hicolor/256x256/apps/io.github.saulob.jurassic-park-trespasser.png
    share/licenses/JurassicParkTrespasserRecompiled/
    share/doc/JurassicParkTrespasserRecompiled/README.txt
```

SDL3, FFmpeg, OpenSSL, libstdc++ and libgcc are linked statically, so no shared copies are staged. The script still bundles any
unexpected dynamic dependency and refuses libstdc++, libgcc, OpenSSL, FFmpeg, SDL3, the Vulkan loader, Mesa, the display servers and
the audio servers. Every ELF in the AppDir is checked for `DT_NEEDED`, `RPATH`/`RUNPATH` and the newest `GLIBC_` import; the executable
carries `DT_RPATH $ORIGIN/../lib` (not `DT_RUNPATH`). The baseline is Ubuntu 20.04, glibc 2.31.

The AppImage runtime is the upstream AppImage Type 2 runtime at the commit pinned in `patches/appimage-runtime/baseline.env`, with
`patches/appimage-runtime/0001-extract-and-run-when-fuse-fails.patch`; `tools/build-appimage-runtime.sh` builds it in an Alpine chroot
and accepts it only with the pinned SHA-256; `JPT_APPIMAGE_RUNTIME=FILE` reuses an already reviewed build of it (checked by SHA-256) instead of rebuilding. When FUSE cannot mount the image, the runtime extracts it and runs from the extracted copy
automatically; `APPIMAGE_EXTRACT_AND_RUN=1` and `--appimage-extract-and-run` force the same path, and signals are forwarded to the
child. The runtime keeps `APPIMAGE` pointing at the original file in both modes. The appimagetool version and SHA-256 are pinned in
`tools/package-linux.sh`.

The port folder follows `APPIMAGE`: `game/`, `logs/` and `user_data/` live beside the AppImage file, never inside the read-only mount.
The restart command starts the **original AppImage file** with the same arguments (`jpt_port_relaunch`, `lx_relaunch`) and never
`/proc/self/exe` of a mount or an extracted copy (`linux_paths_tests: test_appimage`, `test_appimage_logs`). Nothing is written into the
AppImage. The launcher's updater downloads the `.AppImage`, checks its `.sha256`, replaces the original `$APPIMAGE` through a temporary
file and a backup with rollback, and relaunches it with `--launcher`.

After writing the AppImage, the script extracts it again, repeats the AppDir audit on the result, verifies that the embedded runtime is
the pinned one and that the extracted tree equals the staged AppDir byte for byte, then writes the `.sha256`. `--verify DIR` checks an
existing AppDir; `--verify-only FILE` checks a finished AppImage. `JPT_RELEASE_DATE=YYYY-MM-DD` overrides the AppStream release date;
`SOURCE_DATE_EPOCH` is set from the last commit when possible. The same source, version and tools produce the same tree; the AppImage
file's SHA-256 can still change if appimagetool or the pinned runtime include their own metadata.

### Steam Deck

Steam Deck support is **experimental** (preparation is complete; it has not been validated on Steam Deck hardware — status:
**not validated**). There is no
separate Steam Deck executable, no Steamworks/Steam Input SDK dependency and no Wine or Proton layer: Steam Deck runs the exact
same native Linux x86_64 AppImage (built by `tools/package-linux.sh`).

* **Display & Graphics.** Targets the 1280x800 (16:10) display on Vulkan (Mesa RADV). A 4:3 picture is centred with equal side
  bars and matching mouse mapping; a 16:10 setting fills the screen. Under **Gamescope** (Gaming Mode, detected from
  `GAMESCOPE_WAYLAND_DISPLAY`), the Vulkan swapchain keeps the surface's minimum image count (`jpt_vk_choose_image_count_for`)
  instead of queueing an extra frame, and tolerates persistent `VK_SUBOPTIMAL_KHR` without recreation loops.
* **Controller, trackpad & touchscreen.** SDL3 Gamepad drives both the game (with Steam Deck glyphs in Automatic mode) and the
  launcher (D-pad/left stick, A to select, B to cancel/back). In Gaming Mode (or when `zenity`/`kdialog` are absent), **Import
  game...** opens the launcher's built-in controller-friendly file browser, which lists the program folder, Downloads, Home and
  mounted drives (`/run/media` SD cards). Because Trespasser uses a real mouse pointer in menus and relative mouse motion in
  gameplay, Steam Deck trackpad and touchscreen inputs work through SDL3's mouse path without a separate Steam Input layer.
* **Gaming Mode setup (Non-Steam Game).** Place the AppImage in a writable folder (internal storage or SD card; nothing hardcodes
  `/home/deck`), mark it executable (`chmod +x`), add it via **Games -> Add a Non-Steam Game to My Library**, and run it. To
  reopen the launcher after disabling **Show this launcher**, add `--launcher` to the shortcut's **Launch Options** (then remove
  it to start directly into the game).
* **Portable paths & updates.** `game/`, `user_data/` (settings, saves, caches, Vulkan pipeline cache) and `logs/` always live
  beside the original `$APPIMAGE` file, never inside the read-only `/tmp/.mount_*` mount. Self-updates verify SHA-256, replace
  the original `$APPIMAGE` with rollback on failure, and relaunch it with `--launcher`.

### Tests and commands

```bash
tools/test-linux-parity.sh                 # the parity checks of this chapter, headless, no build needed beyond the suites
tools/test-linux.sh                        # the whole headless Linux gate
python3 -I tools/linux_parity_report.py --write   # rewrite docs/LINUX_PARITY.md after editing tests/feature_contract.json
python3 tools/fetch_launcher_deps.py       # SDL3, Dear ImGui and TRG Launcher at their pinned commits (build-linux.sh runs it)
```

### Known limits

* No run on real hardware yet: the launcher window, controller feel, Wayland, audio devices and Steam Deck are unconfirmed.
* Shift at start does not bring the launcher back; use `--launcher`.
* Spanish and Brazilian Portuguese run through their own lifts and pass the headless first-frame boot; interactive gameplay,
  display, audio and input are still pending manual validation.
* The native desktop picker uses `zenity` or `kdialog` (otherwise the built-in browser opens); the folder buttons need `xdg-open`.

## The one Linux artifact (JPT-ART4N)

There is one Linux binary to run, test by hand, package and ship: `build/linux/bin/trespasser_recomp`, with
`trespasser_recomp.build.json` beside it. It is written only by `tools/test.ps1` (or `tools/test-linux.sh --stage`), and only after
the Windows gate, the Linux build, every Linux suite, the feature parity and the binary's own checks passed. `tools/build-linux.sh`
and `tools/test-linux.sh` build in `build/linux-work` (or the cache), so a binary you build by hand can no longer land on the
artifact path and pass for the validated one. The cache under `~/.cache/jpt-recomp` keeps feeding objects, CMake, Ninja and the
pinned FFmpeg; the gate copies the finished binary out of it.

The first manual runs of the Linux port used a stale binary: `build/linux/bin/trespasser_recomp` was an older build (system GPL
FFmpeg, no audio pacer, the old "overrides not installed" boot line) while the gate validated the one in the cache. Nothing tied the
two together.

* **Provenance.** `trespasser_recomp --build-info` prints (JSON, nothing else) the version, git commit and dirty flag, a source
  fingerprint, the compiler, target, configuration, glibc baseline, FFmpeg libraries and licence, and the hash and schema of
  `tests/feature_contract.json`. The fingerprint (`tools/source_fingerprint.py`) is the SHA-256 of a sorted manifest of file hashes
  of `CMakeLists.txt`, `cmake`, `src`, `config`, `assets`, the pcrecomp runtime, imgui and the build scripts; text files are hashed
  with line endings normalised, lifted code (`src/recomp/gen*`) by name and size (`tools/lifts.py` verifies its content), and
  `build`, `user_data`, `logs`, `game`, `.git` never take part. `tools/gen_build_info.py` writes the header during the build and
  rewrites it only when it changed. No clock goes into the binary.
* **Self-check.** `trespasser_recomp --self-check` opens no window, no audio device, no log and writes no file; no guest code runs.
  Each capability is a real call into its module: SelfMod entries, the override registry (71 listed, the number Linux can install, the
  number installed with default settings), the 27 lift hooks, the host features, the audio pacer (policy version, queue target,
  periods that follow the mixer rate) and SDL's audio stream, the input context (menus: free absolute pointer; gameplay: relative
  motion), the gamepad layer and its menu step, the HD front end and loading screen, the controls card, Russian text, FFmpeg Smacker,
  the virtual Stream.tpa, video subtitles and save pictures.
* **Gate rows.** `elf self-check`, `elf build provenance` (the binary's fingerprint and contract hash against the tree, its override
  and hook counts against `guest_override_list.h` and `lift_hook_list.h`) and `elf feature contract` (every Linux PASS feature that
  lists `"runtime": [...]` capabilities in `tests/feature_contract.json` needs them in the binary). A passing suite with a binary
  built from other sources, or without a feature, now fails.
* **Staging.** `tools/linux_artifact.py stage` checks, copies to `trespasser_recomp.new` and renames; the manifest records the SHA-256,
  fingerprint, contract hash, compiler, glibc maximum, FFmpeg mode, the gate's suite counts and its UTC time. A failing gate leaves
  the last validated binary alone. `tools/package-linux.sh --skip-build` verifies the artifact against its manifest and the current
  sources (`linux_artifact.py verify`), requires the Release configuration and packages it; it never builds. Without `--skip-build`,
  the script runs the Release gate first.
* **Boot log.** After the features are installed: `Linux features: 30/71 overrides installed (N required), 27/27 lift hooks active`
  (with the host features off it says so). 30 is what Linux can install with every option on; the number installed follows the
  settings (29 with the defaults).
* **Audio log.** One line after the device opens (`SDL3 pacer active (policy 1): source 22050 Hz ..., device N Hz, ..., queue target N
  periods`) and one at shutdown with the underruns, backlog drops, queue minimum and maximum and the longest wake-up gap; they
  separate our bugs from device jitter. `JPT_AUDIO_STATS=1` still logs while running.
* **Input log.** `JPT_INPUT_STATS=1` logs the mouse totals (SDL motions, absolute and relative motions that reached the guest,
  buttons, wheel, context switches) at each change of input context and at shutdown, never per event. The pointer path is the
  explicit input context of `host_policy.h`; `SetCursorPos` only stores the position (`tests/test_linux_artifact.py` reads it).

### WSLg

WSLg is a development and smoke environment. It is not the reference for audio latency or quality (PulseAudio through RDP), raw
relative mouse motion, gamepad hardware, fullscreen behaviour or Gamescope, and a problem seen only there is not a failure of the port
until it is reproduced on a native X11 or Wayland session. The code carries no WSL, `PULSE_SERVER` or X11 special case. The absolute
pointer of the menus stays covered by headless suites with fake SDL events.

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
* **Other lifts.** `lift116`, `lift_es117` and `lift_ptbr117` each carry their own SelfMod section, generated from the
  build's own executable with `tools/lift_selfmod.py` (the profile's SelfMod range). The 117 sections have 46 entries and
  44 patch sites like the baseline; their ranges are `0x0063D000-0x00646B75` and `0x0063F000-0x00648B75`.
* **Tests.** `tests/test_selfmod_lift.py`: the baseline range, 46 sorted entries equal to the executable's routine starts,
  every SelfMod target the lifted `.text` calls has a dispatch entry, every call out reaches lifted code, nothing is left
  unlifted, 44 patch sites are guest loads, no placeholder constants remain; the 117 lifts are checked for their own range,
  entries and patch sites.

## Guest DLLs (`tools/lift_module.py`, `src/linux/linux_modules.*`)

A guest DLL is lifted to C like the executable; the DLL is a development input and is never executed (no Wine, `dlopen`,
`LoadLibrary` or emulator). MSVCIRT.DLL is the first and `config/guest_modules.json` is the list; nothing about it is hard-coded in
the lifter or the runtime.

* **MSVCIRT, as supported.** `MSVCIRT.DLL` 6.0.8168.0, PE timestamp 1998-06-17 18:45:46 UTC, SHA-256
  `647f7534aaaedffa93534e4cb9b24bfcf91524828ff0364d88973be58139e255` (identical in every installed version), base `0x780A0000`,
  size `0x12000`, entry (DllMain) `0x780A1000`, sections `.text` (`0x71E2` bytes), `.rdata`, `.data`, `.rsrc`, `.reloc`. 445 exports,
  all named: 390 code, 55 data, 0 forwarders. 889 relocations. No TLS directory. Imports: MSVCRT 43, KERNEL32 5.
* **Lift.** Starts are the 390 code exports, DllMain, the 141 code addresses held in `.rdata`/`.data` and the 81 held as immediates in `.text` (both found through the relocation table:
  vtable entries, callbacks, the exit handlers `push offset f` passes to `_onexit`) and the closure of every direct call and jump target: 578 functions. A seed that lies
  inside another function's body is a label, not a function. Output: `recomp_msvcirt.c` (the functions, `msvcirt_dispatch_table`,
  `msvcirt_meta`) and `recomp_msvcirt.json` in the lift's folder, inside the `lift116` archive; symbols are `msvcirt_sub_<va>`, so they
  never collide with `lift116_*`. `tools/run_lift.py` runs it for every lift that lists the module; a full regeneration reproduced all
  77 files byte for byte. The manifest records the module (SHA-256, base, size, entry, text range, entry and export counts, export
  fingerprint) and `tools/lifts.py verify --content` checks it. The generated lift lives in `lift116`; the runtime maps it for
  every version, because the installed `MSVCIRT.DLL` is the same file in all four releases.
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

Maps the selected version's `guest.img` (SHA-256 checked against its lift's source) and the lifted MSVCIRT DLL, binds the imports of
both, attaches the DLL and calls the active lift's entry point on a guest thread (`0x00626BE2` for `lift116`). The host window is the
virtual backend: no window, no audio. The run stops, with a
status, when the guest presents its first frame (`BOOTSTRAP_FRAME_PRESENTED`, the default), after `frame_limit` frames, when it polls
`PeekMessageA` 200 000 times with nothing to deliver (`BOOTSTRAP_IDLE`), at the first import with no implementation, or when it exits.
Run by `tests/linux_boot_tests.c` (needs the installed game, else `DATA UNAVAILABLE`; it checks every installed version); log lines:
`Linux guest: entry ...`, `Linux HLE: resolved N/297`, `Linux USER32: registered class ...`, `Linux USER32: created window ...`,
`Linux GDI: framebuffer ...`, `Linux DDRAW: primary surface ...`, `Linux presentation: ...`.

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
  changes). Without a GPU renderer there is no Direct3D: `QueryInterface` for it is refused and the game keeps its software renderer. Pixels live in guest memory, so the
  guest's own rasteriser (SelfMod) writes straight into the surface the presenter shows.
* **Presenter** (`linux_screen.*`, `linux_window.*`): frames go to the backend unconverted (RGB565 stays RGB565; other formats become
  XRGB8888). `jpt_presentation` decides the view: letterboxed, a new guest screen size takes effect only with the first frame of that
  size (`tests/linux_present_tests.c`: 640x480, 800x600, 1280x720, 2560x1440), and the mouse maps back through the same view. Window
  size policy (Auto, fixed, fullscreen) of the Windows host is not ported; F11 and the host hotkeys are (below). ImGui is not
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
  the game's virtual paths and over a guest file handle. The HLE needs a decoder: FFmpeg's (`libavformat`, `libavcodec`, `libavutil`,
  nothing else, no `ffmpeg` program, no `swscale`/`swresample`) when the build finds it (`JPT_HAS_FFMPEG=1` in the configure output,
  `pkg-config` or `-DFFMPEG_ROOT=<prefix>`); without it nothing is installed and the registry answers `NoVideo = 1`. The log says which
  case it is, with the library versions and licences, and warns when FFmpeg is not LGPL-only (`-DJPT_FFMPEG_REQUIRE_LGPL=ON` refuses such
  a build at configure time). Packages: `sudo apt install libavformat-dev libavcodec-dev libavutil-dev`. The picture goes into the buffer
  the game passed to `SmackToBuffer` (palette indexes or RGB555/565, changed 4x4 blocks only, rectangles, pitch, clipping), the sound
  into the same SDL3 mixer as the game's audio (no second device), and `SmackWait` follows the audio's played position, the monotonic
  clock when there is none. The Linux log has one line per open and close (`SmackOpen: 640x348, 1585 frames ...`).
* **lz32** (`linux_lz32.*`): `LZOpenFileA`, `LZRead`, `LZSeek`, `LZClose` over a whole-file buffer; a file with the SZDD header is expanded
  when it is opened, any other is read as it is. The game reads its scenes and worlds this way.
* **Guest threads and the machine lock**: the lock is not fair, so a guest thread that only polls would keep the others from ever
  running. The import path hands the machine over (100 us, every 64th import) while another guest thread waits for it.

## Videos, Stream.tpa, saves, sessions and the gamepad (JPT-LNX13V)

Measured headless (virtual window, SDL dummy audio and video drivers, nothing shown or played), with the installed game:

* **Videos.** `linux_video_tests` plays the intro (`tpassintro.smk`, 640x348) and `newgame.smk` (640x344, Y-interlaced as in the
  retail files) through the guest's own `CVideoWnd::Play`: 120 frames of each decoded by FFmpeg, 1.5 MB of PCM pushed to the mixer, an
  Escape key-up skips each video, both are closed, and the game goes on to the menu and to Beach. `tests/smk_fixture.h` builds a
  synthetic Smacker file (own bitstream, no game video in the repository) that `smacker_ffmpeg_tests` decodes frame by frame, palette
  by palette, with two audio tracks, truncation and corruption sweeps and the HLE over the real backend. Not measured: the sound
  itself (dummy device), A/V sync by ear. The picture of `newgame.smk` keeps the retail scan lines.
* **Stream.tpa.** One virtual view per handle (`src/host/stream_vfile.*`, shared by the Windows host through `stream_audio.c`), over
  `stream_pack.c`: the game's directory handle and its pool of read-write handles each get their own position and lifetime; every
  one reads the corrected directory (4 restored records added, 1 replaced), the corrected captions and the restored records, also
  beyond the physical end of the file. A view is opened read-only whatever access the game asks for; a `WriteFile` on it is refused,
  so the retail file is never written (the Windows host passes such writes to the real file; the game was not seen to write).
  `linux_stream_tests` (341 checks) covers a synthetic TPA and the retail file: Beach samples VH116, VH117, VH10 and VH16 through
  16 handles (two packs of 8) byte-identical, the five restored records, 410 caption samples, the caption scan, the limit of 32 views
  and 500 open/close cycles that leave nothing. The log line is `Stream.tpa: 405 captions corrected, 4 restored samples added,
  1 replaced, 0 left as in the player's file` in the real game.
* **Saves.** The game saves with msvcrt `_open`/`_write`/`_close` (and uses `remove`, `rename`, `_mkdir`), implemented over the
  vpath. `linux_save_files_tests` covers create, overwrite, truncated files, names in any case, folders on the way, a read-only
  folder. `linux_session_tests` drives the real game: New Game, pause menu, Save as `JPTSAVE1` (`SAVEGAME.000`, 335 KB), walk, Load
  (a double click on the save: the OK button alone did nothing in the script), the view returns to the saved one; Quit to the main
  menu, a second New Game in the same process, 7615 frames of walking, strafing and jumping (resident memory +3 MB), and the game's
  own Quit to the desktop (`ExitProcess`, code 1). At that exit the game still holds its level swap file and its streaming thread,
  which the host closes; no Stream.tpa view, DirectDraw surface, audio buffer, Smack object or find handle is left. The save format
  is the game's own bytes; a Windows save was not loaded on Linux (no fixture: saves in `user_data` are personal).
* **Video subtitles.** The Windows host draws them in its own presenters (`video_subtitles.c`: the frame of the Smacker player decides
  the cue, GDI rasterises the text); the captions of `Stream.tpa` are the voice-over of the game and have nothing to do with them. On
  Linux nothing called that code, so the videos played bare. Now the Smacker HLE tells the host when a video opens (its file name, from
  the name or from the handle's `CreateFileA` name), which frame is on screen and where, and when it closes (`smk_host_t.video_*`);
  `linux_video_subtitles.*` selects the timeline with the same fingerprint (size and first 64 KB) as Windows, takes the cue from that
  frame, rasterises the text with `linux_text` (embedded Droid Sans, a Unicode entry added for UTF-8 text), wraps, places and outlines it
  with the shared `jpt_video_subtitles` (`jvs_outline` moved there from the Windows host) and blends it, in the video's rectangle, into
  a copy of the frame the presenter shows; the guest's surface and the video's pixels are not touched. A close, a skip or the HLE
  shutting down clears the cue at once. The text is `jpt_tr` of the cue's key in the language of `trespasser.ini` (`language`, else the
  release's own; the switch is `video_subtitles`, default On): all 7 languages translate all 128 cue texts of the 3 videos with dialogue
  (`tpassintro.smk` 16 cues, `newgame.smk` 9, `win.smk` 7; `credits.smk` has none). `linux_video_subtitle_tests` covers the timelines,
  languages, the overlay's pixels (place, centring, lines, clipping, other sizes), the retail files and the skip; `linux_video_tests`
  plays both videos through the guest and checks the cues, the text in the presented frame, and no cue left after the skip.
* **Mouse.** The Windows host never applies the game's `ClipCursor` or `SetCapture` (menus, dialogs and videos keep the pointer free) and
  gives relative mouse mode only to gameplay, which the game announces by re-centring its cursor every frame (`SetCursorPos`; its
  own `ClipCursor(NULL)` brackets the same time). Linux used to turn each `ClipCursor(rect)` into an SDL mouse grab, and the game
  sets one before the intro (log 007: confined pointer in the intro and the main menu) and never took it back, with no relative
  motion in the level. `linux_mouse.*` now reuses the Windows policy (`host_policy.c`: `input_state_from`, `input_hide_pointer`,
  `pointer_add_relative`, `pointer_recapture`): clip and capture are logical state of user32; the pointer is hidden while the game's
  `ShowCursor` counter is below zero and in gameplay; relative mode is on only while the game re-centres (three `SetCursorPos` calls
  within 250 ms) and the window has focus; a lost focus releases it, a regained one takes it again; relative motion moves the
  virtual cursor, absolute positions only move it outside gameplay. One log line per change (`Linux mouse: relative on, pointer
  hidden (INPUT_GAMEPLAY_CAPTURED)`). `linux_mouse_tests` follows the game's own call sequence (start-up, video, menu, level, pause,
  focus); `linux_video_tests` checks, against the real guest, that no grab or relative mode occurs during the videos, an Escape skip
  and the menu (it failed before the change: 1005 frames grabbed, 305 of them in the menu), with a focus loss during the intro and
  relative mode on in the level.
* **Gamepad.** `linux_gamepad.*` is the Windows policy layer (`gamepad_policy`, `jpt_gamepad`, `pad_sdl`) behind an SDL3 device
  backend: connection, hot-plug (a poll every 200 ms), the controller in use, the look as cursor motion, the actions' command bits
  and Start as Escape, added to the game's own CInput read (`0x005F80B0`, through the shared override table: `lx_features_install_cinput`, which also empties the result while a host menu is open). `linux_gamepad_tests`
  drives it with a fake backend (SDL's virtual joystick is not in this SDL build): all 15 buttons, both sticks and triggers, every
  default action, connect, remove, reconnect, two controllers, no key from LB/RB/D-pad. The game imports only `joyGetPosEx`: it
  has no force feedback, so nothing drives rumble; `lx_gamepad_rumble` exists and reaches the device. Menu navigation by pad, the
  shortcuts and the glyphs are the shared ones (see JPT-PAR9F below).
* **Host keys.** F11 (fullscreen) and Alt+F4 work in the SDL backend (`linux_sdl_window_tests`). F3 (Objectives), F4 (Show Objective), F5
  (minimap), F6 (frame counter), F7/F8 (Quick Save/Load), F9 (physics snapshot) and F10 (Cheats) are the Windows host's, from the same table
  (`key_policy`) and the same menu model; F2 opens the controls card (JPT-PAR9F).
* **Sanitizers.** ASan and UBSan (`-DJPT_LINUX_SANITIZE=ON`, leak detection off) run clean on the stream, save, gamepad, SDL window,
  HLE, Smacker (HLE and FFmpeg), video and session suites.

## Host features over the lifted game (JPT-PAR8H)

The Windows host's gameplay, physics, camera and menu features run on Linux from the same sources. `src/host/guest_abi.h` is the
interface they are written against: the lifted runtime's registers and `ADDR()` guest memory, `host_log`, `guest_call`, the guest reads
and writes (`guest_read32`, `guest_readable`, `guest_writable`, `guest_load_string`, `guest_scratch`), the stable-entry overrides
(`guest_override_install`) and the game's window stack (`guest_ui_probe`). Windows implements it in the host (`host.h` includes it), Linux
in `src/linux/linux_host.c`, `linux_overrides.c` and `linux_features.c`.

- **One table.** `src/host/guest_override_list.h` (71 sites) and `src/host/lift_hook_list.h` (27 hooks) are read by both hosts.
  `guest_override_lift.inc` gives each listed function its stable entry `lift116_sub_<va>` and its row in the registry
  (`override_registry.c`); a replacement is installed through the entry, never into the ELF. The lifted code calls the entries, so an
  override is reached by every direct call, alias entry and dispatch.
- **Call forms.** An override is a `void (*)(void)` that reads the guest's registers and stack as the lifted body would: REPLACE (it
  never calls the original), AROUND (it calls `*lifted` once, with its own changes before and after, as `stow`, `camera_set_properties`,
  `CPartition::bAddChild`), and the lift hooks, which run in front of one instruction with the guest's registers and may change them.
  `g_alias_entry` is checked by every override that must leave alias entries to the original.
- **Fingerprints and failure.** The registry refuses an install before the executable is verified (`target_verified`; the boot has
  compared the image with the build the active lift was generated from), an unlisted address, a second install, a missing replacement
  and the lifted body as its own replacement; each refusal is recorded and a required one stops the run. The registry chosen is the
  one of the active lift; a registry that belongs to another lift is refused. `lx_overrides_reset` puts every entry back on its
  lifted body.
- **Shared features.** Dual stow, Maximum object detail, the collision-sound re-arm, the 100 MB disk-space bypass and the Quick Save
  list filter (`ce_gameplay.c`), Active AI (`active_ai.c`), the official Patch 1.1 (`patch117*.c`, its level files served fixed from the
  LZ calls through `patch117_level_bytes`), the pelvis and water physics (`physics_fixes.c`), the terrain tolerances, the field of view
  (`widescreen.c`, the part that does not need the display's mode table), Skip Intro (`skip_intro.c`, the game's own NoVideo branch), the
  F3 and F10 menus (`debug_menu.c`, `overlay.c`, `objectives.c`, `cheats.c`), the minimap and the language (`l10n.c`: game text through LoadStringA, the voice captions, the level TEXT actions and the menu DDF files translated into the cache). Windows-only: the mode table and the
  Direct3D engine limits, the GDI and Direct3D 11 painters (`debug_menu_win.c`, `ce_quick.c`).
- **Menus.** `debug_menu_panel.cpp` is the Dear ImGui code both hosts draw the panels, the compass and the minimap with; Linux draws it
  into a copy of the presented frame through the CPU backend `imgui_software.cpp` (`linux_panel.c`), at the guest's resolution. Keys,
  characters, the pointer and the pad go through `lx_hostkeys_filter`, the user32 events and `lx_features_pointer_*`.
- **Tests.** `linux_overrides_tests` (stub lifted bodies and fake guest memory: installs, refusals, rollback, the call forms, threads, the
  behaviour of each shared feature through Linux guest pointers), `linux_features_tests` (the real lifted game plays New Game with the
  features on and the test reads the guest's memory) and `linux_panel_tests` (pixels and hit table of the panels).
- **FFmpeg.** When the machine has no `libavformat`/`libavcodec`/`libavutil`, `tools/test-linux.sh` runs `tools/ffmpeg-bootstrap.sh`: a
  pinned FFmpeg n7.1.1 (commit verified), LGPL-only (no `--enable-gpl`, `--enable-nonfree` or `--enable-version3`), static, with only the
  Smacker demuxer and its two decoders, built once into `~/.cache/jpt-recomp/ffmpeg`. No sudo, nothing installed on the system; offline it
  says so and the video rows are BLOCKED.

## HD front end, loading screen, controls card, pad menus and Russian (JPT-PAR9F)

One implementation, two presenting sides. The Windows host's front end is a bridge that reads the game's window stack into a scene
(`frontend_hd.c`, `frontend_layout.c`), a painter that draws it (`frontend_assets.c`, `frontend_art.c`, `frontend_plaque.c`) and a
loading screen built the same way (`loading_hd.c`, `loading_layout.c`). Those files are compiled for Linux unchanged apart from what
is below; nothing is copied.

* **Backend seam.** `frontend_platform.h` is what the bridge and the loading screen ask of the host: where the finished layer goes,
  the host pointer, the clock, files, the user folder. On Windows it is inline calls to the Direct3D 11 renderer (nothing changed:
  `frontend_tests` 27 699 checks, `gamepad_tests`, `input_tests` pass as before). On Linux it is `src/linux/linux_frontend.c`.
* **Painter.** Only text and threads were Windows: GDI measures and rasterises the glyphs and `CreateThread` runs the bands. On Linux
  `frontend_text_stb.c` measures and rasterises the same roles with the embedded TrueType face (stb_truetype, the display code page
  converted through `jpt_cp_to_utf8`, so Cyrillic comes out of the bytes the game stores) into the same 8-bit glyph cells, and the bands
  run on pthreads. Layout, materials, art (the game's own bitmaps, Catmull-Rom scaled), alpha and colour-key rules are the shared code:
  the Linux suite checks one thread against four and a dirty rectangle against a full repaint, byte for byte.
* **Compositor** (`lx_frontend_compose`, called by `lx_screen_present` between the video subtitles and the host overlays). The frame
  becomes the window's size: black bars, the guest picture scaled into the rectangle `lx_present_view` gives it (the same rectangle
  the mouse mapping uses), the HD layer over it (copied 1:1 when it has the rectangle's size, bilinear otherwise), then the pointer.
  The guest's framebuffer is never altered. The layer is painted at the rectangle's size within the existing 17 Mpixel budget, so a
  window of any size, aspect or scale (100/150/225 % are 1920x1080, 2880x1620, 4320x2430 in the suite), a resize or fullscreen
  changes only the rectangle. Text and menus are sharp at the window's resolution, not at 640x480.
* **When it shows.** HD menus when the guest is at 640x480 and a window the front end knows is in front; anything else (another guest
  mode, a window it cannot read, gameplay) is the game's own frame. The HD loading screen needs a rectangle larger than 640x480
  and the loader window in front, and replaces the Windows guest-size layer with the rectangle's size, so a 640x480 guest gets a
  window-resolution loading screen. The "Loading Level" plaque and the optional `assets/frontend/loading_hd/<slot>.png|jpg` art work
  as on Windows (PNG and JPEG are decoded with the stb_image bundled in SDL's tree).
* **Save pictures.** `<saves>\previews\<save>.png` instead of `.jpg` (SDL3's PNG writer and the stb decoder; Windows keeps WIC and
  JPEG), written when the save is created and deleted with it (`lx_frontend_file_created/deleted` from the `_open` and `DeleteFileA`
  HLE), from the game's frozen gameplay picture.
* **Hit tests and hover.** The game still does them: the pad or the mouse moves the game's pointer, its controls draw their own hover
  and pressed flags, and the layer shows what the game reports. The pointer sprite is hidden under pad focus.
* **Controls card (F2, View).** `linux_card.c`: the card and the hint are the Windows model (`jpt_prompt_card`, `jpt_card_fit`, the
  translations, the tokens of the keymap and bindings, `pad_glyph_coverage` for the glyphs) drawn by the host's overlay stage inside the
  picture's rectangle. The Windows card is an entry of the game's text overlay; here the host draws it, so the same rows, columns and
  glyphs appear, translated, and the game's own text is untouched. F2 reaches the shared overlay owner; the UI loop opens and closes it.
* **Pad in the menus.** `gamepad_ui.c` is the Windows host's UI-loop step (shortcuts over gameplay, host menus, the game's menus through
  `gamepad_menu.c`); Windows and Linux call it. D-pad and left stick move focus, A clicks, B and Start are Escape, View opens the card.
  Keyboard and mouse input from the user switches the device back (host-made keys and clicks are marked and do not).
* **Russian.** The display code page reaches the text rasteriser (`lx_text_set_codepage` follows the language), the painter and the
  card. Proven by pixels: the same bytes draw different glyphs in 1251 and 1252, all 64 letters have glyphs, and the front end, the
  loading screen, the card, the panels and the video subtitles draw Cyrillic.
* **Not done.** The Windows widescreen modes (the mode table override) are not on Linux, so the guest stays at its own mode and
  menus only take the HD path at 640x480; save pictures written by Windows (`.jpg`) are not read on Linux and the reverse.

Suites: `linux_frontend_tests` (219 checks: scene reading, painter, text, compositor geometry, pointer, resize, session, performance,
loading, card), `linux_gamepad_tests` (73: menu navigation, Start/View/B, device sharing), `linux_save_files_tests` (save pictures),
`linux_overrides_tests` (F2 through the host key table, 30 overrides of 71 installed).

## Next blocker

After the first manual run (log `trespasser_linux_002.log`): `DrawTextExA` (done: `0x00405EC0` is the text method of a UI element,
flags `0x400` then `0x0`/`0x25`/the element's own, `SetBkMode(1)`, text and shadow colours), then `lz32!LZOpenFileA`, `GetFileTime`,
`_findfirst`, `printf`, and `IDirectDrawSurface::GetDC` (the game retries it forever on an error: that was a hang, not a stop).
The older text below describes the state before those.


The headless runs do not stop on a missing import any more: the whole session of the suite above (menu, videos, level, Save, Load, Quit,
second game, Quit to the desktop) runs through the game's own code. What a real display, speaker and controller show cannot be known from
here; the first manual `--run` of this round is the test of that.

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
| Lifted builds `lift116`, `lift_es117`, `lift_ptbr117` with their SelfMod sections, Linux bindings for the profile override lists and the 27 baseline hooks (the shared features install over them: see above) | `src/linux/linux_lift.c` | all three link into the executable; the installed version selects one |
| Guest path resolution, registry store, synchronisation, file interface | `src/common/jpt_vpath.*`, `jpt_reg_store.*`, `jpt_sync.*`, `jpt_fs*.c` | tested |
| kernel32 HLE: 46 of 59 imports | `src/linux/linux_hle.c` | tested (`linux_hle_tests`) |
| msvcrt HLE (start-up, `_EH_prolog`, exit, heap, x87 helpers, descriptors and `FILE`, `sprintf`, strings, numbers) and the data exports `_acmdln`, `_adjust_fdiv`, `_iob`, `__pioinfo`, `__badioinfo` | `src/linux/linux_msvcrt*.c` | reached by the bootstrap; no unit test of its own yet |
| Folders and logging (`trespasser_recomp_NNN.log`, same canonical format as Windows; v0.1.1 `trespasser_linux_NNN.log` kept as historical compatibility) | `src/linux/linux_paths.*`, `linux_log.*` | tested |
| user32, gdi32, DirectDraw, presentation policy | `src/linux/linux_user32.*`, `linux_gdi32.*`, `linux_ddraw.*`, `linux_screen.*` | tested (`linux_user32_tests` 68, `linux_gdi_tests` 35, `linux_ddraw_tests` 58, `linux_present_tests` 23 checks) |
| Window backends (virtual, SDL3) | `src/linux/linux_window.*`, `linux_sdl_window.c` | virtual tested; SDL3 built, never run |
| Processor.dll, dsound (no device), PE resources | `src/linux/linux_processor.*`, `linux_dsound.*`, `linux_resource.*` | exercised by `linux_boot_tests` |
| Bootstrap | `src/linux/linux_boot.*` | runs the guest headless to its first frame, and to the main menu (`linux_boot_tests`, 14 checks) |

Not done: winmm (`joyGetPosEx`, the old joystick), a real display, speaker and controller run (the headless measurements above are the
evidence so far).

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

`trespasser_recomp` with no mode option opens the launcher and then runs the guest in an SDL3 window until the guest exits (SDL picks
X11 or Wayland; `SDL_VIDEODRIVER` overrides it); `--run` skips the launcher. `--bootstrap-headless` runs the headless bootstrap against
the virtual window and prints one of the `BOOTSTRAP_*` results; `--dry-run` maps the image and reports the HLE coverage. The window and
the launcher are the first things to try by hand; the log (`Linux presentation: SDL3 window ...`) says what the window system did.

```
trespasser_recomp [--game DIR] [--version ID] [--user_data_root DIR] [--log-dir DIR] [--log-file FILE]
                  [--launcher | --skip-launcher | --run | --dry-run | --bootstrap-headless] [--launcher-renderer NAME]
                  [--renderer NAME] [--graphics-card ID] [--<setting> VALUE ...]
```

Defaults: `game/`, `user_data/` and `logs/` next to the program (`TRESPASSER_GAME_DIR`, `TRESPASSER_USER_DATA_ROOT`,
`TRESPASSER_LOG_DIR` override them, arguments override those). The guest image is `<game>/versions/<id>/guest.img`.

## Vulkan renderer (`src/vulkan/`, `src/linux/linux_vulkan.*`)

* **Requirements.** A system Vulkan loader (`libvulkan.so.1`, loaded dynamically at run time — never linked as `DT_NEEDED` and never bundled in the AppImage) and a Vulkan 1.1/1.2+ ICD (AMD RADV, Intel ANV, NVIDIA, or Mesa `llvmpipe` for headless testing).
* **Backend selection.** `trespasser.ini` (`renderer = auto | vulkan | software`) or `--renderer <name>` on the CLI. `auto` selects Vulkan when available and falls back cleanly to Software with a logged reason if no loader/ICD/compatible GPU exists. `d3d11` is unavailable on Linux and falls back cleanly without crashing.
* **GPU selection.** `graphics_card` or `--graphics-card <auto | label | vendor:device | vendor:device:uuid8>`. `auto` checks minimum requirements first (graphics + present queues, `VK_KHR_swapchain`, depth attachment format), prefers usable discrete GPUs over integrated/virtual/CPU devices, and respects `DRI_PRIME`, `MESA_VK_DEVICE_SELECT`, `VK_LOADER_DEVICE_SELECT`, and `__NV_PRIME_RENDER_OFFLOAD`.
* **Fallback and recovery.** Missing loader/GPU falls back to Software with a readable message. Swapchain `OUT_OF_DATE`, `SURFACE_LOST`, resize, minimize (`0x0` wait), and persistent `SUBOPTIMAL` (capped to avoid Gamescope/Wayland loops) are handled cleanly; `VK_ERROR_DEVICE_LOST` latches cleanly without infinite recovery loops.
* **Direct3D 6 bridge (`linux_d3d.c`, `linux_d3d_target.c`, `linux_d3d_guest.c`).** With Vulkan the game takes its own Direct3D path
  (`Use D3D`, `D3D GUID`, `Hardware Water` and, until the game stores one, `RenderQuality` are answered as on Windows). The vtables are
  those of `d3d.h`: IDirect3D3 12, IDirect3DDevice3 42, IDirect3DViewport3 21 and IDirect3DTexture2 6 methods, each popping its own
  arguments; what the game never calls is refused with DDERR_UNSUPPORTED and one log line. IDirect3D3 and IDirect3DTexture2 are faces of
  the DirectDraw object and of the texture surface. z-buffers attach, and a level's worth of surfaces fits (8192).
  The target is hybrid as on Windows: text and the polygons the game rasterises go to its 16-bit copy and only what changed is drawn
  over the GPU's scene; a Lock that reads gets the scene back (a polygon only its box). In gameplay Flip shows the GPU's scene with the
  host's overlays as a layer; under a menu, a dialog or a loading screen the CPU's picture is made complete and takes the usual path,
  so the HD front end is unchanged. The target keeps the guest's shape inside the launcher's render resolution, and a CPU picture is
  shown in its own shape, as the SDL renderer shows it and as the mouse is mapped.
* **Water refraction, Enhanced sky, Fog, Shadows, Ambient occlusion.** With Vulkan they have effect on Linux: the water's rows run as
  the host kernel, a Load links the drawn texture to its wave field (one field per water texture), the sky's draws take the level's fog
  band, the device's fog colour and the Clear2 background follow the level at BeginScene, and ambient occlusion gets the camera's view.
  Fog and Shadows act on the engine's own data, so they also apply to Software. All are read at start.
* **Validation state.** Implemented, and covered headless by `linux_d3d_tests` (vtables and stack sizes of every method, HRESULTs, guest
  writes, device life, states, textures, water, sky, the hybrid target, Flip) with a recorder in place of the GPU. **Not validated by
  eye yet**: no gameplay frame through this bridge on a real Linux GPU. `--renderer software` keeps the game's own rasteriser. At exit
  the log's `[d3d]` line counts frames, draws, texture uploads, water and sky draws, readbacks and refused states.
* **Known limitations.** A host-rasterised text or a blended software polygon costs a readback of its area or of the screen; the
  software polygons Windows can draw on the GPU (textured and bump, 1.0.116) stay on the CPU; a texture changed without Lock, Load or
  Blt is not noticed (none was seen on Windows). Manual Steam Deck verification remains; the AppImage is packaged by `tools/package-linux.sh`. No RenderDoc or validation layers are bundled (`JPT_VK_VALIDATION=1` optionally enables Khronos validation in development when installed).
