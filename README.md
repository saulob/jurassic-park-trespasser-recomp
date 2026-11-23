<div align="center">

# Jurassic Park: Trespasser - PC Port

### Native recompilation of the original 1998 PC game, with modern Windows support, experimental native Linux and Steam Deck support, modern graphics and display options, a minimap, compass and waypoints, gameplay improvements and the TRG launcher

[![Latest release](https://img.shields.io/github/v/release/saulob/jurassic-park-trespasser-recomp?style=for-the-badge&label=release&color=d89a2b&labelColor=0b1a12&include_prereleases&cacheSeconds=300)](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/saulob/jurassic-park-trespasser-recomp/total?style=for-the-badge&color=d89a2b&labelColor=0b1a12&cacheSeconds=300)](https://github.com/saulob/jurassic-park-trespasser-recomp/releases)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20experimental%20%7C%20Steam%20Deck%20experimental-2f7a46?style=for-the-badge&labelColor=0b1a12)

<sub>The game's machine code is translated to C once, ahead of time, and compiled into a native program. <b>It is not an emulator</b>, and on Linux it uses <b>no Wine and no Proton</b>. <b>No commercial game files are included</b>: bring your own legally obtained Trespasser disc image.</sub>

</div>

<img width="958" height="762" alt="2026-11-23_16-05" src="https://github.com/user-attachments/assets/67eab2b7-17bc-4d5b-928f-694cabb9e9eb" />

*Jurassic Park: Trespasser* was released for Windows by DreamWorks Interactive in 1998. This project is a static recompilation of that retail game: its levels, physics, dinosaurs, weapons, audio and videos run as native code on a modern host, with a modern renderer, display modes, navigation aids, controls and a launcher around it. The game is playable and still in testing, not a final remaster.

## Status

**v0.1.0 is the first public release.** Windows is the primary platform; the native Linux x86_64 build (AppImage), which also targets the Steam Deck, is experimental. The game installs from your disc image, starts, and runs its menus and levels with the Direct3D 11, Vulkan or software renderer.

| Milestone | State |
|---|---|
| Disc and executable inventoried, target fingerprinted | done ([ANALYSIS](docs/ANALYSIS.md)) |
| All of `.text` lifted to C: 0 lift errors, 0 unimplemented instructions, 0 unresolvable constant targets | done |
| All 297 imports resolve against this Windows' 32-bit DLLs | done |
| Host, launcher and installer build and run | done |
| Menus, video and audio | working |
| Levels and gameplay | playable, in testing |
| Linux and Steam Deck | experimental, not validated on a Steam Deck |

## Screenshots

<img width="1114" height="669" alt="2026-11-23_16-07" src="https://github.com/user-attachments/assets/fbb86b76-9199-4124-be0a-75f059b63d92" />

<img width="1114" height="672" alt="2026-11-23_16-09" src="https://github.com/user-attachments/assets/9d23f6db-d4e1-4a53-9641-09db13c4502f" />

<img width="1113" height="669" alt="2026-11-23_16-11" src="https://github.com/user-attachments/assets/a52d7816-c718-4d99-94b3-d93469b8575b" />


## Features

- **Native static recompilation**: the retail executable's code runs as C on the host CPU. No CPU emulator, no Wine, no Proton
- **Modern graphics**: Direct3D 11 on Windows, a Vulkan 1.2 renderer on Windows and Linux, and the original software renderer as a fallback, with render resolutions up to 4K on Windows
- **True Ultrawide on Windows**: 21:9 and 32:9 show more of the world at the sides instead of stretching the picture
- **Minimap, compass and waypoints**: a circular minimap, a compass strip, creature and vehicle markers, 3D objective markers and a distance readout, none of which existed in 1998
- **Gameplay improvements**: Quick Save and Quick Load, Dual Stow, Active AI, physics and collision fixes, and a reimplemented subset of the official 1.1 patch
- **Cheats panel** (F10) with movement speed, jump height, invulnerability, infinite ammo, frozen dinosaurs, health, noclip, stealth, teleport, Level Select and a free camera
- **Modern controls**: keyboard, mouse and SDL3 gamepads (Xbox, PlayStation, Switch, Steam Deck) with hotplug, glyphs and rebindable buttons
- **TRG launcher** that imports your disc image, selects the installed version, saves its settings, updates the port, and sends privacy-filtered bug reports
- **Seven languages**, video subtitles, restored English dialogue and corrected caption timings
- **Portable**: saves, settings and logs stay next to the program
- **Experimental native Linux and Steam Deck** AppImage

## Graphics

- **Renderers**: Direct3D 11 is the Windows default; Vulkan 1.2 is a native backend on Windows and Linux (the Linux default); Software is the game's own renderer and the fallback. The renderer is chosen at start and stays for the run. On Windows, D3D11 is the recommended path.
- **Native resolution** (Windows): 720p, 1080p, 1440p, 4K, Native and custom sizes; the 3D world, text and viewport use the render size, which does not depend on the window size. Menus and videos keep their 4:3 canvas, centred and scaled.
- **Field of view**: 60 to 110 degrees across the 4:3 view (75, the original, by default).
- **Texture filtering and anti-aliasing**: anisotropic filtering up to 16x, MSAA up to 8x where the card supports it, and generated mipmaps (the game's files are never changed).
- **Draw distance, LOD and terrain**: objects stay drawn 1.2 times as far and stay 3D models out to 25-150 m before becoming flat impostors (15 m in 1998); the terrain keeps its textures out to 750 m; textures and impostors use 256 pixels instead of the 1998 limit of 128; optional Maximum object detail.
- **Ambient occlusion** (Off to Ultra), **water refraction** and **enhanced sky**, as Direct3D 11 and Vulkan passes.
- **Texture cache and streaming**: texture memory grows from 24 MB to 96 MB, and uploads are timed precisely so new scenery gets its textures sooner.
- **HD front end, loading screens and videos** (video on Windows): rebuilt at the screen's resolution from the game's own art. **No HD texture pack is included**; the 3D world uses the original textures.
- **Window and presentation**: Windowed and Borderless Fullscreen (F11), monitor selection, Letterbox or Stretch, VSync on or off, FPS in the title and a frame counter (F6).

The Linux Vulkan build has its own filtering, MSAA, ambient occlusion, water, sky and HD front end, but not the Windows native-resolution and widescreen path. See [Linux and Steam Deck](#linux-and-steam-deck-experimental).

## Ultrawide

- **Widescreen (Hor+)**: 16:9, 16:10, 21:9 and 32:9 keep the vertical view of the original 4:3 camera and add world to the left and right, with no stretching and no holes at the edges.
- **True Ultrawide** is this Hor+ rendering at 21:9 and 32:9: a 21:9 screen shows more than 16:9, and 32:9 more than 21:9. It is supported on **Windows**.
- **Stretch is not ultrawide.** The launcher's Presentation setting has Letterbox (keeps the picture's shape, with bars) and Stretch (fills the window by stretching the finished picture). Stretch does not add any world to the sides; use Letterbox for correct geometry.
- Menus and dialogs stay 4:3 and are pillarboxed. There is no dedicated ultrawide HUD layout.
- On **Linux**, the guest stays at its 4:3 mode and is scaled to the window; native 4K and 21:9 / 32:9 gameplay are Windows-only in v0.1.0.

## Navigation

The minimap, compass, objective markers and waypoint were added by this port; the 1998 interface had none of them.

- **Minimap**: circular, bottom-left, player-up, fixed 2x zoom. Press **F5** (keyboard) or **L3 + R3** (gamepad) to toggle it. It can start on or off from the launcher's Gameplay page.
- **Creature markers**: red when the animal's brain is awake and engaged, green otherwise; the dot grows in four steps with the animal's size.
- **Vehicle markers**: ground vehicles such as jeeps, trucks and the Unimog appear as small orange rectangles.
- **Compass**: a strip at the top of the picture. The objective is a diamond with Anne's distance in metres, and an arrow when it is beyond the ends of the strip.
- **Waypoints and 3D markers**: the waypoint is **the current objective**, not a point you place. A diamond marks it in the 3D picture and becomes an edge arrow when it is off-screen. Where a level has stepping stones on the route, chevrons mark the next ones.
- **Objectives**: **F3** (or D-pad Right) lists them; **F4** (or D-pad Down) shows the current title under its distance for 4 seconds. Objectives follow the game's own hints in the eight campaign levels.
- Everything hides in menus, dialogs, videos and loading screens.

## Gameplay

- **Quick Save and Quick Load**: **F7** saves the level in progress to its own slot (a normal retail save, listed as "Quick Save"); **F8** loads it. Both keys are rebindable.
- **Level Select** (F10 Cheats), numbered in the game's order.
- **Dual Stow**, **Active AI** (6 creatures active instead of 3), **water refraction**, **enhanced sky**, **maximum object detail**, mouse sensitivity and restored footstep and collision sounds, from [Trespasser CE-inspired improvements](#trespasser-ce-inspired-improvements).
- **Physics, collision and stability fixes**: a subset of the official v1.1 build 117 patch, widescreen culling, clean shutdown and window presentation fixes ([Official Trespasser v1.1 build 117 fixes](#official-trespasser-v11-build-117-fixes)).
- **Gameplay options** in the launcher: Skip Intro, Video subtitles, Minimap, Dual stow, Active AI, Official Patch 1.1. Every option has an Original or Off choice that keeps the retail behaviour.

## Cheats

Press **F10** in a level or from the main menu.

| Cheat | Scope |
|---|---|
| Movement Speed (0.5x to 10x), Jump Height (0.5x to 5x), Invulnerability, Infinite Ammo, Freeze Dinosaurs, Super Health | Saved in the launcher's Cheats page as the starting value; F10 changes them for the running session only |
| Health (fill), Noclip, Stealth, Teleport, Level Select, Free Camera | Session only (F10), start Off in every run |

Free Camera and Noclip return to normal in another level or the main menu, and a save never keeps a cheat. The game's own console (Ctrl+F11) still works.

## Controls

- **Keyboard and mouse**: the game's own bindings are the defaults and are remapped in the game's Options > Controls screen. Mouse sensitivity (0.25 to 4.00) and the port's hotkeys are in the launcher.
- **Gamepads** (SDL3): Xbox, PlayStation, Switch and Steam Deck controllers, together with the keyboard and mouse, with hotplug.
- **Analog movement**, a layout built around Anne's arm, menu navigation, rebindable buttons, stick and trigger deadzones, look sensitivity and Y inversion.
- **Button glyphs** follow the device used last (Xbox, PlayStation, Steam Deck, Nintendo or generic); the tutorial prompts show the bound key or button.
- **F2** (or View) shows a controls card in a level. Rumble is not driven by any game event, so there is no vibration.

Details: [CONTROLS](docs/CONTROLS.md).

## Launcher

The TRG launcher opens before the game and keeps its settings in `user_data/launcher/trespasser.ini`.

| Page | What it does |
|---|---|
| Play | Game status, version selection, Import and Reinstall, Remove version |
| Display | Display mode, window size, monitor, aspect ratio, presentation |
| Graphics | Renderer, graphics card, render resolution, field of view, anti-aliasing, ambient occlusion, water, sky, texture filtering, VSync |
| Gameplay | Language, Minimap, Video subtitles, Skip Intro, Dual stow, Active AI, Official Patch 1.1 |
| Controls | Gamepad bindings and feel, mouse, hotkeys |
| Cheats | Starting values of the persistent cheats |
| Bug Report | A description and one privacy-filtered log, with a preview |
| About | Credits, folders, settings file, reset, versions |

It also imports a disc image (ISO), checks for updates ([Updates](docs/LAUNCHER.md#updates)), and keeps `game`, `user_data` and `logs` next to the program. See [LAUNCHER](docs/LAUNCHER.md) and [user data](docs/user-data.md).

## Gameplay details

### Trespasser CE-inspired improvements

Selected improvements were reimplemented natively in this port after studying Trespasser CE. The port does not include
Trespasser CE, does not need it, and does not read its executable, DLLs or `tp_mod.ini`: everything below is the port's
own code over the retail game (the graphics ones are Direct3D 11 passes; no DirectX 9 code). Every option has an Off
or Original choice that keeps the retail behaviour.

- **Quick Save / Quick Load**, always available: F7 saves the level in progress to its own slot (`SAVEGAME.QCK`, a
  normal retail save, listed as "Quick Save" in the Load screen), F8 loads it, in a level or from the main menu. Both
  keys can be changed in the launcher's Controls page.
- **Dual stow** (Gameplay > Trespasser CE): one item on the belt and one on the back with the hand free. Saves stay
  retail saves.
- **Active AI** (Gameplay > Trespasser CE): Enhanced lets 6 creatures be active at once instead of the retail 3, as
  CE's `MaxActiveAi`; Original keeps the retail limit. Saves stay retail saves.
- **Water refraction** (Graphics): the scene behind water is distorted by the game's own wave heights, after CE's
  refractive water, as a Direct3D 11 pass (a GPU copy or MSAA resolve of the scene and one shader; no CPU readback).
- **Enhanced sky** (Graphics): the sky fades into the level's fog colour towards the horizon by view direction with the
  level's own sky fog band, as CE's enhanced sky, in Direct3D 11 over the retail sky plane and texture.
- **Maximum object detail** (Graphics): every object keeps its most detailed model, as CE's `ForceMaxObjectDetail`.
- **Mouse sensitivity** (launcher, Controls > Keyboard & Mouse): one value for both axes, as CE's
  `MouseSensX`/`MouseSensY`.
- **Footstep and collision sounds** no longer go silent after loading a save or restarting a level (always on).

Credits: [Trespasser CE](https://www.trescomforum.org/viewtopic.php?t=10674) by Lee Arbuco;
[CE Patcher Large Version](https://www.trescom.org/download/ce-patcher-large-version/) by yochenhsieh, hosted by
TresCom; [Trespasser Mod Manager](https://github.com/miketheraptor/Trespasser-Mod-Manager) by MikeTheRaptor, used as a
reference for CE's configuration options.

### Official Trespasser v1.1 build 117 fixes

DreamWorks Interactive released an official v1.1 (build 117) patch in November 1998. The port runs the retail 1.0
build 116 and reimplements selected fixes from that patch natively, studied by comparing the two builds and their
level files. The patch does not need to be installed, and none of its files are included or read.

- **Patch 1.1 fixes** (Gameplay > Official Patch 1.1, On by default): the levels' terrain mesh tolerances
  (`TrrPixelTol` 8 to 10, `TrrPixelTolFar` 20 to 50, the patch's "Adjusted terrain texture resolution") are applied to
  the retail level scenes as they load, only when each file is the retail one. Off keeps the retail values. Saved games
  are not changed and stay retail saves.
- **Occasional object disappearance** (same setting): when an object moves, build 117 no longer lets the world place
  it inside a more detailed partition that the renderer skips at lower render qualities (`CPartition::bAddChild`).
- **Jump consistency**: a jump request stays pending until it fires or 25 physics steps fail to find ground, and a
  held jump key keeps asking, so a press between physics steps is no longer lost.
- **Object pickup, keypad interaction, centering aim**: build 117's hand: an empty hand rises toward the view and is
  tucked at the shoulder while something blocks the way, reaches 0.7 m instead of 0.8 m, can grab only once settled,
  holds a picked-up object 2 s instead of 1 s before pulling it in, and the view no longer chases a settling hand or
  recentres a held gun.
- **Interpenetration of compound objects, box-box interaction**: corners already inside a box are pushed out,
  contact normals use the rotation of compound sub-boxes, edge-edge contacts run, and pairs that ignore each other's
  collisions still wake each other.
- **Floating objects**: the official placements of 177 vegetation objects in IJ and IT and one rifle in AS, the ones
  that need none of the patch's rebuilt terrain or partitions.

Each fix follows the same setting; Off runs the retail code and data. What was left out and why is in
`docs/LAUNCHER.md`. Credits:
[Trespasser v1.1 (117) Full Patch](https://www.trescom.org/download/trespasser-patch-v1-1-117-full/) by DreamWorks
Interactive, hosted by TresCom.
## Localization

**Languages** are built into the port: Gameplay > Language in the launcher picks one of English, Brazilian Portuguese,
French, German, Spanish, Russian or Italian. Every language has the whole text of the game: game text, the words on the
menu pictures, subtitles, the tutorial and the port's own panels; the voices, effects, music and videos stay the English
originals. Brazilian Portuguese also translates the launcher, at once; the other languages keep it in English
([Languages](#languages)).

### Languages

Languages are built into the PC port: nothing is downloaded or installed. The translated text is part of the executable,
and the launcher's Gameplay page has one Language setting.

| Language | Game text, menus and subtitles | Voices and videos | Launcher |
|---|---|---|---|
| English | English | English | English |
| Brazilian Portuguese | Brazilian Portuguese | English | **Brazilian Portuguese** |
| French | French | English | English |
| German | German | English | English |
| Spanish | Spanish | English | English |
| Russian | Russian | English | English |
| Italian | Italian | English | English |

The port completes the localizations where the releases of 1998 and the fan translations stop: the official
translations have no subtitles, the Russian strings and the German ones miss a few entries, and there is no Italian game
text. Everything else (the subtitles of four languages, the Italian game text, the words on the menu pictures, the
controls card, the objectives, the debug and cheat menus) was translated for the port. Localized audio and videos are
not distributed: the voices stay the player's own English files.

The words on the menu pictures (NEW GAME, RESUME, CANCEL, the Quit and Sound titles, the legal lines of the main screen)
are taken out of the player's own pictures at run time and set again in the language, in the same stencil face and
colours, hover and pressed states included; no picture of the game is shipped. The tutorial's subtitles name the
controls by the button or key of the device in use in every language. The game's own Show Subtitles option still turns
subtitles on and off.

#### Translation credits

| Language | Content | Source | Translation | GameStrings conversion |
|---|---|---|---|---|
| Brazilian Portuguese | game text, menu wording | official | DreamWorks Interactive | Nick3069 |
| French | game text, menu wording | official | DreamWorks Interactive | Nick3069 |
| German | game text, menu wording | official | DreamWorks Interactive | Nick3069 |
| Spanish | game text, menu wording | official | DreamWorks Interactive | Nick3069 |
| Russian | game text and subtitles | community | cynodesmus | |
| Italian | subtitles (v1.0.0, timings adjusted by hand) | community | KeyofBlueS | |
| English | caption timings, restored dialogue and its audio ([below](#subtitle-timing-and-restored-dialogue)) | community | KeyofBlueS | |
| All | subtitles of Brazilian Portuguese, French, German and Spanish; Italian game text; missing German and Russian strings; the menus' words where a release had none; the translations of the restored dialogue; the port's own interface | PC port completion | Trespasser Recompiled | |

The port's own translations are not attributed to DreamWorks Interactive, cynodesmus or KeyofBlueS. Where an official
text had an evident defect it was corrected: the French strings have every "è" and "ê" swapped in the converted file,
and the Spanish OK button and the Brazilian Portuguese Credits button of the 1998 menus show the wrong word.

Source preservation and hosting: the [TresCom](https://www.trescom.org/) community, from whose download pages these
come ([Brazilian Portuguese](https://www.trescom.org/download/brazilian-portuguese-official-translation-audio/),
[French](https://www.trescom.org/download/french-official-translation-audio/),
[German](https://www.trescom.org/download/german-official-translation-audio/),
[Spanish](https://www.trescom.org/download/spanish-official-translation-audio/),
[Russian game strings](https://www.trescom.org/download/russian-unofficial-translation-game-strings/),
[Russian subtitles](https://www.trescom.org/download/russian-fan-translation-subtitles/),
[Italian subtitles](https://www.trescom.org/download/italian-fan-translation-subtitles/)). Only the text of those
packages is used: no audio, menu picture, video or program. The About page lists the same credits. Czech subtitles, the
Fargus and Lost Jungle Russian translations and the Italian cutscene packages are not included. The licence of these
downloads is not stated ([LEGAL](docs/LEGAL.md)).

### Subtitle timing and restored dialogue

The data in this section comes from the *English Stream TPA Fix (Subtitle Timing Fix)* by KeyofBlueS, preserved and hosted
by [TresCom](https://www.trescom.org/download/10423/) (March 14, 2026). Its author's description: a `Stream.tpa` with
adjusted caption timing, which also restores previously cut dialogue lines.

What the port took from it:

- **Corrected caption timings.** Of the 402 voice captions of the retail `Stream.tpa`, 328 are corrected (309 only in
  timing, 19 also in line breaks or text); with the 3 captions the retail file lacked for audio it has and the 5
  restored records the canonical table holds 410 samples; the port applies the corrected captions to the English game
  and gives every other language the same temporal events, so a caption appears and ends at the same moment in English,
  Brazilian Portuguese, French, German, Spanish, Russian and Italian. Where the fix splits, merges or adds a caption
  line, the translations were split, merged or given the same pause. The timings of the Russian release, which used
  placeholder durations, were replaced.
- **Restored English dialogue audio.** The retail files lack the audio of four voice lines that the retail levels call
  (`VA24` and `VA34` in the `jr` level, `VA208` in `as2`, the tour guide in `be`) and hold only a short
  version of the Ray computer voice (`VCRAYCOMPUTER`, 10.4 s against 33.8 s with its dialogue). These five records, 4,159,690
  bytes of uncompressed PCM exactly as the fix stores them, are compiled into the executable and presented to the game
  as part of `Stream.tpa` in memory. The player's own file is only read; the audio the retail file has is never replaced
  except `VCRAYCOMPUTER`, and only when its bytes are the known retail ones.
- **Captions the retail file lacked for audio it has** (the phone operator, a diary line, "At center court...").
- The restored lines stay English; only their subtitles are translated. All restored dialogue is translated in every
  supported language (the translations of the restored lines are the port's own, marked as PC port completion).

What it did not take: the modified `Stream.tpa` itself is not distributed and nothing is copied at start-up. No external
translation or audio package is needed, and nothing is downloaded. The fix holds 69 more samples the retail file lacks
(14.5 MB) that no level of the retail game triggers; they are not included because nothing can play them. A restored caption is shown only when
its audio exists, in the player's file or in the embedded records.

Size: restored audio 4,159,690 bytes, its manifest 1,272 bytes, caption and timing text 119,189 bytes (the canonical
English captions 43,827 bytes, the Russian completion 69,780 bytes, the other completions 5,582 bytes); about 4.28 MB in
the executable (about 3.5 MB more in a release archive).

The developer tool `tools/stream_fix_pack.cpp` made `assets/restored_audio` and `assets/translations/en.subtitles.txt`
from the fix and the retail file; it is not part of the build, and a clean clone builds without the fix.

### Video subtitles

Gameplay > **Video subtitles** (key `video_subtitles`, **On** by default) shows the spoken lines of the videos in the
language chosen in Gameplay > Language: English, Brazilian Portuguese, French, German, Italian, Russian or Spanish.
There is no separate language for the videos.

| Video | Dialogue | Cues |
|---|---|---|
| `tpassintro.smk` (opening) | Hammond's narration | 16 |
| `newgame.smk` (after New Game) | the answering machine and Jill's message | 9 |
| `win.smk` (ending) | the answering machine and Jill's last message | 7 |
| `credits.smk` | music only | none |

- The subtitles are drawn by the port over the picture at the window's resolution, in the face of the game's own
  caption font, white with a dark outline, centred near the bottom of the picture (not the window), on at most two
  lines when they fit. The video files, their pixels and their audio are never changed.
- Each cue follows the frame the game's Smacker player shows, so subtitles stop with the video, end with it and are
  never left on the next screen; Skip Intro still skips the video and its subtitles.
- Only the retail files get subtitles: each timeline is tied to the file's size and the SHA-256 of its first 64 KiB.
  A replaced video (a mod) plays without them, and the log says `unknown replacement video, subtitles skipped`. The
  Russian, Spanish and Brazilian Portuguese releases' dubbed videos each have their own timeline (the dubs speak on their
  own timing); the words come from the chosen language, never from the timeline. `tools/smk_inventory.py` lists the
  videos with their sizes and hashes and `tools/smk_speech.py` derives the dubs' cue frames from their audio.
- Works with Direct3D 11 (HD video presentation and the game's own) and with the Software renderer (drawn at the
  software picture's size).

The cue boundaries were measured frame by frame on the Italian-subtitled upscaled cutscenes by KeyofBlueS, used
only as a timing reference; their frames match the retail files one to one. No video, frame, picture or audio from
them is included, and the build does not need them. The English text of the narration is the game's own captions
and the English Stream TPA Fix's; Jill's lines follow the *Trespasser Voiceover List 3.0* by tatu (TresCom) and the
retail audio. The Italian lines are those of the Italian reference; the other languages reuse the port's existing
translations of the same captions where they exist, and the rest was translated for the port.

## Supported game versions

| Release | Version | Windows | Linux |
|---|---|---|---|
| English retail (UK `Trespass_UK_116`, US `Trespasser_116d`) | 1.0.116 | Yes | Yes |
| Russian Fargus (`TRESPASSER_116D`) | 1.0.116 | Yes | Yes |
| Spanish (`TRESPASSER_S`) | 1.1.117 | Yes | Import only |
| Brazilian Portuguese | 1.1.117 | Yes | Import only |

The disc is recognised by file sizes and SHA-256, never by its name. The original executables are import inputs only. No demo, mod or fan-made conversion is supported. See [GAME_VERSIONS](docs/GAME_VERSIONS.md).

## Windows installation

Windows 10 or 11, 64-bit. The Windows program is a 32-bit (x86) host that runs on 64-bit Windows.

1. Download **JurassicParkTrespasserRecompiled-v0.1.0-windows-x86.zip** from the [latest release](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/latest) and extract it into a writable folder.
2. Run **trespasser_recomp.exe**, keeping `trg-updater.exe` beside it.
3. On the **Play** page choose **Import game...** and select your own Trespasser `.iso` disc image. It is only read.
4. When Play says the game is ready, press **PLAY**.

The Windows executable is not code-signed, so SmartScreen may warn on the first start; choose **More info**, then **Run anyway** if you downloaded it from this repository's releases.

## Linux and Steam Deck (Experimental)

*Jurassic Park: Trespasser* was released for Windows in 1998. This recompilation also offers a native **x86_64 Linux executable**, distributed as an **AppImage**. It uses **no Wine and no Proton**.

- **Vulkan** is the Linux GPU renderer; Software is the fallback. If Vulkan cannot start, the run falls back to Software and says why.
- **AppImage**: make it executable and run it. Without FUSE it extracts itself and runs automatically (`APPIMAGE_EXTRACT_AND_RUN=1` forces that).
- **Import** your disc image from the launcher's Play page, as on Windows.
- **`game`, `user_data` and `logs`** live next to the AppImage file, never inside it.
- **Controllers** work through SDL3, including Steam Deck glyphs; the launcher is usable with a gamepad and has a built-in file browser when `zenity` or `kdialog` is missing.
- **Updates** replace the AppImage itself after checking its SHA-256, and restart it.
- **Requirements**: 64-bit x86 Linux with glibc 2.31 or newer, an X11 or Wayland desktop, and a Vulkan 1.1+ driver for the Vulkan renderer.

**Steam Deck** support is experimental and uses the same AppImage: add it with **Games > Add a Non-Steam Game to My Library**. It has **not been validated on Steam Deck hardware**, and it is not Steam Deck Verified. See [docs/LINUX.md](docs/LINUX.md) and [docs/LINUX_PARITY.md](docs/LINUX_PARITY.md).

Verified limits on Linux in v0.1.0:

- The guest keeps its 4:3 presentation, scaled to the window.
- True Ultrawide and HD video are Windows-only; Linux plays the original videos with subtitles.
- All four versions import, but only English and Russian (1.0.116) run; Spanish and Brazilian Portuguese are not playable yet.
- The Steam Deck, real desktops, GPUs and audio devices are not validated; the Linux tests run headless.

## Known limitations

- **No validated high frame rate.** VSync Off removes the present wait, but there is no uncapped-frame-rate mode and no 120 FPS guarantee.
- **"HD" means the front end, loading screens and videos**, not a texture pack; the 3D world uses the original textures.
- No dedicated ultrawide HUD, no HDR, no vibration, no mods, no custom waypoints.
- Windows Vulkan is selectable but D3D11 is recommended; the playable host is 32-bit (the 64-bit host is groundwork only).
- The Windows executable and updater are unsigned.

The complete list is in the [release notes](docs/RELEASE_NOTES_v0.1.0.md#known-issues-and-limitations).

## Bug reports

Use the launcher's **Bug Report** page: describe the problem, choose a log and press **Send report**; **Open preview** shows exactly what goes out, and nothing is sent until you confirm ([BUG_REPORTS](docs/BUG_REPORTS.md)). You can also open an [issue](https://github.com/saulob/jurassic-park-trespasser-recomp/issues) with the newest log from `logs`. For Linux problems add your distribution, GPU and driver, and on a Steam Deck whether you used Desktop or Gaming Mode.

## Downloads

Get the files from the [latest release](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/latest). The Windows port is x86; the Linux build is x86_64.

| Platform | File | Checksum |
|---|---|---|
| Windows 10 / 11 | [JurassicParkTrespasserRecompiled-v0.1.0-windows-x86.zip](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.0/JurassicParkTrespasserRecompiled-v0.1.0-windows-x86.zip) | [.sha256](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.0/JurassicParkTrespasserRecompiled-v0.1.0-windows-x86.zip.sha256) |
| Linux x86_64 / Steam Deck (experimental) | [JurassicParkTrespasserRecompiled-v0.1.0-x86_64.AppImage](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.0/JurassicParkTrespasserRecompiled-v0.1.0-x86_64.AppImage) | [.sha256](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.0/JurassicParkTrespasserRecompiled-v0.1.0-x86_64.AppImage.sha256) |

No game files are in the downloads.

## Security

The four v0.1.0 files (the Windows ZIP, `trespasser_recomp.exe`, `trg-updater.exe` and the Linux AppImage) were scanned on VirusTotal with no detections: [ZIP](https://www.virustotal.com/gui/file/4b677fb36519f63221ff25dffd5b5a50a250672db56658b07865810649bc1a36), [EXE](https://www.virustotal.com/gui/file/52b4dc10c9da024b1c10ef152615f1235d21301f9bcacf0d1d6f742f91c0d7fd), [updater](https://www.virustotal.com/gui/file/ba019e55a195d234c8f6b02e6f2bc2dc04aeb0df27ac3aa3570c29cb514c4be6), [AppImage](https://www.virustotal.com/gui/file/eb4e8d6308fa41407b503668e59536c387da654baf0ffa87bc88b2e4447856d9). A clean scan is an additional signal, not a guarantee of safety: verify the SHA-256 and download only from this repository. The full table is in the [release notes](docs/RELEASE_NOTES_v0.1.0.md#security-and-virustotal); see also [RELEASE_SECURITY](docs/RELEASE_SECURITY.md).

## How it works and building

### How it works

- The lifts were made from the retail builds (1.0.116 Pentium Pro, and the Spanish and Brazilian 117 builds) with
  [pcrecomp](https://github.com/sp00nznet/pcrecomp), which recovers the functions in `.text` and lifts them to C. They
  are project artifacts (`lifts\`); the original executables are development inputs only and are not required to
  install, build or play the recomp.
- The host is one 32-bit program, `trespasser_recomp.exe`, that carries every lift, maps the guest image the launcher
  installed from your disc at `0x00400000`, forwards its imports to the real Windows DLLs, and runs the lifted C of the
  selected game version in place of `.text`.
- `SelfMod`, the self-patching rasteriser section, runs as the original machine code.

[ARCHITECTURE](docs/ARCHITECTURE.md) lists exactly which parts are recompiled, which are
original code, and which are Windows.

### Quick start

```powershell
.\tools\build.ps1 -Run     # builds what is out of date, then opens the launcher and the game
.\tools\build.ps1          # builds only
```

Nothing that is already current is redone, so the same command is the normal way to build and to play.
`.\tools\run.ps1` is an optional shortcut that starts the existing build without checking for changes.

The build needs no game files. To play, import your own disc image from the launcher's Play page. Details, options and
what to do when a run stops: [BUILD](docs/BUILD.md).

### Layout

```
config/        profiles, guest maps, decoder fixups, extra entry points (lift development)
lifts/         the lifted builds (tar.xz) and their manifest
tools/         lift generation (maintainers), checks and build scripts
src/host/      the runtime host (hand-written)
src/recomp/gen* lifted C unpacked from lifts/ by CMake (ignored by Git)
launcher/      the launcher: pages, disc image reader, installer (C++)
assets/        launcher header art, sponsor heart, page icons and the translated text (assets/translations)
packaging/     third-party licence notices
src/common/    settings file and display arithmetic shared by host, launcher and tests
docs/          ANALYSIS, ARCHITECTURE, BUILD, CONTROLS, LAUNCHER, LEGAL, REFERENCES, user-data
external/      pcrecomp checkout (fetched, ignored by Git)
game/          your installed game files (ignored by Git)
user_data/     next to the program: settings and saves (never in a release;
               build\user_data for tools\build.ps1 -Run)
analysis/      regenerated analysis output (ignored by Git)
logs/          one log per run (ignored by Git)
```
### Testing

`.\tools\test.ps1` is the one command that validates the whole project on both platforms. It builds only the test
programs (incrementally), runs every Windows suite, then the Linux gate inside WSL (it finds the distribution, converts the
repository path with `wslpath` and runs `tools/test-linux.sh` itself: no WSL terminal, no `.sh` by hand), then compares the two
feature by feature against `tests\feature_contract.json`. It ends with `ALL TESTS PASSED` or a list of what failed (exit code 1):

```powershell
.\tools\test.ps1                    # Windows gate, Linux gate in WSL, Windows/Linux feature parity
.\tools\test.ps1 -WindowsOnly       # the Windows half only (-SkipLinux is the same); parity is then not evaluated
.\tools\test.ps1 -LinuxOnly         # the Linux half only
.\tools\test.ps1 -LinuxDistro JptFocalBuild   # the WSL distribution (JptFocalBuild, Ubuntu 20.04, is preferred by default)
.\tools\test.ps1 -Sanitize          # Linux-specific suites under AddressSanitizer + UndefinedBehaviorSanitizer (separate, slower)
.\tools\test.ps1 -List              # the sections and suites, nothing is built or run
.\tools\test.ps1 -Filter cheats     # only suites whose section, name or description contains the text
.\tools\test.ps1 -Detail            # also show the output of passing suites
.\tools\test.ps1 -Release           # also validate the release artifacts already in dist\ (never packages)
.\tools\test.ps1 -Full              # water_tests without --quick (about five minutes more)
```

It is headless: it never starts the game or the launcher, opens a window, reads a controller, plays audio or uses the
network (the Linux half runs on SDL's dummy drivers), and it works from any folder. Without WSL the Linux rows are BLOCKED, never
PASS, and the run fails. The feature parity lists every feature as global (the same on both platforms), Windows-only or
Linux-only, with a status per platform (PASS, FAIL, BLOCKED with the reason, PLATFORM_ONLY with the reason); a global feature that
differs fails the run. It checks that every test target, script and tool is wired in, that every
supported build, language, cheat and setting has its row, and that no suite changed the working tree. A suite that
needs game data that is not installed says so (`DATA UNAVAILABLE`, or a skipped part in the summary); it is never
reported as a pass. [Building](docs/BUILD.md#headless-tests) lists what each suite covers.

## Sponsoring

Jurassic Park: Trespasser PC is maintained and developed in my free time. If you enjoy the project or find it useful, consider supporting its continued development.

<p align="center">
<a href="https://github.com/sponsors/saulob" target="_blank"><img src="https://img.shields.io/badge/GitHub%20Sponsors-Sponsor-EA4AAA?style=for-the-badge&logo=githubsponsors&logoColor=white" alt="GitHub Sponsors"></a>
</p>

## Credits

- *Jurassic Park: Trespasser* by DreamWorks Interactive (1998). This is an unofficial fan project, not affiliated with or endorsed by DreamWorks, Universal or EA, and it contains no game files.
- Built on [sp00nznet/pcrecomp](https://github.com/sp00nznet/pcrecomp), following the reconnaissance of [sp00nznet/trespasser](https://github.com/sp00nznet/trespasser).
- Trespasser CE, CE Patcher Large Version, Trespasser Mod Manager and the official v1.1 patch: see the credits in [Trespasser CE-inspired improvements](#trespasser-ce-inspired-improvements) and [Official Trespasser v1.1 build 117 fixes](#official-trespasser-v11-build-117-fixes).
- Translations, subtitles and the restored dialogue: see [Translation credits](#translation-credits) and the [TresCom](https://www.trescom.org/) community.
- [TRG Launcher](https://github.com/TekRantGaming/trg-launcher), SDL3, Dear ImGui, Intel ASSAO, FFmpeg, OpenSSL and the other libraries listed in [docs/LEGAL.md](docs/LEGAL.md) and [docs/REFERENCES.md](docs/REFERENCES.md); their licences are in the `licenses` folder of the Windows ZIP.
- Everyone who tested the port and sent logs.

## License

See [docs/LEGAL.md](docs/LEGAL.md) for the legal notes, the third-party components and their licences ([THIRD-PARTY-LICENSES.txt](packaging/licenses/THIRD-PARTY-LICENSES.txt)). The game, its assets and the third-party translation packages are not covered by this project and are not included.
