# Jurassic Park: Trespasser Recompiled v0.1.3 - The Custom Levels Update

**v0.1.3 - The Custom Levels Update.** The launcher has a new Custom Levels page: import community levels and fan missions from a ZIP, 7z or RAR file or from a folder, see which ones the port can play before anything is installed, and play them with their own saves, a full restart, a generated minimap and objectives. It also adds support for many Trespasser CE and ATX features, a Squirrel interpreter for Trespasser CE scripts, fog, lighting and shadow improvements, renderer and launcher improvements, and Linux fixes.

**No game files are included.** You need your own legally obtained *Jurassic Park: Trespasser* disc image. The launcher imports it, verifies it and keeps your installation, saves and settings beside the program.

## Introduction

v0.1.3 is an update to [v0.1.2](https://github.com/saulob/jurassic-park-trespasser-recomp/blob/main/docs/RELEASE_NOTES_v0.1.2.md). Its main work is support for the levels the Trespasser community has made over the years. Many players today play mostly community levels, and this release answers their request for them.

Everything in the v0.1.0, v0.1.1 and v0.1.2 notes still applies, including the native builds, the three renderers, native resolutions and Hor+ widescreen on Windows, cheats, gamepad support, seven languages, the TRG launcher and all four supported game versions. Those features are not repeated here.

Windows remains the primary platform. The Linux AppImage remains experimental, and the Steam Deck has not been validated on physical hardware.

## Highlights

- **Custom Levels** with **ZIP, 7z, RAR and folder import**, checked level by level by an automated compatibility scanner before anything is installed.
- **Trespasser CE and ATX features**: most ActionTypes, CE textures and normal maps, custom skies, newer scene formats and larger engine limits for custom sessions. Some features are not implemented yet (see [Known Limitations](#known-limitations)).
- **Squirrel scripting**: the Squirrel interpreter and a compatibility layer that lets Trespasser CE scripts call into the game, with memory and instruction budgets.
- **Minimap and objectives** generated for custom levels.
- **Full restart and independent saves** for custom levels, kept apart from the campaign.
- **Fog, lighting and shadow improvements**: new Fog and Shadows options, stronger terrain shadows, better object shadows and a fixed Gamma range.
- **Ambient occlusion respects fog.**
- **Renderer improvements on Windows and Linux**: Trespasser CE textures and normal maps in Direct3D 11 and Vulkan, Vulkan water mipmaps, and water and sky through the native Direct3D bridge on Linux.
- **AI vocal memory corruption fixed** for levels with sounds past the original table.
- **Protection against malformed GRF world files.**
- **Launcher improvements**: a launcher that opens without graphics support, MSAA per graphics card, sidebar scrolling and a launch handoff window.
- **Linux mouse and pause menu fixed** (JPT-Y2CHNM).
- **Stability and compatibility**: atomic and validated saves, capacity checks before the engine's limits are reached, and builds that never mix stale lifted code.
- **Tested**: 259 passing results, 0 failures and 867,876 checks counted, with 143 of 143 global features at parity between Windows and Linux.

## Custom Levels

The launcher has a new **Custom Levels** page in its sidebar, on Windows and Linux.

1. Open the launcher.
2. Select **Custom Levels**.
3. Choose **Import ZIP / 7z / RAR** or **Import Folder** and select the level package.
4. Check the status of each level: **Ready**, or **Not supported** with the reason.
5. Select a Ready level and press **Play**.

### Levels you already have on disk

You do not need a ZIP, or to download a level again, if you already have it extracted. Choose **Import Folder** and select that folder.

- **Import Folder** reads your folder and installs a copy of the level's files in the launcher's own `user_data/custom_levels/<package>` folder. Your folder is not moved or changed.
- The port plays that **isolated copy**, so its saves, settings and checks stay apart from the campaign and from other packages. The copy uses its own disk space; **Delete** in the launcher removes it and keeps its saves.
- **Playing directly from an external folder**, without a copy, is not available in v0.1.3.

The game itself still has to be installed by the launcher from your disc image (ISO). Importing an existing Trespasser installation without an ISO is not supported in v0.1.3; it is being evaluated for a future version.

### Import

- **ZIP, 7z, RAR and folders.** The format is read from the file's contents, not its name. RAR 2.0 to 5.0 archives, solid or not, are read by the launcher's own decoder and checked against their CRC32 or BLAKE2sp.
- **Checked before installing.** A package is unpacked into a staging folder and analysed there. Each level is judged on its own: missing worlds, terrain or texture sets, damaged or unsupported archives, unsupported features and the instructions of its readme are reported per level, with the reason.
- **Partially compatible packages.** A package installs when at least one of its levels is Ready; the others stay listed as **Not supported**, and Play refuses them.
- **Safe extraction.** Absolute paths, `..`, reserved names, links and reparse points, oversized archives and too many files are refused. Programs and scripts for Windows (`.exe`, `.dll`, `.bat` and similar) are skipped and never run. A failed or cancelled import leaves nothing behind.
- **Malformed level files** are reported instead of being read: a world whose meshes point past their own points is refused as not supported, and the analysis finishes cleanly.
- **Isolated installation.** Levels go to `user_data/custom_levels/<package>`. Nothing in the installed game is added, replaced or changed.
- **Readme, Open Folder and Delete** for each package. Delete keeps the package's saves.
- Importing the same package again is recognised and does not install it twice.
- On Linux and the Steam Deck, the launcher's own file browser chooses the package when `zenity` or `kdialog` is not available.

### Playing

- **Independent saves.** Each package keeps its saves in `saves/custom_levels/<package>`. Campaign saves are never written by a custom level, and the campaign refuses a custom level's save.
- **Full level restart.** Restart Level reloads a custom level completely, scripts included.
- **Transitions** between the levels of a package work as in the original game. When a level points to a next level that is not in the package, you stay in the current level with a message instead of a crash.
- **Generated minimap and objectives** appear when the level has the information for them; otherwise the HUD says that there is no map or objective information.
- **Package settings** from `fm.ini`, `tp_mod.ini` and the level's own files apply to that session only. The campaign's settings are not changed.
- **Script keys.** A level whose Trespasser CE scripts read the USER commands gets keys for them: `script_user1_key` to `script_user8_key` in `trespasser.ini`, Shift and M for the first two by default. No other level and never the campaign has them.
- **Bug Report** includes the selected level's analysis, so a problem with a community level can be reported from the launcher.

### What READY means

**READY means that the automated compatibility scanner found no known blocking issues. It does not guarantee full gameplay compatibility.**

The scanner checks that the files a level needs are present and that the features, settings, ActionTypes and script functions it uses are ones the port implements. It does not play the level. A READY level can still have problems with scripts, triggers, graphics, performance or behaviour during play, and most READY levels have not been played from start to finish in the port.

The same rules apply to every level; no level or author has code of its own in the port.

The scanner was developed and checked against a local corpus of community levels. That corpus is under a provenance review ([LEGAL](https://github.com/saulob/jurassic-park-trespasser-recomp/blob/main/docs/LEGAL.md#community-levels)), so these notes no longer quote its counts as a measure of compatibility.

## Trespasser CE and ATX Compatibility

What is implemented:

- **ActionTypes** 100 to 106, 109 to 118, 120 and 121: game speed, map pictures, gun reloads, health, videos, hint text, colours, fixed speeds, controls, player AI, teleport, use, jump, particles, texture swaps and trigger rules. ActionType 99 (scripted actions) is not supported.
- **Textures**: BMP, TGA and DDS, including DXT1 to DXT5, with mipmaps and alpha; Trespasser CE's `texhw` texture folders; `.norm` normal maps.
- **Custom skies** (`.sky` files): dome, body and strip layers with the `OPAQUE`, `ALPHA`, `BURN`, `2X_ALPHA` and `2X_BURN` blend modes, drawn by Direct3D 11 and Vulkan. Stars and meteors layers are not supported.
- **Terrain**: `TerrainBackfaceCullBias`.
- **Scenes**: scene formats 23 to 26 and `hint_strings` text.
- **ATX**: map pictures and the `MAPS` folder.
- **Engine limits**: larger heaps, texture pools and mesh limits, applied only to custom level sessions and sized from the level's own content, with capacity checks before the engine's limits are reached. The campaign keeps the original limits.

## Scripting

Trespasser CE levels can carry scripts written in Squirrel. The port embeds the Squirrel 3.2 interpreter and implements a compatibility layer, following Trespasser CE's scripting documentation, that lets those scripts call functions of the game engine. Scripts run in custom levels only: the campaign never starts a script machine.

What the port implements:

- **One script machine per level**, with a memory budget (16 MB by default) and an instruction budget for every callback. A script that fails, runs out of its budget or raises an error is stopped on its own and logged; the rest of the level goes on.
- **No file, operating system or thread access** is exposed to scripts. This limits what a script can reach; it is not a promise that every faulty or hostile script is prevented from slowing down or stopping the game.
- **Lifecycle**: constructors and destructors, `OnScriptInit`, `OnGameStart`, `OnLevelStart`, `OnLevelEnd` at transitions, `OnSimStep`, `OnPrepareSave`, `OnPostLoad`, `OnWorldPurge`, `OnMessage`, `OnPaintHW3D`, `OnHandleInput`, modal input, `OnActBite`, `OnJump` and the player's speed and jump factors.
- **Saves**: each script's `nvram` is saved with the game and restored on load, restart and transition, as in Trespasser CE.
- **Engine functions**: world objects, animals, the player and guns (`CInstance`, `CAnimal`, `CPlayer`, `CGun`), `Vector3` and `Rotate3`, teleport and fixed velocity, messages, the player's voice, health, ammunition, head position and rotation, drawing textures on screen, and the USER keys.
- **Not yet implemented**: part of Trespasser CE's documented scripting API, including action scripts (ActionType 99) and the `OnLevelTransitionRequested` and `OnGameCompleted` callbacks. A level whose scripts use something missing is listed as Not supported, with the missing names.

How script support has been checked:

- **Static analysis**: the launcher reads a level's scripts without running them and checks every Trespasser CE function and callback they use against the list the port implements. Passing this check means the names exist in the port, not that the level's script logic behaves as intended.
- **Synthetic tests**: headless tests run small scripts written for the project over a model of the game world, covering each implemented function and callback.
- **Real gameplay**: no scripted community level has been confirmed in a full manual play-through in the port yet.

## Graphics

- **Fog** and **Shadows** options in the launcher's Graphics page, On by default: the original game's distance fog and its shadows of objects and dinosaurs, in every renderer.
- **Lighting and shadows**: terrain shadows have stronger contrast at the default brightness, object shadows and the shadows of see-through plants are improved, and Gamma levels 8 and 9 no longer remove the shadows (the engine's own table ends at level 7, which is now the highest level used).
- **Ambient occlusion respects fog**, so distant objects no longer show occlusion through the haze.
- **Windows renderers**: Trespasser CE textures and normal maps drawn by Direct3D 11 and Vulkan.
- **Linux renderer**: the game's Direct3D drawing, including water and sky, now goes through a native Direct3D bridge onto Vulkan, and Vulkan water mipmaps are fixed.

## Launcher

- The **Custom Levels** page, with import progress, per-level status and reasons, Readme, Open Folder, Delete and Play.
- **The launcher opens without graphics support.** Each renderer is tried in turn and the launcher falls back to software drawing; `--launcher-renderer software` forces it for one run. A graphics card list that cannot be read leaves the Graphics page usable.
- **MSAA levels follow the chosen graphics card.**
- **Sidebar scrolling** with keyboard and controller navigation of the page rows.
- **Launch handoff**: a window in the launcher's place shows the start-up phases until the game's window is ready, and the game window no longer shows a white rectangle before its first frame.
- **Bug Report** carries the analysis of the selected custom level.

## Linux and Steam Deck

- **Menu cursor fixed** (JPT-Y2CHNM): the cursor no longer freezes in menus and dialogs after gameplay, moves correctly between relative and absolute mouse modes, follows the window after a resize, and takes the real pointer's position when the window regains it.
- **Pause menu**: Esc closes the in-game options menu as on Windows, instead of reopening it at once.
- **Custom Levels** work on Linux with the same rules, imports and saves as on Windows.
- **Packaging**: the application ID is read correctly from source files with Windows line endings, the native ELF is checked for Windows library names, and the glibc baseline stays at 2.31.

The Linux mouse and pause menu fixes were validated by headless tests; they have not been confirmed on a physical Linux desktop yet. Linux and the Steam Deck remain experimental.

## Fixes

- **AI vocal memory corruption** in custom levels: an animal sound index outside the original table is now refused instead of reading past it.
- **Malformed GRF world files** with mesh indices past their points are refused instead of being read out of bounds, in the launcher and in the game.
- **Atomic saves**: a save is written to a temporary file and replaces the previous one only when it is complete, so a failed save keeps the previous one.
- **Save validation** on load, for the campaign and for custom levels: damaged saves are recognised and not loaded.
- **Stale builds**: lifted code archives update only the files whose content changed, so a build never mixes old and new lifted code.

## Supported Game Versions

| Release | Version | Windows | Linux |
|---|---|---|---|
| English retail (UK `Trespass_UK_116`, US `Trespasser_116d`) | 1.0.116 | Yes | Yes |
| Russian Fargus (`TRESPASSER_116D`) | 1.0.116 | Yes | Yes |
| Spanish (`TRESPASSER_S`) | 1.1.117 | Yes | Yes |
| Brazilian Portuguese | 1.1.117 | Yes | Yes |

Custom levels play with the installed game version selected on the Play page.

## Compatibility

- **Windows 10 and 11 (x86)** is the primary platform, with the native high-resolution, Hor+ widescreen and Direct3D 11 rendering paths.
- **Linux x86_64 and Steam Deck** run the experimental AppImage with the Vulkan or software renderer. Gameplay stays in the 4:3 mode, scaled to the window.
- The AppImage targets Ubuntu 20.04 and a glibc baseline of 2.31.
- Settings and saves written by v0.1.2 are read unchanged. The new **Fog** and **Shadows** settings default to On.

## Known Limitations

### Custom level features not implemented

A level that uses one of these is listed as **Not supported**, with the reason:

- **Trespasser CE settings**: extended AI (`EnableAiActivityEx`), `MaxAiActivateOnRenderDist`, wood sap particles (`EnableWoodSapParticles`) and a custom end screen (`EndGameDDF`).
- **Trespasser CE textures**: `texhw` textures on meshes the original game bump-maps, and `texhw` textures whose sides are not powers of two.
- **Custom skies**: stars and meteors layers.
- **Scripted actions**: ActionType 99 and its action scripts (`ScriptAT`).
- **Script callbacks and functions**: `OnLevelTransitionRequested`, `OnGameCompleted`, `OnLevelEnd` outside a level transition, and the parts of Trespasser CE's scripting API the port does not have yet.
- **ATX features that Trespasser CE does not have either**, for a package that says it needs ATX: animals that jump as they bite (`MAPS/JMP`), animals that throw objects (`MAPS/SPT`), ATX plugins (`MAPS/PLG`) and rules changed in `ATXconfig.ini`.

Present in the port but with limits:

- **`OnConsoleCommand`** is implemented, but the game has no console, so no command reaches it in play. A level that defines it is not refused.
- **Replacement menus, menu sounds and opening or ending videos** of a package are kept but not used: the launcher starts each level directly.
- **ATX options that are off by default** (its HUD pictures, randomizer, night mode, rain and menu texts) are not used.

### Common reasons a level is not READY

- Files missing from the download or an incomplete package: a world, a terrain or a texture set absent, or a patch without its base level.
- One of the features listed above, for example extended AI (`EnableAiActivityEx`), `EndGameDDF` or `texhw` textures on bump-mapped meshes.

### Other limitations

- **READY is not a play-through.** It is the result of an automated scan; most custom levels have not been played from start to finish in the port.
- **RAR archives** that are encrypted, split into volumes, made with RAR 1.5 or use a dictionary over 256 MB are refused: extract them and use **Import Folder**.
- **Importing an existing Trespasser installation** without a disc image, and **playing a level directly from an external folder** without a copy, are not available.
- **Linux mouse and pause menu fixes** were validated by headless tests, not yet on a physical Linux desktop.
- **Windows is the primary platform.** Linux is experimental, and the Steam Deck has not been validated on physical hardware.
- **Linux does not have every Windows display feature.** Native high resolutions and true 21:9 / 32:9 Hor+ gameplay remain Windows-only.
- **Unsigned Windows executables.** SmartScreen may warn on first launch.
- All other limitations in the [v0.1.2 notes](https://github.com/saulob/jurassic-park-trespasser-recomp/blob/main/docs/RELEASE_NOTES_v0.1.2.md#known-limitations) and earlier notes still apply.

## Testing

- The full headless gate passed on Windows and Linux: **259 passing results, 0 failures and 867,876 checks counted**, with **143 of 143 global features at parity between Windows and Linux** in the 194-feature contract. Some optional fixtures were skipped, as the gate report lists.
- The Windows and Linux builds passed, and the Linux executable needs at most glibc 2.30, within the 2.31 baseline.
- Custom level import, analysis, sessions, saves, restart and the script machine are covered by headless suites, and the TresCom download catalog was analysed with the release scanner.
- The final Windows ZIP and Linux AppImage were checked by the project's release validators.
- Real GPU rendering, audio, controllers, the Steam Deck and full play-throughs of custom levels still need manual testing; headless tests are not a substitute for it.

## Credits and Acknowledgements

Thanks to the TresCom community, whose request for custom level support started this release, and to everyone who has made, kept and shared Trespasser levels over the years. The TresCom download catalog was the corpus every compatibility rule was checked against. Thanks also to everyone who tested v0.1.2 and sent reports. The full credits are in the [v0.1.0 notes](https://github.com/saulob/jurassic-park-trespasser-recomp/blob/main/docs/RELEASE_NOTES_v0.1.0.md#credits-and-acknowledgements).

## Downloads

| Platform | File | Checksum file |
|---|---|---|
| Windows 10 / 11 (x86) | [`JurassicParkTrespasserRecompiled-v0.1.3-windows-x86.zip`](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.3/JurassicParkTrespasserRecompiled-v0.1.3-windows-x86.zip) (30,465,751 bytes) | [`JurassicParkTrespasserRecompiled-v0.1.3-windows-x86.zip.sha256`](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.3/JurassicParkTrespasserRecompiled-v0.1.3-windows-x86.zip.sha256) |
| Linux x86_64 / Steam Deck (experimental) | [`JurassicParkTrespasserRecompiled-v0.1.3-x86_64.AppImage`](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.3/JurassicParkTrespasserRecompiled-v0.1.3-x86_64.AppImage) (50,571,584 bytes) | [`JurassicParkTrespasserRecompiled-v0.1.3-x86_64.AppImage.sha256`](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.3/JurassicParkTrespasserRecompiled-v0.1.3-x86_64.AppImage.sha256) |

Debug symbols, development builds, game data, community levels, saves and logs are not published.

## Checksums

The two `.sha256` files published with the release contain:

```
c3add7e4a9b07367b7b77802fa8e9587428c9984d16531615a81102ae22d0293  JurassicParkTrespasserRecompiled-v0.1.3-windows-x86.zip
463439827f0ef08d5a1a4becedd3cd1a38bf2134ecc9572cc56ae0dd354be3c5  JurassicParkTrespasserRecompiled-v0.1.3-x86_64.AppImage
```

Inside the Windows ZIP:

| File | SHA-256 |
|---|---|
| `trespasser_recomp.exe` | `b17a1dfa1c9465edb9afc8189599c1991f17fbaa496e5a89c51a744434962e23` |
| `trg-updater.exe` | `f8a985639a26e259a4439c0a827df4fcfa3bb316fb72298ce2380fa03e1b26b5` |

## Security

The four final files were checked manually on VirusTotal before publication, and the project maintainer reported OK for all four. The links below are the reports for the exact file hashes.

VirusTotal is an additional verification step, not an absolute guarantee of safety. Verify the SHA-256 values before running a file, and download only from this repository's Releases page.

| File | SHA-256 | VirusTotal |
|---|---|---|
| Windows ZIP: `JurassicParkTrespasserRecompiled-v0.1.3-windows-x86.zip` | `c3add7e4a9b07367b7b77802fa8e9587428c9984d16531615a81102ae22d0293` | [VirusTotal](https://www.virustotal.com/gui/file/c3add7e4a9b07367b7b77802fa8e9587428c9984d16531615a81102ae22d0293?nocache=1) |
| Linux AppImage: `JurassicParkTrespasserRecompiled-v0.1.3-x86_64.AppImage` | `463439827f0ef08d5a1a4becedd3cd1a38bf2134ecc9572cc56ae0dd354be3c5` | [VirusTotal](https://www.virustotal.com/gui/file/463439827f0ef08d5a1a4becedd3cd1a38bf2134ecc9572cc56ae0dd354be3c5?nocache=1) |
| Windows program: `trespasser_recomp.exe` (inside the ZIP) | `b17a1dfa1c9465edb9afc8189599c1991f17fbaa496e5a89c51a744434962e23` | [VirusTotal](https://www.virustotal.com/gui/file/b17a1dfa1c9465edb9afc8189599c1991f17fbaa496e5a89c51a744434962e23?nocache=1) |
| Windows updater: `trg-updater.exe` (inside the ZIP) | `f8a985639a26e259a4439c0a827df4fcfa3bb316fb72298ce2380fa03e1b26b5` | [VirusTotal](https://www.virustotal.com/gui/file/f8a985639a26e259a4439c0a827df4fcfa3bb316fb72298ce2380fa03e1b26b5?nocache=1) |

The project does not distribute commercial game files or community levels. You must supply your own compatible disc image.

## About this revision

These notes were revised after publication to describe custom level support more precisely: what READY means, which Trespasser CE features are implemented and which are not, how script support has been checked, and how to import a level you already have on disk. The release files, their checksums and the VirusTotal reports above are unchanged.

This project is developed with AI coding assistance, and custom level compatibility was checked with an automated scanner run over community levels on the maintainer's machine. Some of those files were processed in AI-assisted sessions, including parts of the scripts of one community level whose author does not permit the use of their work with generative AI tools. The script tests modelled on that level's behaviour have been replaced in the repository by independent tests written from Trespasser CE's documentation; the earlier tests remain in the Git history and the v0.1.3 source archives. No community level file is in the release downloads.

The project's [provenance policy](https://github.com/saulob/jurassic-park-trespasser-recomp/blob/main/docs/PROVENANCE.md) was revised: material whose author does not permit AI use, and material whose authorship is uncertain, is kept out of AI-assisted work, and the tools of the local development corpus skip excluded packages before downloading, extracting or scanning them. These changes cannot withdraw what is already in the Git history, in published source archives or in data already processed by external services. Details are in [LEGAL](https://github.com/saulob/jurassic-park-trespasser-recomp/blob/main/docs/LEGAL.md#community-levels).
