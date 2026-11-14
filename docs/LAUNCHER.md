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

The header reads **JURASSIC PARK: TRESPASSER** over `RECOMPILED · v<version> · BY SAULOB`, over the header art.
The sidebar has the other ports' icons; the footer has the hints, **Sponsor on GitHub** (with the heart, left of Quit,
opening `https://github.com/sponsors/saulob` in the browser and nothing else), Quit, Save and PLAY.

| Page | Rows |
|---|---|
| Play | status card, Game files (Install / Reinstall, progress and Cancel), Show this launcher |
| Display | Display mode, Window size, Monitor, Aspect ratio, Presentation |
| Graphics | Renderer, Render resolution, Render size (what will really be created), Anti-aliasing, Texture filtering, VSync, Graphics card |
| Gameplay | Gore (the game's 0-2), Invert mouse, Frame rate in title |
| Controls | Gamepad (the controller connected now and its status, Controller glyphs, Look sensitivity X/Y, Invert look Y, Stick and Trigger deadzone); Gamepad controls (what every button does with the current bindings, in the shown glyph style); Gamepad bindings (one per action, Gamepad defaults); Keyboard & Mouse (every action with the keys the game loads from its `KeyMap`, including the mouse combinations for the hand); Port hotkeys ([CONTROLS](CONTROLS.md)) |
| Cheats, Bug Report | listed, "Coming later"; nothing active, nothing sent |
| About | Sponsor card, description and credits, folders, settings file, Reset settings, logs folder, port version, game, renderer, platform, libraries, repository link |

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
`--display-mode`, `--monitor`, `--window-size`, `--presentation`, `--show-fps`, `--skip-intro`).

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
| `skip_intro` | 1, 0 | 0 |
| `launcher_x`, `_y`, `_width`, `_height`, `_maximized`, `_fullscreen` | the launcher window | |
| `settings_version` | 3 | written with every change |

**Migration.** The first, experimental launcher (test 018) wrote no `settings_version`, and its file cannot tell a
chosen value from one written while trying the page out. A file without `settings_version` therefore has its display
and graphics keys (`display_mode` to `show_fps` above) removed once, at the next start, so the current defaults apply;
`show_launcher` and the launcher placement are kept, and the log says `came from the experimental launcher`.

Version 2 files were written by the launcher whose graphics defaults were Original filtering, MSAA off and VSync
off (log 023). Once, at the next start, exactly those values are removed (`texture_filter = original`, `msaa = 0` or
`1`, `vsync = 0`), so Anisotropic 16x, 4x MSAA and VSync On apply; every other value, and any other choice of those
three keys, is kept, and the log says `held the earlier launcher's graphics defaults`. Files written since carry
version 3 and are never reset, so choosing Original, Off or Off again sticks.

`recomp-registry.txt` stays the game's own registry; the Gameplay page edits its `Gore` and `InvertMouse` values,
which the game's options menu also changes. The run log's `[settings]` lines show what a run uses.

## Resolutions

Five sizes are kept apart:

| | What | Example (16:9, 1440p on a 1080p desktop) |
|---|---|---|
| gameplay | the game's own screen in gameplay: its DirectDraw mode, back buffer, z-buffer, viewport, camera and text | 2560x1440 |
| 3D render target | the Direct3D 11 colour, MSAA and depth targets | 2560x1440 |
| UI virtual canvas | menus and the in-game videos, as the game draws them | 640x480 |
| presentation | the swap chain shown in the window, letterboxed or stretched; the title bar names it | 2560x1440 |
| window | the client area | Window size |

With Direct3D 11 the game plays at the render size itself: its DirectDraw mode is the render box, so its viewport,
`D3DTLVERTEX` coordinates, clip extents, camera and text are all in those pixels and the target is drawn 1:1. Larger
boxes than 3840x2160 pixels in all (21:9 and 32:9 at 4K, 4:3 at 4K) play at the largest mode of their shape within that
budget, a multiple of 8 each way, and D3D11 still rasterises at the box. The retail engine has no limit below that
(32-bit addressing, 16.16 span coordinates good to 32767 pixels, viewport from the surface); the budget keeps the
host's per-mode buffers (front and back RGB565, 32-bit z, the renderer's reference and overlay copies: 14 bytes a
pixel, 111 MB at 3840x2160) small in this 2 GB process. The game's mode table (`0x006D0B48`, 60 sizes) and its video
options list (`0x0066DCA0`, 20 sizes up to 800 wide) are unchecked arrays: the host lists 12 modes at most.

The software renderer has its own limit: it keeps the Aspect Ratio's compatibility mode (4:3 800x600, 16:9 1024x576,
16:10 960x600, 21:9 1280x540, 32:9 2048x576), because the CPU rasterises every pixel. The registry's `Width`/`Height`
answer the mode of the renderer chosen at start, which stays until the game closes. Original keeps the
compatibility mode too, with the earlier automatic target (the smallest 4:3 size covering the presentation area).

**Render resolution.** A preset names a 16:9 size; with the Aspect Ratio (Display page) it gives the render box: wider
shapes keep the preset's height, narrower ones its width, and 21:9 takes the ultrawide monitor sizes. Native fits the
Monitor setting's monitor. The Graphics page shows the result as **Render size**. 640x480 menus and videos get the
largest 4:3 size inside it. The Aspect Ratio never changes by itself.

| Preset | 4:3 | 16:9 | 16:10 | 21:9 | 32:9 |
|---|---|---|---|---|---|
| 720p | 1280x960 | 1280x720 | 1280x800 | 1720x720 | 2560x720 |
| 1080p | 1920x1440 | 1920x1080 | 1920x1200 | 2560x1080 | 3840x1080 |
| 1440p | 2560x1920 | 2560x1440 | 2560x1600 | 3440x1440 | 5120x1440 |
| 4K | 3840x2880 | 3840x2160 | 3840x2400 | 5120x2160 | 7680x2160 |
| Native, 1920x1080 monitor | 1440x1080 | 1920x1080 | 1728x1080 | 1920x810 | 1920x540 |

The run log names each size: `[settings] render resolution 1440p, aspect ratio 16:9: gameplay resolution 2560x1440
..., 3D render target 2560x1440`, `software renderer gameplay resolution 1024x576`, `UI virtual canvas 640x480 for menus
and videos (source 640x480), shown 4:3 at 1920x1440; presentation 2560x1440`, then per display
`[window] game screen WxH; presentation WxH in a WxH client` and `[d3d11] 3D render target WxH ... for the game's WxH
screen`. A window smaller than the render size shows it reduced; the game still renders at the render size.

**Menus and videos** keep the game's 640x480 canvas: their 4:3 picture is shown centred in the frame with its shape
kept (pillarboxed in 16:9 and wider), with the cursor mapped to that rectangle. Smacker videos are 640x480 assets and
are shown the same way; nothing is added to their pixels. Neither changes the resolution in the title.

**In-game menus and dialogs** (Esc menu, Options, Audio, Video, Controls, Save, Load, Restart, Quit, confirmations)
are drawn by the game for 640x480 and are 320x200 panels centred in the screen (`0x00411EA0`). Over a larger game
screen with D3D11 they are drawn into an offscreen 640x480 raster of the game's own DIB raster class (`0x0040FD80`)
and the renderer scales that canvas, in the same pass that presents the frame: the frozen gameplay picture stays on
the game's screen at its own resolution, the canvas is centred as the largest 4:3 rectangle (scale not rounded: 3x at
2560x1440, 2.25x at 1920x1080, 4.5x at 3840x2160, 3x at 3440x1440 and 5120x1440 with wider side bars) and its
unwritten texels are transparent. The hooks are the UI frame (`0x00417300`, which draws the windows and the cursor and
presents), the window and cursor draws (`0x00417380`, `0x00417410`, given the canvas as their raster), the frozen
picture's background (`0x0040C780`, given the screen back) and the centring (`0x00411EA0`, which sees a 640x480
screen). The canvas is sampled with the frontend's linear filter. The game reads the cursor raw (`GetCursorPos`), so
in menus and dialogs the host answers canvas coordinates (and the same in mouse messages): the frame pixel under the
cursor, taken at its centre, over the canvas rectangle, so every click lands on the control it is drawn over. The
menu text is GDI drawn into private DIBs by the controls (font sizes fixed in pixels), so it scales once, with the
canvas; the gameplay text policy above does not touch it. Windows are composed in the render frame and the whole frame
is shown reduced in a smaller window, so the window size never changes the canvas. The loading windows draw
themselves straight to the screen and stay as they were, and the software renderer keeps the original sizes.

**Gameplay text** (hints, tutorial, subtitles) is the game's own GDI text over the 3D picture. The game sizes its Arial
font from the screen width, `clamp(width / 20, 12, 32)` (`0x00468A80`, font class `0x0050E750`), so at 2560 wide it
would stay 32 pixels. The host keeps the share of the height it has at 800x600: on screens taller than 600 lines the
font is `height * lines / 600` (77 pixels at 1440), and the game's own layout (wrap width, line spacing, centring) uses
the font's measured widths, so it follows. The font is drawn aliased, as on the systems of the time, so drawing it reads
nothing under it: the text's `GetDC` starts a capture instead of a GPU readback, which at 2560x1440 would move 7 MB each
time a hint is drawn. Text stays at the game's anchors (top-left, centre or right of the whole screen); a centred
16:9 safe area for ultrawide is not part of this work.

Composited CPU pixels (text, software polygons) are drawn over the GPU target point-sampled. Each upload covers the
changed box and one transparent texel around it: at a non-integer scale (Original, or a mode inside a larger box) the
box's first target row can fall exactly on a texel edge, and the texel above the box used to keep an older picture,
which showed as a horizontal line through the tops of the text.

## True widescreen (Hor+)

Gameplay runs at a mode of the Aspect Ratio's shape (see Resolutions). Found in the retail binary:

- `0x00412A00` reads the registry `Width`/`Height` and snaps them to the mode table (`0x006D0B48`) filled by
  `0x00502970`, whose byte argument lifts an 800-pixel width cap in its callback `0x00502A60`; a size missing from the
  table falls back to 640x480 (`0x00502B40`). The host passes 1 (thunk wrapper, every aspect ratio, 4:3 included),
  lists both renderers' gameplay modes in its DirectDraw (12 modes at most) and answers `Width`/`Height` with the
  current renderer's mode, so the in-game video options (which list only modes up to 800 wide) cannot change it.
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

**Menus and HUD.** Menus are 640x480 and stay 4:3 (see Resolutions). In gameplay, the game lays out its text (hints,
subtitles) relative to the 3D viewport with left, centre and right anchors (`0x0050E8C0`); its font follows the
screen height (Resolutions, Gameplay text), so text stays undistorted and on screen. A dedicated ultrawide HUD layout
is not part of this work.

## Graphics

- **Renderer**: Direct3D 11 (default) or Software, for the whole run (there is no switch while playing).
- **Texture filtering**: Original (the filters the game asks for, its single texture level), Bilinear (every
  texture, filtered between generated mipmaps too), Anisotropic 4x/8x/16x (where the game filters, with generated
  mipmaps; its point-sampled draws stay point). The renderer falls back 16x, 8x, 4x, 2x where a card has less and
  logs it once.
- **Anti-aliasing**: Off, 2x, 4x, 8x MSAA. The launcher lists only the levels the graphics card supports for the
  game's colour and depth formats (asked of a short-lived device); a saved level that is not supported is pointed
  out, the game falls back 8x, 4x, 2x, off and logs it once, and a multisampled target that cannot be created is
  created without MSAA instead of failing.
- **VSync**: On keeps the game's own Flip request (waits for the refresh); Off never waits.
- **Frame rate in title** (Gameplay, as in the other ports; key `show_fps`): the title shows the renderer, FPS and
  the selected resolution (the render size with D3D11, the gameplay mode with Software or Original), which menus and
  videos do not change; Off shows only the name.
- **Skip Intro** (Gameplay, as in Dragon's Crown Pro PC; key `skip_intro`, Off by default): the opening video before
  the main menu (`tpassintro.smk`) and the one after New Game (`newgame.smk`) are not shown. When the game is about to
  play one of them, its own `NoVideo` value reads 1 for that call only, so `CVideoWnd::Play` takes the game's own
  no-video branch and returns as after the video: the main menu, or the level load, follows. The credits, the ending
  and the Video Test still play. Read when the game starts, so a change in the launcher applies to that run.
- **Graphics card**: DXGI adapter 0, the one the game's device uses. No adapter choice yet.
- Draw distance, LOD, texture sizes, texture memory and generated mipmaps are not settings: they are part of the
  Direct3D 11 renderer (see the README and `[graphics]` in the log).

## Next targets (not in this work)

A dedicated ultrawide HUD, an adapter choice, an uncapped frame rate and the x64 host (its own branch).

## Manual test

1. Start the game: the launcher opens on Play with "Ready to play · Trespasser UK 1.0.116", the header art, the sidebar
   icons and Sponsor on GitHub left of Quit; compare it with Dragon's Crown Pro PC and 13 Sentinels side by side.
2. Graphics: the Render size row follows Display > Aspect ratio (16:9 1440p: 2560 × 1440; 21:9: 3440 × 1440).
3. 4:3, 16:9, 21:9, 32:9 in turn: more world to the sides, the same height, no stretching, text readable.
4. Render resolution 1440p, then 4K: the log's `3D render target WxH` lines show the real size.
5. F11, Esc, F1, F2 (controls card in a level), Alt+F4 as before (F5 only opens the Video Test in the main menu; F12
   does nothing); F11's choice is kept for the next start.
