# Jurassic Park: Trespasser Recompiled v0.1.4 - Compatibility and Quality of Life Update

**v0.1.4 - Compatibility and Quality of Life Update.** Install the game from the copy you already have on disk, without an ISO. Play custom levels straight from their own folders, including Trespasser CE mod folders. Levels that use features the port does not have yet can now be started on request, after a clear warning. This release also adds a Nearest (Unfiltered) texture filter and several fixes.

**No game files are included.** You need your own legally obtained copy of *Jurassic Park: Trespasser*: a disc image, a copy of the disc, or an existing installation. The launcher reads it, verifies it and installs the game beside the program.

## Introduction

v0.1.4 is an update to [v0.1.3](https://github.com/saulob/jurassic-park-trespasser-recomp/blob/main/docs/RELEASE_NOTES_v0.1.3.md). Everything in the earlier notes still applies and is not repeated here.

Windows remains the primary platform. The Linux AppImage remains experimental, and the Steam Deck has not been validated on physical hardware.

## Highlights

- **Import Existing Installation**: install the game from an existing installation or an extracted copy of the disc. No ISO is needed when the required original files are there.
- **Add the Disc's Folder**: an incomplete or patched installation can be completed with a folder copy of the original disc.
- **Link External Folder**: play custom levels where they already are, without copying them.
- **Trespasser CE mod folders**: each fan mission of a `tp_mod.ini` tree becomes its own entry.
- **Limited Compatibility and Play Anyway**: levels that use features the port does not have yet can be started after a warning.
- **Nearest (Unfiltered)** texture filtering for Direct3D 11 and Vulkan.
- **Objective notifications follow the Waypoints setting.**
- **Bug reports no longer include the text of custom level readmes or other documents.**

## Installation

The **Play** page now has two ways to install the game:

- **Import Disc Image...**: your own `.iso` disc image, as before.
- **Import Existing Installation...**: a folder with the game you already have. This can be a copy of the disc, or an installed game.

How it works:

- The version is recognised from the files themselves (sizes and SHA-256), never from file or folder names. All four supported versions can be installed this way.
- Every file must match the original retail file. A file that was patched or modified, for example by Patch 1.1 or Trespasser CE, is never used.
- When files are missing, the launcher names them. **Add the Disc's Folder...** then completes the installation from a folder copy of the original disc. This covers installations that keep their levels on the CD, installations patched to 1.1, and installations with Trespasser CE copied over them.
- Your folders are **only read**. Nothing in them is changed, moved or deleted, and no program, installer or `Autorun.inf` in them is ever run.
- The installation uses the same safe steps as a disc image: a free space check, a staging folder, verification of every file, and rollback if anything fails or you press Cancel. Your installed versions stay as they were until the new one is complete.

## Custom Levels

### Play directly from external folders

**Link External Folder** registers levels where they already are, without copying them. You can choose one level, a fan mission, or a folder that holds several of them.

- The linked folder is **only read**. The launcher keeps its own records in `user_data\custom_levels`, and saves stay in `saves\custom_levels`, apart from the campaign.
- Links are remembered between sessions.
- **Rescan** reads a linked folder again after you change it.
- If you move or rename a linked folder, it shows as **Folder unavailable**. **Relocate Folder** finds it again, and its saves stay.
- **Unlink** removes the link and the launcher's own records. **It never deletes anything in your folder**, and the level's saves are kept.
- Archives (ZIP, 7z, RAR) inside a linked folder are not linked. Import them with **Import ZIP / 7z / RAR**.
- **Import ZIP / 7z / RAR** and **Import Folder** work as before.

### Trespasser CE mod folders

Link a Trespasser CE folder with `tp_mod.ini`, and each fan mission in its `FMPath` becomes its own entry. Files are found in Trespasser CE's order, using the shared `UberModPaths` and `ModPaths` folders, and nothing has to be moved, renamed or repacked. Not every mod is supported: each mission goes through the same checks as any other level.

### Ready, Limited Compatibility and Invalid / Unsafe

Every level now shows one of three states, with its reasons in **Compatibility details**:

| State | Meaning | Button |
|---|---|---|
| **Ready** (green) | the automated check found no known problem | **Play** |
| **Limited Compatibility** (yellow) | the level uses something the port does not have yet, such as some Trespasser CE script functions or ATX options, and the game can leave it out | **Play Anyway** |
| **Invalid / Unsafe** (red) | files are missing or damaged, the format or size is beyond what the engine can take, or a path is unsafe | none |

- **Play Anyway** opens a **Compatibility Warning** that lists the reasons. **Cancel** is the default choice, also with Escape or a controller's B button.
- Your answer applies **to that start only**. Nothing is saved, and the next start asks again.
- One confirmation also covers the Limited Compatibility levels that the chosen level leads to. Other levels of the package that were not confirmed are not loaded during that session, and neither are saves from them.
- Before the game starts, the level is checked again if any of its files changed. A level that became Invalid / Unsafe is not started, and one whose reasons changed asks again.
- **Invalid / Unsafe levels are never started**, by the launcher or by the game.
- A package is installed or linked when at least one of its levels is Ready or Limited Compatibility.

Levels played with Play Anyway may show graphical problems, broken or missing scripts, gameplay issues or crashes. The port leaves out what it does not have, and the level was not made for that.

**Ready is not a play-through.** It means that the automated check found no known blocking problem. Most levels have not been played from start to finish in the port.

Packages, links, saves and settings from v0.1.3 are kept. Each package is checked again with the new rules the first time the page lists it; a level that v0.1.3 called Not supported becomes Limited Compatibility only when that check allows it.

### Compatibility reports

A level that is not Ready has its own **Bug Report** button. It adds the level's compatibility reasons to a report that you can preview and choose to send. Nothing is sent automatically.

## Graphics

- **Nearest (Unfiltered)** in **Graphics > Texture filtering**: the 3D world's textures stay sharp and pixelated, picking the nearest mipmap level with no blending, in Direct3D 11 and Vulkan on Windows and in Vulkan on Linux.
- Every other graphics improvement stays independent and keeps working: render resolution, anti-aliasing, ambient occlusion, water refraction, enhanced sky, fog and shadows. The menus, videos, fonts and the port's own interface keep their own filtering.
- The Software renderer draws textures itself, so the option is greyed out there.
- **Anisotropic 16x** remains the default, and a texture filter you chose in v0.1.3 stays exactly as it was.

## Fixes

- **Objective notifications follow the Waypoints setting.** With Waypoints off, the automatic "Objective updated" message no longer appears. Objectives still progress normally. F1 Hint, F3 Objectives and F4 Show Objective still work when you ask for them, and the game's own messages, Quick Save and minimap notices are unchanged.
- **Bug reports and logs no longer include custom level texts.** The import log used to copy the full text of readmes, walkthroughs and other documents in a package, and quoted readme sentences as reasons. The log now lists each document by name, size and SHA-256 only, and a reason names the readme and the sentence number. The **Readme** button still opens the document on your computer.
- **Older logs are cleaned before sending.** When you send a log written by v0.1.3, Bug Report leaves out the copied document text and the quoted sentences, in the preview and in what is sent. Your log files on disk are not changed or deleted.
- **Safer game import.** The launcher refuses to read the game from its own temporary or backup installation folders, which it empties during an install.
- **Script keys.** The default keys for the Trespasser CE USER commands are now **1 to 8**. In v0.1.3 they were Shift and M for the first two, and Shift is also the wrist rotation and Dual Stow modifier. A key you set yourself in `trespasser.ini` (`script_user1_key` to `script_user8_key`) is kept.

## Development tools

These changes concern the project's own tools and do not affect the game:

- The Bug Report service's deployment script uses a **pinned local Wrangler** (version 4.160.1, locked in `package-lock.json`) through Node.js. It never uses `npx` or a global Wrangler, and it returns Wrangler's exit code. Node.js 22.12 or newer is required.
- The development compatibility tools report **Ready, Limited Compatibility and Invalid / Unsafe**, the same states the launcher uses. A failed download or extraction is never reported as a compatibility state.
- The Bug Report service documentation now describes the service as it is deployed.

## Supported Game Versions

| Release | Version | Windows | Linux |
|---|---|---|---|
| English retail (UK `Trespass_UK_116`, US `Trespasser_116d`) | 1.0.116 | Yes | Yes |
| Russian Fargus (`TRESPASSER_116D`) | 1.0.116 | Yes | Yes |
| Spanish (`TRESPASSER_S`) | 1.1.117 | Yes | Yes |
| Brazilian Portuguese | 1.1.117 | Yes | Yes |

Each version can be installed from its disc image or from an existing installation or copy of its disc.

## Compatibility

- **Windows 10 and 11 (x86)** is the primary platform.
- **Linux x86_64 and Steam Deck** run the experimental AppImage with the Vulkan or software renderer. The AppImage keeps its glibc 2.31 baseline (Ubuntu 20.04): the executable needs at most glibc 2.30.
- Settings, saves, custom level packages and their saves from v0.1.3 are kept. Custom level packages are checked again once with the new rules.

## Known Limitations

### Installation

- **Patched and modified files are never used.** An installation patched to 1.1, or with Trespasser CE copied over it, needs a folder copy of the original disc to complete it.
- **Trespasser builds the port does not support** are refused, and the log lists their file fingerprints.

### Custom levels

- **Not every community level is supported**, and Ready is the result of an automated check, not a play-through.
- **Limited Compatibility levels play without the features the port lacks.** Expect missing effects, broken scripts, gameplay problems or crashes.
- **Not implemented yet**, so a level that needs them is Limited Compatibility: Trespasser CE's extended AI (`EnableAiActivityEx`), `MaxAiActivateOnRenderDist`, wood sap particles, `EndGameDDF`, `texhw` textures on meshes the original game bump-maps and `texhw` textures whose sides are not powers of two, stars and meteors sky layers, scripted actions (ActionType 99), the `OnLevelTransitionRequested` and `OnGameCompleted` script callbacks and other parts of Trespasser CE's scripting API, and ATX jumps, projectiles, plugins and `ATXconfig.ini` rules.
- Not every Trespasser CE or ATX feature is supported, and no level is promised to work in full.
- **A linked folder must stay where it is.** On Windows, its path must be one the system's code page can spell, with files at most 250 characters deep; otherwise use **Import Folder**.
- **A linked folder is read while you play.** The launcher checks the level again before each start, but changing the folder's files while a level is running is not supported.
- **RAR archives** that are encrypted, split into volumes, made with RAR 1.5 or use a dictionary over 256 MB are refused: extract them and use **Import Folder**.

### Linux and Steam Deck

- **The Linux mouse and menu problems reported by players have not been confirmed fixed on physical Linux hardware.** The fixes so far were checked by headless tests only.
- Linux and the Steam Deck remain experimental, and the Steam Deck has not been validated on physical hardware.
- Native high resolutions and true 21:9 / 32:9 gameplay remain Windows-only.

### Other

- **Unsigned Windows executables.** SmartScreen may warn on first launch.
- All other limitations in the [v0.1.3 notes](https://github.com/saulob/jurassic-park-trespasser-recomp/blob/main/docs/RELEASE_NOTES_v0.1.3.md#known-limitations) and earlier notes still apply.

## Testing

- Installation from folders, external folders, Trespasser CE mod folders, the three compatibility states, Play Anyway, sessions, saves, restarts and transitions, the compatibility tools, Nearest filtering and the objective notifications are covered by headless test suites on Windows and Linux.
- **Full headless gate (Windows and Linux):** 255 passing results and 3 failures, with 853,281 checks counted and no compiler warnings. The three failures had one cause: the generated Linux feature parity inventory (`docs/LINUX_PARITY.md`) was out of date. It was regenerated, and the parity test then passed on Windows and on Linux (29 tests each). The full Windows gate was not run again after that fix.
- **Full Linux gate after the fix:** 123 passing results, 0 failures and 74,742 checks counted, with no compiler warnings, on Ubuntu 20.04 (glibc 2.31) with GCC 13.1.0. The executable needs at most glibc 2.30; the self-check found all 21 runtime capabilities and 29 of 29 lift hooks; all four supported game versions passed.
- **Release builds:** the Windows x86 Release (clang-cl) and the Linux x86_64 Release (GCC 13.1.0, Ubuntu 20.04) built with no warnings. The Linux Release executable passed its own headless gate before packaging (120 passing results, 0 failures); the suites that need game data or a real GPU reported them as unavailable.
- **Release validators:** the final Windows ZIP and Linux AppImage passed the project's release checks: contents, versions, architectures, licences, checksums, the AppImage runtime and a headless start of the AppImage in every start mode.
- Gameplay, real GPU rendering, audio, controllers, Linux desktops, the Steam Deck and full play-throughs of custom levels need manual testing; headless tests are not a substitute for it. Vulkan on a real Linux GPU and the Steam Deck were not tested for this release.

## Credits and Acknowledgements

Thanks to everyone who tested v0.1.3, reported problems and asked for these features, and to the people who have made and kept Trespasser levels over the years. The full credits are in the [v0.1.0 notes](https://github.com/saulob/jurassic-park-trespasser-recomp/blob/main/docs/RELEASE_NOTES_v0.1.0.md#credits-and-acknowledgements).

## Downloads

| Platform | File | Checksum file |
|---|---|---|
| Windows 10 / 11 (x86) | [`JurassicParkTrespasserRecompiled-v0.1.4-windows-x86.zip`](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.4/JurassicParkTrespasserRecompiled-v0.1.4-windows-x86.zip) (30,596,476 bytes) | [`JurassicParkTrespasserRecompiled-v0.1.4-windows-x86.zip.sha256`](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.4/JurassicParkTrespasserRecompiled-v0.1.4-windows-x86.zip.sha256) |
| Linux x86_64 / Steam Deck (experimental) | [`JurassicParkTrespasserRecompiled-v0.1.4-x86_64.AppImage`](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.4/JurassicParkTrespasserRecompiled-v0.1.4-x86_64.AppImage) (50,567,488 bytes) | [`JurassicParkTrespasserRecompiled-v0.1.4-x86_64.AppImage.sha256`](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.4/JurassicParkTrespasserRecompiled-v0.1.4-x86_64.AppImage.sha256) |

Debug symbols, development builds, game data, community levels, saves and logs are not published.

## Checksums

The two `.sha256` files published with the release contain:

```
e2027381cc637d1e9fa43d781beb5bf80df9e8531a1ebba80338efb85fffca0d  JurassicParkTrespasserRecompiled-v0.1.4-windows-x86.zip
7a26c2e4d44df51bb9bcfbb5f267704d950ae0bf80553654e4b4ef6bd2956095  JurassicParkTrespasserRecompiled-v0.1.4-x86_64.AppImage
```

Inside the Windows ZIP:

| File | SHA-256 |
|---|---|
| `trespasser_recomp.exe` | `8e7c0c65b8f83b084f1bdab8350237de3a78376f6dd247fd4d815170980dd872` |
| `trg-updater.exe` | `d6ac87ddab783f39dfe25f01872e428f107c6a84f3223be964bada950d527caf` |

## Security

The four final files were checked manually on VirusTotal, and the project maintainer reported them approved on 2026-11-27. The links below are the reports for the exact file hashes.

VirusTotal is an additional verification step, not an absolute guarantee of safety. Verify the SHA-256 values before running a file, and download only from this repository's Releases page.

| File | SHA-256 | VirusTotal |
|---|---|---|
| Windows ZIP: `JurassicParkTrespasserRecompiled-v0.1.4-windows-x86.zip` | `e2027381cc637d1e9fa43d781beb5bf80df9e8531a1ebba80338efb85fffca0d` | [VirusTotal](https://www.virustotal.com/gui/file/e2027381cc637d1e9fa43d781beb5bf80df9e8531a1ebba80338efb85fffca0d?nocache=1) |
| Linux AppImage: `JurassicParkTrespasserRecompiled-v0.1.4-x86_64.AppImage` | `7a26c2e4d44df51bb9bcfbb5f267704d950ae0bf80553654e4b4ef6bd2956095` | [VirusTotal](https://www.virustotal.com/gui/file/7a26c2e4d44df51bb9bcfbb5f267704d950ae0bf80553654e4b4ef6bd2956095?nocache=1) |
| Windows program: `trespasser_recomp.exe` (inside the ZIP) | `8e7c0c65b8f83b084f1bdab8350237de3a78376f6dd247fd4d815170980dd872` | [VirusTotal](https://www.virustotal.com/gui/file/8e7c0c65b8f83b084f1bdab8350237de3a78376f6dd247fd4d815170980dd872?nocache=1) |
| Windows updater: `trg-updater.exe` (inside the ZIP) | `d6ac87ddab783f39dfe25f01872e428f107c6a84f3223be964bada950d527caf` | [VirusTotal](https://www.virustotal.com/gui/file/d6ac87ddab783f39dfe25f01872e428f107c6a84f3223be964bada950d527caf?nocache=1) |

The project does not distribute commercial game files or community levels. You must supply your own legally obtained copy of the game.
