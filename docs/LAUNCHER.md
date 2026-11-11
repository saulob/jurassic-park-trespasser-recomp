# Launcher, display and graphics

`trespasser.exe` opens the launcher before the game, in the same process. Pages: Play, Display, Graphics, Gameplay,
Controls, Cheats (coming later), Bug Report (coming later) and About. There is no online or multiplayer page.

## Architecture

The launcher is the one of Dragon's Crown Pro PC and 13 Sentinels: Aegis Rim PC.
[TRG Launcher](https://github.com/TekRantGaming/trg-launcher) 1.1.0 with their patch series
(`launcher/patches/trg-launcher`, the 19 patches of 13 Sentinels), in an SDL3 window with SDL's software renderer and
Dear ImGui's SDL3 backends. The host, placement, header art, page icons, Play gate and page layout are their files
with this port's names. SDL 3.4.10 (their version), Dear ImGui and TRG are fetched by `tools\build.ps1` at the commits
in `config/launcher_deps.txt` into `external\` (not committed); SDL is built as a static library, since MSVC x86 has
no SDL package. TRG's downloader is not built: the launcher makes no network request. Only one SDL window and one ImGui
context exist at a time, and all of SDL is shut down (`SDL_Quit`) before the game's window and Direct3D 11 device
exist.

What differs, because the game differs: the game window is the host's Win32 window, so the Monitor list is the one
that window uses (primary first, then the system's order); images are decoded with SDL's own `SDL_LoadPNG` instead of
libpng; the launcher's `[launcher]` lines go to the host log; there is no update check, no separate game process and no
online page.

| Path | Purpose |
|---|---|
| `launcher/launcher.cpp` | pages, Play flow, start-up decision, `jpt_launcher_run` (C entry, `jpt_launcher.h`) |
| `launcher/host.cpp`, `displays.h`, `placement.h` | the launcher window: SDL3, SDL renderer, Dear ImGui, F11, placement |
| `launcher/header_art.cpp`, `page_icons.cpp`, `options.cpp` | header art placement, sidebar icons (nanosvg), page model and labels |
| `launcher/iso9660.cpp` | the disc image reader |
| `launcher/game_files.cpp`, `game_manifest_uk_116.inc` | supported builds, the readiness check, SHA-256 (CNG) |
| `launcher/game_install.cpp` | Install game: identity, plan, staging, check, activation, rollback |
| `launcher/settings.cpp` | the pending view of `trespasser.ini`; the game's registry values Gore and InvertMouse |
| `launcher/system_info.cpp` | the graphics card and its real MSAA levels |
| `assets/launcher/` | header art, sponsor heart, page icons with `SOURCES.md` (README.md there) |
| `src/common/` (C) | `trespasser.ini` parsing and writing, render sizes, presentation, monitors, user files: shared by host, launcher and tests |
| `src/host/widescreen.c` | the gameplay mode and the Hor+ camera |
| `tests/launcher_tests.cpp` | headless tests (docs/BUILD.md) |

## Folders

| Folder | Holds | Default |
|---|---|---|
| game | the installed game, never written by the port at run time | `game\` (`--game`, `TRESPASSER_GAME_DIR`) |
| user | `trespasser.ini`, `recomp-registry.txt` (the game's registry), `SAVEGAME.*`, level swap files | `user\` (`--user`, `TRESPASSER_USER_DIR`) |
| logs | one `trespasser_NNN.log` per run | `build\logs\` (`--log-dir`, `--log-file`) |

The game builds its save and swap paths from the registry value `Installed Directory` and reads its data from
`Data Drive`\data. The host answers `Installed Directory` with the user folder and `Data Drive` with the game folder,
so saves never land in the installed game. At start, `recomp-registry.txt` and `SAVEGAME.*` of an older layout are
copied (not moved) from `game\` to `user\` when `user\` has no file of that name.

## Start-up

| Game files | Show this launcher | Result |
|---|---|---|
| missing or invalid | any | the launcher opens |
| ready | On (default) | the launcher opens; PLAY starts the game |
| ready | Off | the game starts directly |
| ready | any, with Shift held or `--launcher` | the launcher opens |
| ready | any, with `--skip-launcher` | the game starts directly (a skip wins over Shift, as in the other ports) |

`--dry-run` never opens it. When its window cannot be opened, the game starts if the files are ready.

The host reserves the game's fixed address range by restarting itself before anything loads
(`native32_hold_image`), which needs `game\trespass.exe`. When the game is installed by this very run, the process
could not reserve it, so after PLAY it starts the game as a new process (`--skip-launcher`, same log) and returns its
exit code.

## Pages

The header reads **JURASSIC PARK: TRESPASSER RECOMPILED**, `PC PORT · v<version> · BY SAULOB`, over the header art.
The sidebar has the other ports' icons; the footer has the hints, **Sponsor on GitHub** (with the heart, left of Quit,
opening `https://github.com/sponsors/saulob` in the browser and nothing else), Quit, Save and PLAY.

| Page | Rows |
|---|---|
| Play | status card, Game files (Install / Reinstall, progress and Cancel), Show this launcher |
| Display | Display mode, Window size, Monitor, Aspect ratio, Presentation |
| Graphics | Renderer, Render resolution, Render size (what will really be created), Anti-aliasing, Texture filtering, VSync, Graphics card; Advanced graphics, disabled, "Coming later" |
| Gameplay | Gore (the game's 0-2), Invert mouse, Frame rate in title |
| Controls | the port hotkeys |
| Cheats, Bug Report | listed, "Coming later"; nothing active, nothing sent |
| About | Sponsor card, folders, settings file, Reset settings, logs folder, port version, game, renderer, architecture, graphics card, libraries, credits, repository link |

## Play

The card shows **Ready to play · Trespasser UK 1.0.116** (with `trespass.exe` and its SHA-256), **Game not installed**
or **Invalid game files** with the reason. PLAY saves the pending settings once ("Settings saved. Starting game..."),
closes the launcher and the game starts. It is refused, with the reason, while the files are not ready or an install
runs; a failed save keeps the launcher open. **Save** writes without starting; **Quit** (Alt+F4) closes without saving.

## Install game

**Install game...** / **Reinstall game...** opens the file dialog for the player's own disc image (`.iso`). Example
image: `Trespasser_EN.iso`, 652,992,512 bytes, volume `Trespass_UK_116`.

**The disc format.** Plain ISO 9660 with a Joliet supplementary tree (long names). The original installer
(`setup95.exe`) does not use CAB or InstallShield archives: every file the game needs is stored uncompressed.

| On the disc | Installed as |
|---|---|
| `setup/tpassp6.exe` (Pentium Pro/II build, 1.0.116) | `trespass.exe` |
| `setup/msvcirt.dll`, `setup/smackw32.dll`, `setup/processor.dll` | `MSVCIRT.DLL`, `SMACKW32.DLL`, `Processor.dll` |
| `readme.txt` | `Readme.txt` |
| `data/` (251 files) | `data\` |

`setup/tpassp5.exe`, `setup/tpassk6.exe`, the installer and `setup/DirectX` are not installed. That is exactly the
256-file, 594,182,171-byte tree that `tools/extract_game.py` lays out for development.

**Steps** (on a worker thread, with progress and Cancel):

1. Read the volume descriptors and the whole directory tree (Joliet when present). Damaged records, unsafe names,
   loops and extents outside the file are refused. No mounting, no setup program, no external tool.
2. Identify the disc: the volume identifier and the size of `setup/tpassp6.exe` must match a known profile;
   another disc, or another version of it, is refused before anything is copied.
3. Check the free space, then copy each planned file into `<game>.install-temp` through a `.part` file.
4. Check the staging folder like an installed game: `trespass.exe` SHA-256
   `d88bb548d8f7c893de2d5a2ffb69fa513a3d75b7e5c336990b63ec5cad5679ed`, then every file of the profile's manifest
   present with its size. An unknown executable is refused.
5. Activate: copy the old folder's saves and registry store to `user\` (when missing there), rename `game` to
   `game.install-backup`, rename the staging folder to `game`, delete the backup.

Any failure or Cancel before step 5 deletes the staging folder and leaves the current installation as it was. If
the second rename fails, the backup is renamed back. An image stored inside the game folder is refused. Profiles are
a table (`game_files.cpp`): another version is one more entry with its own hash and manifest.

## Settings

`user\trespasser.ini`, `key = value`; lines a writer does not own (comments, other keys) are kept, and the file is
replaced in one step. Booleans are written `1`/`0` and read as `1`/`on`/`yes`/`true` and `0`/`off`/`no`/`false`. A
missing or invalid value is the default, in the launcher and the game alike. The command line overrides a setting
for one run (`--renderer`, `--render-resolution`, `--aspect-ratio`, `--texture-filter`, `--msaa`, `--vsync`,
`--display-mode`, `--monitor`, `--window-size`, `--presentation`, `--show-fps`).

| Key | Values | Default |
|---|---|---|
| `show_launcher` | 1, 0 | 1 |
| `display_mode` | `windowed`, `borderless` | `windowed` (F11 saves it) |
| `monitor` | 1-based, primary first | 1 |
| `window_size` | `auto`, `WxH` | `auto` |
| `presentation` | `letterbox`, `stretch` | `letterbox` |
| `renderer` | `d3d11`, `software` | `d3d11` |
| `render_resolution` | `original`, `720p`, `1080p`, `1440p`, `4k`, `native`, `WxH` | `native` |
| `aspect_ratio` | `4:3`, `16:9`, `16:10`, `21:9`, `32:9` | `16:9` |
| `texture_filter` | `original`, `bilinear`, `anisotropic4`, `anisotropic8`, `anisotropic16` | `anisotropic16` |
| `msaa` | 0, 2, 4, 8 | 4 |
| `vsync` | 1, 0 | 1 |
| `show_fps` | 1, 0 | 1 |
| `launcher_x`, `_y`, `_width`, `_height`, `_maximized`, `_fullscreen` | the launcher window | |
| `settings_version` | 2 | written with every change |

**Migration.** The first, experimental launcher (test 018) wrote no `settings_version`, and its file cannot tell a
chosen value from one written while trying the page out. A file without `settings_version` therefore has its display
and graphics keys (`display_mode` to `show_fps` above) removed once, at the next start, so the current defaults apply;
`show_launcher` and the launcher placement are kept, and the log says `came from the experimental launcher`. Files
written since carry the version and are never reset.

`recomp-registry.txt` stays the game's own registry; the Gameplay page edits its `Gore` and `InvertMouse` values,
which the game's options menu also changes. The run log's `[settings]` lines show what a run uses.

## Resolutions

Four sizes are kept apart:

| | What | Example (16:9, 4K on a 1080p desktop) |
|---|---|---|
| guest logical | the game's own screen: menus 640x480, gameplay the Aspect Ratio's mode | 1024x576 |
| render | the Direct3D 11 colour, MSAA and depth targets | 3840x2160 |
| presentation | the swap chain shown in the window, letterboxed or stretched | the render size, scaled by DXGI |
| window | the client area | Window size |

The game's back buffer stays RGB565 at the logical size (no 4K CPU frame buffer). Its D3DTLVERTEX coordinates are in
logical pixels and become normalised device coordinates against the logical size in the vertex shader, so a larger
render target rasterises the same geometry at more pixels: nothing is drawn at 800x600 and scaled up.

**Render resolution.** A preset names a 16:9 size; with the Aspect Ratio (Display page) it gives the render box: wider
shapes keep the preset's height, narrower ones its width, and 21:9 takes the ultrawide monitor sizes. Native fits the
Monitor setting's monitor. The Graphics page shows the result as **Render size**. Gameplay renders into the box itself
(its mode is within 2% of the box's shape: 1280x540 is 2.370:1, 3440x1440 is 2.389:1, and the picture is shown in the
mode's shape, so that difference never reaches the screen); 640x480 menus get the largest 4:3 size inside it. The
Aspect Ratio never changes by itself.

| Preset | 4:3 | 16:9 | 16:10 | 21:9 | 32:9 |
|---|---|---|---|---|---|
| 720p | 1280x960 | 1280x720 | 1280x800 | 1720x720 | 2560x720 |
| 1080p | 1920x1440 | 1920x1080 | 1920x1200 | 2560x1080 | 3840x1080 |
| 1440p | 2560x1920 | 2560x1440 | 2560x1600 | 3440x1440 | 5120x1440 |
| 4K | 3840x2880 | 3840x2160 | 3840x2400 | 5120x2160 | 7680x2160 |
| Native, 1920x1080 monitor | 1440x1080 | 1920x1080 | 1728x1080 | 1920x810 | 1920x540 |

The run log separates the sizes: `guest logical resolution: gameplay 1024x576, menus 640x480`, `3D render resolution:
1440p -> gameplay 2560x1440, menus 1920x1440 (real render targets)`, `3D render target 2560x1440 ... (rasterised at
this size)` and `presentation WxH in a WxH client`.

Original keeps the compatibility size of earlier builds (the smallest 4:3 size covering the presentation area).
CPU pixels (text, software polygons) are composited over the GPU target and GPU readbacks for the game's Locks are
reduced to the logical size, as before.

## True widescreen (Hor+)

Gameplay runs at the Aspect Ratio's own mode: 4:3 800x600, 16:9 1024x576, 16:10 960x600, 21:9 1280x540,
32:9 2048x576. Found in the retail binary:

- `0x00412A00` reads the registry `Width`/`Height` and snaps them to the mode table (`0x006D0B48`) filled by
  `0x00502970`, whose byte argument lifts an 800-pixel width cap in its callback `0x00502A60`. The host passes 1
  (thunk wrapper), lists the gameplay mode in its DirectDraw (11 modes in all, under the table's limits) and answers
  `Width`/`Height` with that mode, so the in-game video options cannot change it.
- `CCamera::SetProperties` (`0x0051FD90`) builds the projection from `tan(half horizontal angle)` (props `+0x48`,
  75 degrees by default) and an aspect (`+0x4C`) that CRasterWin derives assuming a 4:3 monitor: a widescreen viewport
  alone gives a stretched 4:3 frustum. The same call builds the camera-to-screen transform, the clip volume used for
  culling and the pixels-per-unit LOD scale.
- The host wraps it for the main camera (`[[0x0073F444]+0x14]`, perspective only): the call gets
  `tan * (W/H)/(4/3)` and the true `W/H`, so the vertical extent is the 4:3 one and the horizontal one grows; the
  caller's values are stored back, because callers copy, edit and set the properties again and savegames keep the
  angle. The LOD scale stays the 4:3 one. 75 degrees becomes about 91 at 16:9, 107 at 21:9 and 128 at 32:9.

This happens before anything is projected, so the D3D TLVERTEX path, the software rasteriser and culling all see the
same wider view; nothing is stretched afterwards. Presentation Stretch only decides how the final picture fits the
window.

**Menus and HUD.** Menus are 640x480 and stay 4:3, letterboxed in the window, with the cursor mapped to that
rectangle. In gameplay, the game lays out its text (hints, subtitles) relative to the 3D viewport with left, centre
and right anchors (`0x0050E8C0`) and sizes its font from the raster width (`0x00468A80`), so text stays undistorted
and on screen. A dedicated ultrawide HUD layout is not part of this work.

## Graphics

- **Renderer**: Direct3D 11 (default) or Software. F5 still switches while playing.
- **Texture filtering**: Original (the filters the game asks for), Bilinear (every texture), Anisotropic 4x/8x/16x
  (where the game filters; its point-sampled draws stay point). No trilinear: the game's textures have one level.
- **Anti-aliasing**: Off, 2x, 4x, 8x MSAA. The launcher lists only the levels the graphics card supports for the
  game's colour and depth formats (asked of a short-lived device); a saved level that is not supported is pointed
  out, and the game logs any reduction (`requested level unsupported: reduced`).
- **VSync**: On keeps the game's own Flip request (waits for the refresh); Off never waits.
- **Frame rate in title** (Gameplay, as in the other ports; key `show_fps`): the title shows the renderer, FPS and
  logical resolution; Off shows only the name.
- **Graphics card**: DXGI adapter 0, the one the game's device uses. No adapter choice yet.
- **Advanced graphics**: Draw distance, Object LOD, Texture memory, Texture cache, Mipmaps and Texture detail are shown
  disabled, "Coming later".

## Next targets (not in this work)

Kept apart from resolution: draw distance (the far clip `+0x3C` and the per-quality table `0x0066C518`), object LOD
and its pixel-error thresholds (`MinPixels`, `PixelRatio`, `PixelError`, measured against the camera's 4:3 LOD scale),
billboard and impostor distances, LOD pop-in, terrain texture paging and its 1280-1920 KB budget, the texture cache
and texture memory limits. Also a dedicated ultrawide HUD, an adapter choice and the x64 host (its own branch).

## Manual test

1. Start the game: the launcher opens on Play with "Ready to play · Trespasser UK 1.0.116", the header art, the sidebar
   icons and Sponsor on GitHub left of Quit; compare it with Dragon's Crown Pro PC and 13 Sentinels side by side.
2. Graphics: the Render size row follows Display > Aspect ratio (16:9 1440p: 2560 × 1440; 21:9: 3440 × 1440).
3. 4:3, 16:9, 21:9, 32:9 in turn: more world to the sides, the same height, no stretching, text readable.
4. Render resolution 1440p, then 4K: the log's `3D render target WxH` lines show the real size.
5. F11, Esc, F1, F12, F5, Alt+F4 as before; F11's choice is kept for the next start.
