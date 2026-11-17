# Launcher, display and graphics

`trespasser_recomp.exe` opens the launcher before the game, in the same process, and runs the lifted build of the version you play. Pages: Play, Display, Graphics, Gameplay,
Controls, Cheats, Bug Report (coming later) and About. There is no online or multiplayer page.

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
| `launcher/iso9660.cpp`, `is_cabinet.cpp`, `inflate.cpp` | the disc image reader, the InstallShield 5 cabinet of the Russian disc, raw DEFLATE |
| `launcher/game_files.cpp`, `game_manifest_<id>.inc` | the game versions ([GAME_VERSIONS](GAME_VERSIONS.md)), their files and digests, the installed versions, Remove, SHA-256 (CNG) |
| `launcher/game_install.cpp` | Import game: identity, plan, staging, digest check, repair, activation, rollback |
| `launcher/settings.cpp` | the pending view of `trespasser.ini`; the game's registry value InvertMouse |
| `launcher/user_data.cpp` | the portable `user_data` root, `PortFolder()`, the migration of the old `user` folder ([user-data](user-data.md)) |
| `launcher/system_info.cpp` | the graphics cards (`src/common/jpt_gpu.c`) and their real MSAA levels |
| `assets/launcher/` | header art, sponsor heart, page icons with `SOURCES.md` (README.md there) |
| `src/common/` (C) | `trespasser.ini` parsing and writing, render sizes, presentation, monitors, user files: shared by host, launcher and tests |
| `src/host/widescreen.c` | the gameplay mode and the Hor+ camera |
| `tests/launcher_tests.cpp` | headless tests (docs/BUILD.md) |

## Folders

| Folder | Holds | Default |
|---|---|---|
| game | the installed versions in `versions\<id>\` (an installation made before versions existed stays directly in it), never written by the port at run time | `game\` (`--game`, `TRESPASSER_GAME_DIR`) |
| user data | `launcher\trespasser.ini`; `saves\`: `recomp-registry.txt` (the game's registry), `SAVEGAME.*`, level swap files ([user-data](user-data.md)) | `user_data\` next to the program (`--user_data_root`, `TRESPASSER_USER_DATA_ROOT`; `build\user_data` with `tools\build.ps1 -Run`) |
| logs | one `trespasser_recomp_NNN.log` per run | `build\logs\` (`--log-dir`, `--log-file`) |

The game builds its save and swap paths from the registry value `Installed Directory` and reads its data from
`Data Drive`\data. The host answers `Installed Directory` with the selected version's saves folder
(`user_data\saves` for English 1.0.116, `user_data\versions\<id>\saves` for the others) and `Data Drive` with the
selected version's folder, so saves never land in the installed game. At start, the old flat `user\` folder is moved into `user_data`
([user-data](user-data.md)), and `recomp-registry.txt` and `SAVEGAME.*` of an even older layout are copied (not moved)
from `game\` to `user_data\saves` when it has no file of that name.

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
(`native32_hold_range`): `0x00400000` up to the largest guest image of any lift the program carries (their `SizeOfImage`
in `config/`, generated into `src/host/profiles`), whatever is installed, so a version imported in the same run maps. The
range is held even with nothing installed, so no second process is started after an import.

## Pages

The header reads **JURASSIC PARK: TRESPASSER** over `RECOMPILED · v<version> · BY SAULOB`, over the header art.
The sidebar has the other ports' icons; the footer has the hints, **Sponsor on GitHub** (with the heart, left of Quit,
opening `https://github.com/sponsors/saulob` in the browser and nothing else), Quit, Save and PLAY.

| Page | Rows |
|---|---|
| Play | status card, Game version (with more than one installed), Game files (Import / Reinstall, progress and Cancel), Remove version, Show this launcher |
| Display | Display mode, Window size, Monitor, Aspect ratio, Presentation |
| Graphics | Renderer, Graphics card, Render resolution, Field of view, Anti-aliasing, Ambient occlusion, Water refraction, Enhanced sky, Maximum object detail, Texture filtering, VSync |
| Gameplay | Language, Skip Intro, Video subtitles, Frame rate in title, Render resolution in title; Trespasser CE: Dual stow, Active AI |
| Controls | Gamepad (the controller connected now and its status, Controller glyphs, Look sensitivity X/Y, Invert look Y, Stick and Trigger deadzone); Gamepad controls (what every button does with the current bindings, in the shown glyph style); Gamepad bindings (one per action, Gamepad defaults); Keyboard & Mouse (Mouse sensitivity, Invert mouse, then every action with the keys the game loads from its `KeyMap`, including the mouse combinations for the hand); Port hotkeys (Quick Save and Quick Load, which can be changed, then the fixed ones; [CONTROLS](CONTROLS.md)) |
| Cheats | Movement Speed, Jump Height, Invulnerability, Infinite Ammo, Freeze Dinosaurs, Super Health: what every session starts with ([Cheats](#cheats)) |
| Bug Report | listed, "Coming later"; nothing active, nothing sent |
| About | Sponsor card, description and credits, Translation credits, folders, settings file, Reset settings, logs folder, port version, game, renderer, platform, libraries, repository link |

## Play

The card shows **Ready to play · English · Build 116** with the game's language and its release; **Game not installed**, **Invalid game files** with the reason, or, for a version the port cannot run
yet, why (see [GAME_VERSIONS](GAME_VERSIONS.md)). With more than one version installed, **Game version** chooses which
one Play starts (setting `game_version`; empty: the first ready one the port runs). A selected version that is removed
or broken is replaced by another ready one. The launcher's own language stays the one chosen in Gameplay. PLAY saves the pending settings once ("Settings saved. Starting game..."),
closes the launcher and the game starts. It is refused, with the reason, while the files are not ready or an install
runs; a failed save keeps the launcher open. **Save** writes without starting; **Quit** (Alt+F4) closes without saving.

## Import game

**Import game...** opens the file dialog for the player's own disc image (`.iso`); **Reinstall game...** replaces a
selected version whose files are no longer valid. The disc is recognised by the SHA-256 of one data file that differs in every version (never an executable) and by the
presence of every file of that version, never by its file name or volume label ([GAME_VERSIONS](GAME_VERSIONS.md)).

| Disc | Version | Installed into |
|---|---|---|
| English retail, UK (`Trespass_UK_116`) or US (`Trespasser_116d`) | English · Build 116 | `game\versions\en-116` |
| Russian, Fargus (`TRESPASSER_116D`, InstallShield 5) | Russian · Build 116 | `game\versions\ru-116` |
| Spanish (`TRESPASSER_S`) | Spanish · Build 117 | `game\versions\es-117` |
| Brazilian Portuguese | Brazilian Portuguese · Build 117 | `game\versions\ptbr-117` |

Each version installs `guest.img` (the guest image, made from the disc's `tpassp6.exe` and checked against its pressed
SHA-256; no file named like an executable is installed), `MSVCIRT.DLL`, `SMACKW32.DLL`, `Processor.dll`, `Readme.txt` and
`data\`. The Russian disc keeps its menu art and `tpassp6.exe` in `setup/data1.cab`, read natively (`is_cabinet.cpp`,
`inflate.cpp`), restores its 8.3 names (`tpassi~1.smk` is `tpassintro.smk`) and adds `text\strings.dat` (the string table of
its `SETUP/TRESPASS.EXE`, read as resources only) and `text\tresp.ttf`. The original executables are import inputs only,
are not required to play, and are never copied, hashed or looked for afterwards; the installers, `setup/DirectX`, EA
registration and help files are never copied. A disc without the file the guest image is made from is refused
(nothing is installed). The manifests are
generated by `tools/make_game_manifests.py` from the discs (names, sizes and digests only).

**Steps** (on a worker thread, with progress and Cancel):

1. Read the volume descriptors and the whole directory tree (Joliet when present). Damaged records, unsafe names,
   loops and extents outside the file are refused. No mounting, no setup program, no external tool.
2. Identify: the profile's identity data file with its digest, and every file of that version present with its size (and its
   cabinet readable). A Trespasser disc no version matches is **Unsupported Trespasser build**: nothing is copied and
   its fingerprints (volume, executables with size, SHA-256 and what they are when known) go to the log.
3. A version that is already installed and ready is **Already installed**: nothing is copied and it is selected.
4. Check the free space, then copy each file into `versions\<id>.install-temp` through a `.part` file, computing its
   SHA-256 on the way. A file whose digest differs from the pressed one is damaged: it is taken from another installed
   version that has the identical file with the pressed digest, or the import stops as **Damaged disc image** with
   the files named. (The Brazilian image has five level files with one flipped bit each; importing English first
   repairs them.)
5. Check the staging folder like an installed version, then activate: rename the current `versions\<id>` aside, move
   the staging folder in, delete the old one.

Any failure or Cancel before step 5 deletes the staging folder and leaves every installed version as it was. If the
second rename fails, the backup is renamed back. The image is opened for reading only and never written.

**Remove version** (with confirmation) deletes the selected version's folder; for an installation made before versions
existed, only the files of its manifest are deleted from `game\`. Disc images, saves, settings and other versions are
never touched.

**Older installations.** A `trespass.exe` left in `game\` or `game\versions\<id>` by an earlier launcher is ignored
(not listed, not deleted, not copied): import the version again to install its `guest.img`. Importing a version that is
already installed finds it and copies nothing.

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
| `graphics_card` | the card's name as the launcher lists it | empty: the first usable card |
| `render_resolution` | `original`, `720p`, `1080p`, `1440p`, `4k`, `native`, `WxH` | `native` |
| `aspect_ratio` | `4:3`, `16:9`, `16:10`, `21:9`, `32:9` | `16:9` |
| `texture_filter` | `original`, `bilinear`, `anisotropic4`, `anisotropic8`, `anisotropic16` | `anisotropic16` |
| `msaa` | 0, 2, 4, 8 | 4 |
| `ambient_occlusion` | `off`, `low`, `medium`, `high`, `ultra` | `medium` |
| `water_refraction` | 1, 0 | 1 |
| `enhanced_sky` | 1, 0 | 1 |
| `max_object_detail` | 1, 0 | 1 |
| `dual_stow` | 1, 0 | 1 |
| `active_ai` | `original`, `enhanced` | `enhanced` |
| `official_patch_117` | 1, 0 | 1 |
| `quick_save_key`, `quick_load_key` | `F6`, `F7`, `F8`, `F9`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown` | `F7`, `F8` |
| `vsync` | 1, 0 | 1 |
| `show_fps` | 1, 0 | 1 |
| `show_resolution_title` | 1, 0 | 0 |
| `skip_intro` | 1, 0 | 0 |
| `video_subtitles` | 1, 0 | 1 |
| `cheat_movement_speed` | 0.5, 1, 1.5, 2, 3, 5, 7.5, 10 | 1 |
| `cheat_jump_height` | 0.5, 1, 1.5, 2, 3, 5 | 1 |
| `cheat_invulnerability`, `cheat_infinite_ammo`, `cheat_freeze_dinosaurs`, `cheat_super_health` | 1, 0 | 0 |
| `language`, `language_<version id>` | `en`, `pt-BR`, `fr`, `de`, `es`, `ru`, `it` | the language of the game version |
| `launcher_x`, `_y`, `_width`, `_height`, `_maximized`, `_fullscreen` | the launcher window | |
| `settings_version` | 3 | written with every change |

**Migration.** The first, experimental launcher (test 018) wrote no `settings_version`, and its file cannot tell a
chosen value from one written while trying the page out. A file without `settings_version` therefore has its display
and graphics keys (`display_mode` to `show_fps` above, `graphics_card` aside) removed once, at the next start, so the current defaults apply;
`show_launcher` and the launcher placement are kept, and the log says `came from the experimental launcher`.

Version 2 files were written by the launcher whose graphics defaults were Original filtering, MSAA off and VSync
off (log 023). Once, at the next start, exactly those values are removed (`texture_filter = original`, `msaa = 0` or
`1`, `vsync = 0`), so Anisotropic 16x, 4x MSAA and VSync On apply; every other value, and any other choice of those
three keys, is kept, and the log says `held the earlier launcher's graphics defaults`. Files written since carry
version 3 and are never reset, so choosing Original, Off or Off again sticks.

`recomp-registry.txt` stays the game's own registry; the Controls page edits its `InvertMouse` value, which the game's
options menu also changes. The launcher does not touch the game's other options. The run log's `[settings]` lines show what a run uses.

## Language

**Gameplay > Language** is the one language setting. Each installed game version keeps its own choice under
`language_<version id>` (`language_ptbr-117 = en`); a version with no choice yet plays in the language of its release
(English, Russian, Spanish or Brazilian Portuguese), and choosing English is stored like any other choice. The key
`language` of earlier builds still counts for English 1.0.116 and for `--language`, and never changes another version's
default. The release's own text (Russian strings and font, the Spanish and Brazilian Portuguese string tables) is kept;
the port's translation only fills what the release lacks. Languages are built into the port: there is nothing to
download, install or choose a folder for. Every language has the game's text, the words on its menu pictures, subtitles
and the port's own panels; voices, effects, music and videos always stay the English game files.

| `language` | Game text, menus and subtitles | Launcher |
|---|---|---|
| `en` | English | English |
| `pt-BR` | Brazilian Portuguese | Brazilian Portuguese |
| `fr` | French | English |
| `de` | German | English |
| `es` | Spanish | English |
| `ru` | Russian | English |
| `it` | Italian | English |

The combo lists English, Brazilian Portuguese, French, German, Spanish, Russian and Italian; with the launcher in
Brazilian Portuguese the same labels read Inglês, Português Brasil, Francês, Alemão, Espanhol, Russo and Italiano.

A game version released in the chosen language keeps its own menu pictures and text: only the Brazilian Credits button
(lettered Area de Trabalho in the release) is corrected. Another language relabels the pictures with the port's labels.
The game's own Show Subtitles option (Options > Audio) turns the captions on and off in every version; the Russian,
Spanish and Brazilian Portuguese releases hide the row, so the port draws it (Subtitles, Субтитры, Subtítulos,
Legendas). The option is On until a choice is stored, an Off chosen afterwards stays, and the choice belongs to each
version's own settings. The Spanish and Brazilian Portuguese Stream.tpa carry no caption text at all: the port gives them the captions
(and marks each sample's descriptor as having a block, as the game requires), translated into the language chosen.

**The launcher in Português Brasil.** Only Português Brasil translates the launcher; every other language leaves it in
English. The launcher follows the setting every frame, so choosing Português Brasil changes the whole launcher at once
(no Save, no restart) and choosing another language brings English back. The texts are the rows of
`launcher/localization/pt_br_ui_text.cpp` (English text to Português Brasil, the same scheme as the other ports); a text
without a row stays English, and messages built at run time are translated by known phrase. Logs, diagnostics and the
settings file stay English. The test `launcher_tests` fails when a text the pages show has no row.

**Settings of earlier builds.** `game_text_language` and `subtitle_language` became `language`: at the next start a file
with only the old keys gets the language they gave (a game text language wins, else the subtitle language), the old keys
are removed, and a `language` already there is kept. Files with the old keys still load.

The translated text lives in `assets/translations` (UTF-8) and is compiled into the executable:
`<language>.strings.txt` and `<language>.subtitles.txt` are the releases' text, made by `tools/translation_pack.cpp`
from the extracted TresCom downloads (not part of the build); `<language>.strings.port.txt` and
`<language>.subtitles.port.txt` complete them and are read only for what a release lacks (and for the key names the
Russian strings left in English); `<language>.interface.txt` holds the words of the menu pictures and the port's own
text, with `en.interface.txt` as the list of keys every language must have. GameStrings keep their IDs, string 10 (the
saves' file name) is never translated, and captions keep their own timings (0.2 s units; the port's captions use those of
the English sample). The only files written at run time are a translated copy of each menu `.ddf` the game opens, in
`user_data\cache\menu-text\<language>`, rebuilt at every start; deleting them changes nothing.

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
Monitor setting's monitor. 640x480 menus and videos get the largest 4:3 size inside it. The Aspect Ratio never changes by itself: 16:9 is the default modern shape, 4:3 (original) stays available, and a saved choice is never replaced.

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

**Field of view** (key `fov`, 60-110, default 75). Trespasser CE's `FOV` in `[Render]`: "how it would appear on a 4:3
display", the full horizontal angle of the 4:3 view, widened for wider displays unless its `LockHorizFOV` is set. In
CE it is read once (`0.0` or absent: the game's own angle; else clamped to 30-175) and applied when the main camera is
created, as the ratio of the new angle to the camera's own. Retail has no setting: `CCamera::SProperties` (`0x0051F720`)
sets 75 degrees through `SetAngleOfView` (`0x0051F890`, `+0x48 = tan(angle / 2)`, angles in 2^32 units per turn),
and a level's `CSetRendererAction` (`0x0046EFF0`) can set another angle. The wrapper above scales the angle the game
asks for by `fov / 75` before the Hor+ widening, so 75 is the retail camera bit for bit and a level's own angle keeps
its ratio. The range stops at 60 (the 4:3 vertical view 47 degrees) and 110 (4:3 vertical 94 degrees, 125 degrees
across at 16:9, 151 at 32:9): wide enough for modern screens while the projection stays well short of 180 degrees
(the angle is also capped at 170). There is no Vert-: every aspect ratio keeps the 4:3 vertical view of the chosen
field of view. The host sets it by calling `SetProperties` on the main camera again with its own stored properties
from the game's UI loop, which changes only its projection (never its position, rotation, speed or the save).

This happens before anything is projected, so the D3D TLVERTEX path, the software rasteriser and culling all see the
same wider view; nothing is stretched afterwards. Presentation Stretch only decides how the final picture fits the
window.

**Menus and HUD.** Menus are 640x480 and stay 4:3 (see Resolutions). In gameplay, the game lays out its text (hints,
subtitles) relative to the 3D viewport with left, centre and right anchors (`0x0050E8C0`); its font follows the
screen height (Resolutions, Gameplay text), so text stays undistorted and on screen. A dedicated ultrawide HUD layout
is not part of this work.

## Graphics

- **Renderer**: Direct3D 11 (default) or Software, for the whole run (there is no switch while playing).
- **Graphics card** (key `graphics_card`, the name as listed): every adapter Direct3D 11 can create the game's
  device on, in DXGI order (software adapters are left out). Two cards with the same name are both listed; the second is
  shown and saved as `Name (2)`, the third `Name (3)`, and names that are unique get no suffix. A setting with a
  plain duplicated name chooses the first of them. The game creates its device on the chosen card (its DXGI
  adapter index, kept when an unusable adapter sits before it). A missing setting, or a card that is no longer installed, uses the first card in the list, never a vendor
  preference; the launcher saves that card, and the log says `saved graphics card "..." not available; using ...`.
  The `[d3d11]` lines `graphics card: ... (DXGI adapter N)` and `device created: ..., adapter "..."` show the choice and the device.
  Software uses it too: its picture and the videos are shown through the same device. Anti-aliasing lists the
  chosen card's levels. Without a usable card the page says No graphics card found and Direct3D picks the device.
- **Field of view**: a slider (60-110 degrees across the 4:3 view) with its value and Default (75); see True widescreen.
- **Texture filtering**: Original (the filters the game asks for, its single texture level), Bilinear (every
  texture, filtered between generated mipmaps too), Anisotropic 4x/8x/16x (where the game filters, with generated
  mipmaps; its point-sampled draws stay point). The renderer falls back 16x, 8x, 4x, 2x where a card has less and
  logs it once.
- **Anti-aliasing**: Off, 2x, 4x, 8x MSAA. The launcher lists only the levels the graphics card supports for the
  game's colour and depth formats (asked of a short-lived device); a saved level that is not supported is pointed
  out, the game falls back 8x, 4x, 2x, off and logs it once, and a multisampled target that cannot be created is
  created without MSAA instead of failing.
- **Ambient occlusion**: Off, Low, Medium, High, Ultra (key `ambient_occlusion`, Medium by default; `--ambient-occlusion`
  for one run). Intel ASSAO over the D3D11 3D scene only: Low, Medium, High and Ultra are ASSAO's quality levels 0-3
  (Ultra is its adaptive level). It needs feature level 11.0; without it, or if the effect cannot be created, it stays
  Off and the log says why.
- **Water refraction** (key `water_refraction`, On by default; Direct3D 11 only, greyed out with Software and kept):
  after Trespasser CE's refractive water. The game writes CEntityWater's rows (`water_rows`, 0x0060E3F0, from
  `FillWaterTexture`, 0x0060E590) into the system-memory source surface of its water raster (the pointer
  `CRasterD3D::Lock` got), then `CRasterD3D` vtbl+0x1C (0x00510240) copies that surface into the Direct3D texture the
  draws bind with `IDirect3DTexture::Load`. The host keeps, by texel address, the wave height's differences to the next
  column and row of every texel a row writes (exact floats; CE keeps them as bytes, trunc(x * 127.5 + 127.5)), and a
  Load whose source holds such texels links the texture it fills to that source: that texture is the water, whatever
  else it looks like. A draw of it with alpha blending gets its own shader: the scene behind is sampled at the pixel's
  own screen position (`SV_Position`) shifted by those differences, 0.35 of the target height per unit, faded as CE fades it by 1 / max(distance, 1 m). A vertex's
  rhw is 0.1 / its distance in metres (`CCamera::fInvZAdjust`), so the fade is saturate(10 x rhw): Beach water 6 to 33 m
  away fades to 0.155 to 0.03 of the full shift. The shader returns lerp(shifted scene, water colour, water alpha)
  with alpha 1, so the game's SRCALPHA / INVSRCALPHA blend keeps it as is (no second blend), as CE's own water shader
  (the shifted position is kept inside the game's 3D viewport, at texel centres: the shift is shortened along its own
  direction at the picture's edge, so the black outside the viewport is never sampled, filtered or smeared)
  does: still water draws exactly what Off draws, the refracted scene's share is 1 - alpha (the retail water palette's
  alpha runs from 0.25 to 1.0 with the light, 0x006F24E8 filled by 0x0051DBD0), and the water's colour, fog and alpha
  are the original's. The scene is copied once a frame before the first water batch (`CopyResource`, or
  `ResolveSubresource` with MSAA): one copy and one shader per frame, GPU only. Off, and any texture that is not linked,
  take the original path untouched. If its resources cannot be created, refraction is off for the session and the log
  says so once. The log says `water refraction: the 256x256 water texture is loaded from the surface its wave rows
  write` when the link is first made, `first water batch` with the texture's and vertices' alpha, the texture blend and
  the batch's distances and fade at the first refracted draw, and at exit the water batches, scene copies, the largest
  wave difference and the shift in pixels of the water's pixels (average, p95, p99, largest, with the wave and distance
  of the largest), sampled per triangle and weighted by area. With Software, the launcher greys it out ("Available with the Direct3D 11
  renderer") and keeps its value.
- **Enhanced sky** (key `enhanced_sky`, On by default; Direct3D 11 only): the retail hardware sky (`CSkyRender::DrawD3D`,
  0x005242C0) is one textured plane fogged per vertex by depth, so its horizon haze ignores the level's sky fog band.
  CE's enhanced sky fogs by view direction instead: with v the horizontal length of the unit view direction (0 straight
  up, 1 at the horizon), none up to the band's low edge (CSkyRender +0x30), all of it from the high edge (+0x2C),
  linear between; equal or inverted edges give a step and never a division, and unusable values (NaN, infinite, far
  out of range) keep the retail sky. The port draws the same retail plane and texture and takes v per pixel from the
  plane point the texture coordinates give (u = x · 1e-4 + 1.5 + scroll, v = y · 1e-4 + scroll) against the camera.
  Object, terrain and water fog are unchanged.
- **Maximum object detail** (key `max_object_detail`, On by default): as Trespasser CE's `ForceMaxObjectDetail`, the
  renderer's `bDetailReduce` (render settings `[[0x006D2224]]+0x33`) is cleared, so the object pipeline (0x00519B20)
  and the terrain shadow pass always take an object's top mesh instead of the lower-detail one 0x004A7F10 would pick
  for its screen size. Impostor images and terrain shadows keep their own settings, as in CE. Off is the retail
  choice. The log says `Maximum object detail: every object keeps its most detailed model` once.
- **VSync**: On keeps the game's own Flip request (waits for the refresh); Off never waits.
- **Frame rate in title** and **Render resolution in title** (Gameplay, as in the other ports; keys `show_fps`, On by
  default, and `show_resolution_title`, Off by default): the title is always `Jurassic Park: Trespasser Recompiled | x86 |
  D3D11` (the renderer's name), then ` | 2560x1920` and ` | FPS: 60.0` when they are on and known, in that order, never
  with a separator left over. The resolution is the render size with D3D11 (the Render resolution box), the gameplay
  mode with Software or Original, not the window or the desktop; menus and videos do not change it, and nothing is shown
  until it is known. FPS appears after the first measured rate. There is no in-game frame counter, so the Gameplay
  page has no Frame counter option.
- **Skip Intro** (Gameplay, as in Dragon's Crown Pro PC; key `skip_intro`, Off by default): the opening video before
  the main menu (`tpassintro.smk`) and the one after New Game (`newgame.smk`) are not shown. When the game is about to
  play one of them, its own `NoVideo` value reads 1 for that call only, so `CVideoWnd::Play` takes the game's own
  no-video branch and returns as after the video: the main menu, or the level load, follows. The credits, the ending
  and the Video Test still play. Read when the game starts, so a change in the launcher applies to that run.
- **Video subtitles** (Gameplay; key `video_subtitles`, On by default; a file without the key and Reset all settings
  give On): the spoken lines of `tpassintro.smk`, `newgame.smk` and `win.smk` in the game's language, drawn over the
  picture; `credits.smk` has no dialogue. Only the retail files (size and SHA-256 of their first 64 KiB) get them. Read
  when the game starts. Log: `[video-subtitles] enabled`, `tpassintro.smk timeline: 16 cues`.
- Draw distance, LOD, texture sizes, texture memory and generated mipmaps are not settings: they are part of the
  Direct3D 11 renderer (see the README and `[graphics]` in the log).

## Cheats

The page lists the cheats worth keeping from one session to the next, as Dragon's Crown Pro PC does: **Movement Speed**
and **Jump Height** (a select of the same values F10 steps through, 1.0x the original) and **Invulnerability**,
**Infinite Ammo**, **Freeze Dinosaurs** and **Super Health** (Off or On, Off by default). Nothing is on in a file
without the keys, and a value that is not one of the listed ones reads as the default. The page's help says: press F10
while playing to open the Cheats menu; what it changes applies only to the current session.

- **One list.** `src/common/jpt_cheats.c` holds every cheat once: label, help text, settings key, kind, default and
  choices. The page (`LauncherCheats()` in `launcher/options.cpp`), F10 (`debug_menu_policy.c`: its rows and labels) and
  the settings table read it; `cheats_tests` and `launcher_tests` fail when they stop agreeing.
- **Saved and session.** Save and PLAY write the keys like any other setting; Reset all settings puts them back to the
  defaults in the file too. At start the host reads them once (`cheats_start`, log line `[cheats] saved: Movement Speed
  1.0x, ...`) and F10 starts from those values. F10 never writes the file: it only records what it changed over the
  saved value (`jpt_cheat_session_t`), so a cheat cycled back follows the saved value again, and the next start uses
  the saved values.
- **F10 only.** Heal, Teleport and Level Select are actions, not settings. Noclip and Stealth Mode are toggles of the
  session, never saved and Off at every start. Fly / Free Camera is a mode of the level in
  play (it needs the camera's link to Anne's head, which exists only in a running level, and it takes the controls from
  her), so it is never saved or started On: F10 turns it on, and another level or the main menu turns it off.
- **Removed from F10.** Show Coordinates and Third Person.

## Gameplay > Trespasser CE

A section of the Gameplay page: "Gameplay enhancements reimplemented natively from Trespasser CE." Nothing of Trespasser
CE is needed or read; the options are the port's own code over the retail game.

- **Dual stow** (key `dual_stow`, On by default): one item on the belt and one on the back while the hand stays free.
  Retail already has the two slots (CPlayer +0xE38 belt, +0xE3C back) and saves both; what it lacks is a way to end
  with both stowed and the hand empty. The port wraps the retail stow handler (0x004610D0) as CE does it:
  - holding **Rotate Arm** or **Rotate Wrist** (by default Ctrl or Shift; LB/RB on a gamepad) while pressing
    **Stow/Retrieve** stows the held item into the slot it fits and does not fetch the other slot's item;
  - with both slots full and the hand empty, Stow/Retrieve with Rotate Arm or Rotate Wrist held takes the back's
    item, without them the belt's;
  - everything else is retail (a full slot swaps with the held item, an item that fits nowhere is dropped);
  - CE's per-slot retrieve pose is kept on the host side, seeded from the save's.
  No save format change: a save with both slots full is an ordinary retail save. Off is the retail handler. Turning it
  Off with two items stowed loses nothing: the next Stow/Retrieve takes the belt's item, as retail would.

Quick Save and Quick Load are not options: they are always available, with their keys on the Controls page
(F7 / F8 by default; [CONTROLS](CONTROLS.md)).

- **Active AI** (key `active_ai`, `enhanced` by default; `original` or `enhanced`): how many creatures can be active
  (thinking, moving, with physics) at once. Original is the retail limit of 3 and installs nothing: the game runs its
  own code. Enhanced is 6, as Trespasser CE's `MaxActiveAi` (CE accepts up to 7; the port offers 6). Retail compiles
  the limit into the `CAISystem` code, so Enhanced replaces the 12 functions that use it (constructor and destructor,
  Activate, Deactivate, the radius release and the two radius queries, the think scheduler, Process, save, load, and the
  brain's influence loop through its tail call) with the same logic over 6 entries; entries 1 to 3 stay in the game's
  object and 4 to 6 live beside it. Activation keeps the retail rule (the nearest animal displaces the farthest one) and
  adds CE's physics guard: retail's physics holds 50 boxes (0x00699D40) and its allocator silently gives an object no
  physics when they run out, so an animal whose boxes do not fit (in use, less the one it replaces) stays inactive; the
  log says so once. Saves keep the retail layout: the active list is a fixed block of 10 handles, which already holds 6,
  so a save is byte-identical to retail's while 3 or fewer are active, and retail saves load unchanged. Read when the
  game starts: a change applies at the next start.

- **Patch 1.1 fixes** (key `official_patch_117`, On by default; Gameplay > Official Patch 1.1): DreamWorks
  Interactive's official v1.1 build 117 patch, studied against the retail build 116 and its levels; the patch is never
  needed or read. Every retail level scene (`be`, `jr`, `ij`, `it`, `lab`, `as`, `as2`, `sum`) carries a `Terrain`
  section that `CTerrain::pcLoad` (0x00475010) loads into `TrrPixelTol` (0x0066FAD8) and `TrrPixelTolFar`
  (0x0066FB68); build 117's scenes change them from 8 and 20 to 10 and 50 pixels in every level, the patch's "Adjusted
  terrain texture resolution". The game reads a scene with `LZOpenFileA`/`LZRead`/`LZSeek`; when the opened file is a
  retail level scene (name, size and SHA-256), the port reads it, writes the two official values only where the retail
  ones still are, and serves those bytes to that handle. Any other file, or a scene whose values differ, is read as it
  is (`[patch117] ... skipped`). Each load starts from the retail file, so the fixes apply once per load. Saved games
  keep the values they were saved with, as build 117 does with a 1.0 save, and stay retail saves (build 117 only raised
  the save version from 22 to 23 to look its levels up in its own `Patch` folder; the port keeps 22). Under Direct3D 11
  the port's terrain caps still apply on top (`terrain_quality.c`). Off reads every file unchanged. The log says
  `[patch117] official Patch 1.1 fixes: on` at start and `[patch117] applied 2 official level fixes to BE` when a level
  loads.

  The same setting carries the patch's "Occasional object disappearance" fix. When an object moves, the world database
  re-files it with `CPartition::bAddChild(object, test_priority = 1)` (0x00475F60, from the move path 0x00605AF0): a
  partition refuses an object of lower priority (bits 0-3 of +0x10) and otherwise offers it to its children first. Build
  116 asks the children with the test off, so a moved object could be filed under a child of higher priority, which the
  render traversal (0x00519B20) and the shadow pass (0x00543C20) skip with everything under it when the render quality
  is below that priority: the object and its shadow vanished. Build 117 passes the caller's flag down; the port's
  override of 0x00475F60 does the same when the setting is On and leaves every call as retail when Off. Loading and
  rebuilding the partitions pass the flag off in both builds and are unchanged.

  The other fixes run inside large lifted functions, through host hooks the lift places in front of single
  instructions (`src/host/lift_hook_list.h`, `tools/lift_hooks.py`); every hook does nothing when the setting is Off.

  - **Jump consistency.** Build 116 replaces the pelvis jump request (0x0067AFA0..A8) on every frame in the hand-off
    0x004F0960, so a press is lost when no 10 ms physics step of that frame finds ground. Build 117 keeps it: the key's
    hit frame and, while the key stays held, every frame with no request pending arm it, and Foot_IDOF (0x004E2470)
    drops it after 25 ground checks that could not jump (0.25 s). A held key therefore jumps again on landing. The
    game's own impulses (0x00456490, 0x004609F0) get the same lifetime.
  - **Object pickup, keypad interaction, centering aim** (build 117's hand rework). The raise moves into a function of
    its own (117 0x443320, reimplemented in `patch117_player.c` over 116 functions): an empty hand aims at the view box
    centre at 0.7 m (0.8 m in 116; the stow target too) and, for its first 0.4 s, a hand whose way is blocked by
    anything but the terrain or the player is tucked at the shoulder instead of being pushed into it; the hand settles
    by time or a clear way, not by reaching the box. The mouse steers it only once raised, a grab needs the new state 3
    (the hand once inside the view box; 116 grabbed at 2), and the view follows the hand only in state 3 (116 dragged it
    at once). A picked-up object is lifted 0.1 m at once and held 2 s (116: 1 s, with a 0.5 s strong arm at every raise
    that 117 dropped) before it is pulled in to carry. Raising the hand with a free gun no longer recentres it in the
    view, and looking past the head's 30 degree range turns the body by the whole angle. No keypad code exists in the
    executable: keypads are level triggers the finger touches, so the keypad fix is this hand (shorter reach, no
    pushing into panels).
  - **Interpenetration of compound objects, box-box.** In box-box 0x004E9780 a corner already deeper than the 0.09 m
    skin behind every face of a sub-box got no force; 117 pushes it out along the line from the sub-box core and kills
    the closing velocity. The velocity kill (0x004E85A0) now turns the face normal by the sub-box's own rotation. The
    edge-edge contact code is in build 116 but its gate (per-pair bytes +0xFB4) is never armed; 117 runs it once per
    pair, from the box with the larger smallest half extent (taken when the box is set up, 0x004EDFA0). In the broad
    phase (0x004DC600) pairs that ignore each other's collisions now wake each other. The rest of the box-box changes
    (coefficients hoisted out of the corner loop, cached pointers, the box pool relayout) give the same results and are
    not reproduced; the world-query changes follow 117's rebuilt terrain records and have no visible effect outside
    the terrain's edge, so they are not either.
  - **Floating objects.** The world files (`.grf`, read expanded through `LZRead`) of IJ and IT get the official
    placements of 177 static objects (IJ's billboard vegetation lowered 0.5 m, palms and shrubs in IJ and IT), and AS's
    scene the official resting place of one rifle, each written only over its retail bytes in a retail file (name,
    size, SHA-256). Left out, because build 117 placed them together with changes the port cannot reproduce:
    objects on or near re-encoded terrain (`.wtd`: every level except AS and AS2; all of BE's and JR's moves), objects
    whose new place leaves their 116 partition (build 117 rebuilt every partition: the AS2 truck and its crates, AS's
    ruins, most of IJ's billboards, IT's keypad lowered 0.15 m), everything resting on those, and the added and removed
    objects. A world file is read again on every load, saves included, so the placements follow the setting at once;
    the rifle's applies when AS starts from its level file. `[patch117] applied 172 official object placements to IJ`.

Not listed: **Experimental AI behaviours** (CE's `EnableAiActivityEx`) are not applicable to the retail executable: the
14 activity classes it enables do not exist in it at all, so there is nothing to switch on. There is no **Bump mapping**
option: the original bump maps are already drawn by the game's own bump spans into the Direct3D 11 frame, exactly as
retail.

Vanilla (closest to retail): Dual stow Off, Active AI Original, Water refraction Off, Enhanced sky Off, Maximum object
detail Off, Ambient occlusion Off. Quick Save / Quick Load and the bug fixes stay.

## Next targets (not in this work)

A dedicated ultrawide HUD, an uncapped frame rate and the x64 host (its own branch).

## Manual test

1. Start the game: the launcher opens on Play with "Ready to play · English · Build 116", the header art, the sidebar
   icons and Sponsor on GitHub left of Quit; compare it with Dragon's Crown Pro PC and 13 Sentinels side by side.
2. Graphics: Render resolution follows Display > Aspect ratio (16:9 1440p: 2560 × 1440; 21:9: 3440 × 1440).
3. 4:3, 16:9, 21:9, 32:9 in turn: more world to the sides, the same height, no stretching, text readable.
4. Render resolution 1440p, then 4K: the log's `3D render target WxH` lines show the real size.
5. F11, Esc, F1, F2 (controls card in a level), Alt+F4 as before (F12 does nothing; F5 toggles the minimap); F11's choice is kept for the next start.
