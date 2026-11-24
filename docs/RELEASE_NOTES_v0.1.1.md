# Jurassic Park: Trespasser Recompiled v0.1.1

**A stability and rendering update.** v0.1.1 makes the renderer you choose the renderer you get, fixes visual artifacts in the Vulkan and Direct3D 11 renderers, recovers cleanly after Alt+Tab and screenshot tools, corrects the sky and fog colours, and brings all four supported game versions to the native Linux build.

**No game files are included.** You need your own legally obtained *Jurassic Park: Trespasser* disc image. The launcher imports it, verifies it and keeps your installation, saves and settings beside the program.

## Introduction

v0.1.1 is an update to [v0.1.0](RELEASE_NOTES_v0.1.0.md), the first public release. It adds no new gameplay features. Its work is on the rendering paths players hit most, on how the renderer is chosen, and on the Linux build's coverage of the supported game versions.

Everything in the v0.1.0 notes still applies, including the native builds, the three renderers, native resolutions and Hor+ widescreen on Windows, the minimap and compass, cheats, gamepad support, seven languages and the TRG launcher. Those features are not repeated here.

Windows remains the primary platform. The Linux AppImage remains experimental, and the Steam Deck has not been validated on physical hardware.

## Highlights

- **The renderer you choose is the renderer you get.** A renderer set in the launcher or settings is no longer overridden by a leftover `TRESPASSER_RENDERER` environment variable. The command line still has the final word.
- **Vulkan artifacts fixed (Windows).** Magenta pixels and mis-textured fragments that appeared after focus was lost and regained, such as with Flameshot, are gone.
- **Recovery after Alt+Tab and screenshot tools.** The Vulkan and Direct3D 11 renderers recover correctly when the display device is recreated.
- **Sky and fog colours.** Each level's fog and background colours are applied again after the device is recreated and after level changes, which removes the incorrect blue band on the sky's horizon. Enhanced Sky is preserved.
- **All four game versions on Linux.** English 1.0.116, Russian Fargus 1.0.116, Spanish 1.1.117 and Brazilian Portuguese 1.1.117 can all be played by the native Linux build.

## Improvements

- **Renderer selection follows one fixed order:** the `--renderer` command-line option, then an explicit renderer in the settings file, then `TRESPASSER_RENDERER`, then automatic selection. An invalid environment value is ignored. The same rules apply on Windows and Linux.
- **The log names the source of the choice.** Startup lines say whether the renderer came from the command line, the settings file, the environment or automatic detection, and they name the renderer actually in use.
- **`--renderer` accepts every renderer:** `auto`, `d3d11`, `vulkan` and `software`. The help text lists them all.
- **The Linux four-version build** selects the lifted build that matches the installed game, as the Windows build does.
- **More orderly Linux shutdown.** Shared gameplay patches are disarmed when a run ends, before the game profile is released.
- **Obsolete `Ctrl+F11` removed.** The retail game's original console input no longer has a hotkey, and the launcher no longer lists it. `F10` (Cheats), `F11` (Windowed / Borderless Fullscreen) and the other hotkeys are unchanged.

## Bug Fixes

### Renderer selection

- The renderer set in the launcher and in the settings file is used at start, instead of a value that a leftover environment variable could override.
- The precedence between command line, settings file, environment variable and automatic detection is corrected, and invalid environment values are ignored.

### Direct3D 11 (Windows)

- Stale graphics state after the device is recreated is corrected. Texture bindings that pointed at a freed or replaced texture are cleared, and cached render state is invalidated when the device is attached or detached.
- The first frame after Alt+Tab or a screenshot tool starts from a known state.

### Vulkan (Windows)

- Magenta pixels and mis-textured fragments after focus loss and recovery are fixed.
- The CPU-drawn parts of a frame (menus, text and some polygons) are composited only where the CPU actually wrote, so unwritten areas are not taken for picture data.
- A recreated Vulkan device starts from the picture already on the game's surface instead of an undefined reference.
- Pending CPU polygon work is cleared when the device is released, so it cannot leak into the next session.
- Blending polygons read back only the parts of the render target that are stale.
- Shadows, transparencies, textures, water and the other effects are preserved.

### Fog, sky and background

- The engine's fog colour is kept in sync with the level at the start of every scene. Recreating the device no longer restores the previous level's colour.
- The background colour, previously set only when the device was created, follows the level too.
- The blue band on the horizon is removed, and Enhanced Sky is preserved.

### Launcher and compatibility

- **Windows:** `Ctrl+F11` no longer toggles fullscreen or reaches the game by accident.
- **Linux and Steam Deck:** feature setup and teardown ordering is fixed, and the Spanish and Brazilian Portuguese releases use their own address maps and overrides.
- **Documentation:** the README, Linux notes and game-version documentation describe four-version Linux support.

### Build and release validation

- A Windows Release build error in the clang-cl toolchain is corrected. The Visual Studio development build did not hit it.
- A warning treated as an error in the Linux GCC build is corrected.
- The generated host-offset manifest is regenerated. It has no semantic change.
- The Release test gate now checks the packaged release folder and ZIP correctly.

## Compatibility

### Supported game versions

| Release | Version | Windows | Linux |
|---|---|---|---|
| English retail (UK `Trespass_UK_116`, US `Trespasser_116d`) | 1.0.116 | Yes | Yes |
| Russian Fargus (`TRESPASSER_116D`) | 1.0.116 | Yes | Yes |
| Spanish (`TRESPASSER_S`) | 1.1.117 | Yes | Yes |
| Brazilian Portuguese | 1.1.117 | Yes | Yes |

The disc is recognised by file sizes and SHA-256, never by its file name or volume label. The original executables are import inputs only and are never copied into the installation.

### Platform notes

- **Windows 10 / 11 (x86)** is the primary platform. It has the native high-resolution, Hor+ widescreen and Direct3D 11 rendering paths.
- **Linux x86_64 and Steam Deck** run the experimental AppImage on the Vulkan or software renderer. Gameplay stays in the 4:3 mode, scaled to the window.
- The AppImage targets Ubuntu 20.04 and a glibc baseline of 2.31; the newest glibc symbol it uses is 2.30.

### Test results

- The project's headless test gate passes: 219 PASS, 0 FAIL, 688,620 checks. Windows and Linux feature parity is covered by the same contract.
- Headless boot of all four game versions passes on Linux.
- Manual validation on Windows: a Vulkan session with several device recreations finished normally, with no Direct3D failures, discarded Vulkan draws or failed GPU polygon uploads, and the artifacts described above were not seen.
- Not validated by hand: the Vulkan and fog fixes on Linux, and real GPU rendering, audio and controllers on physical Linux or Steam Deck hardware. Headless tests are not a substitute for that manual testing.

## Known Limitations

- **Windows is the primary platform.** Linux is experimental, and the Steam Deck has not been validated on physical hardware.
- **Linux does not have every Windows display feature.** Native high resolutions and true 21:9 / 32:9 Hor+ gameplay remain Windows-only, and the Linux guest stays at its 4:3 mode scaled to the window. The Windows HD video pipeline is not part of the Linux build.
- **No validated uncapped or 120 FPS mode.** VSync can be turned off, but there is no frame-rate guarantee above 60 FPS.
- **Windows Vulkan** is selectable, but Direct3D 11 remains the recommended renderer.
- **Unsigned Windows executables.** SmartScreen may warn on first launch.
- The project does not distribute commercial game files. You import your own compatible disc.
- All other limitations in the [v0.1.0 notes](RELEASE_NOTES_v0.1.0.md#known-issues-and-limitations) still apply.

## Downloads

These links point to the v0.1.1 assets of the [v0.1.1 release](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/tag/v0.1.1) and work once the release is published.

| Platform | File | Checksum file |
|---|---|---|
| Windows 10 / 11 (x86) | [`JurassicParkTrespasserRecompiled-v0.1.1-windows-x86.zip`](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.1/JurassicParkTrespasserRecompiled-v0.1.1-windows-x86.zip) (29,449,080 bytes) | [`JurassicParkTrespasserRecompiled-v0.1.1-windows-x86.zip.sha256`](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.1/JurassicParkTrespasserRecompiled-v0.1.1-windows-x86.zip.sha256) |
| Linux x86_64 / Steam Deck (experimental) | [`JurassicParkTrespasserRecompiled-v0.1.1-x86_64.AppImage`](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.1/JurassicParkTrespasserRecompiled-v0.1.1-x86_64.AppImage) (49,297,728 bytes) | [`JurassicParkTrespasserRecompiled-v0.1.1-x86_64.AppImage.sha256`](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.1/JurassicParkTrespasserRecompiled-v0.1.1-x86_64.AppImage.sha256) |

Debug symbols, development builds, game data, saves and logs are not published.

## Security

The four final files were checked manually on VirusTotal before publication, and the project maintainer reported OK for all four. The links below are the reports for the exact file hashes.

VirusTotal is an additional verification step, not an absolute guarantee of safety. Verify the SHA-256 values before running a file, and download only from this repository's Releases page.

| File | SHA-256 | VirusTotal |
|---|---|---|
| Windows ZIP: `JurassicParkTrespasserRecompiled-v0.1.1-windows-x86.zip` | `bc8e94cea2d816e9e5617ea30138fef57607537f744867acce1d1a29d164601e` | [VirusTotal](https://www.virustotal.com/gui/file/bc8e94cea2d816e9e5617ea30138fef57607537f744867acce1d1a29d164601e?nocache=1) |
| Linux AppImage: `JurassicParkTrespasserRecompiled-v0.1.1-x86_64.AppImage` | `2d82953c7b1da41d84f1aed6c9154e7a5d1ff9bab458cbfc88019e2355751b14` | [VirusTotal](https://www.virustotal.com/gui/file/2d82953c7b1da41d84f1aed6c9154e7a5d1ff9bab458cbfc88019e2355751b14?nocache=1) |
| Windows program: `trespasser_recomp.exe` (inside the ZIP) | `b5369740890fddab309614133a676787d79ef7699f420c943954f6f1b463a1d9` | [VirusTotal](https://www.virustotal.com/gui/file/b5369740890fddab309614133a676787d79ef7699f420c943954f6f1b463a1d9?nocache=1) |
| Windows updater: `trg-updater.exe` (inside the ZIP) | `20dcdc3b5fc16cde62be3a474c31ffe5a6c646e67f4e5d4d771bc8eb3cb6edac` | [VirusTotal](https://www.virustotal.com/gui/file/20dcdc3b5fc16cde62be3a474c31ffe5a6c646e67f4e5d4d771bc8eb3cb6edac?nocache=1) |

## Checksums

The two `.sha256` files published with the release contain:

```
bc8e94cea2d816e9e5617ea30138fef57607537f744867acce1d1a29d164601e  JurassicParkTrespasserRecompiled-v0.1.1-windows-x86.zip
2d82953c7b1da41d84f1aed6c9154e7a5d1ff9bab458cbfc88019e2355751b14  JurassicParkTrespasserRecompiled-v0.1.1-x86_64.AppImage
```

The program and updater inside the ZIP are listed in the Security table above.

## Credits and Feedback

Thanks to everyone who tested v0.1.0, reported problems and sent logs. The full credits are in the [v0.1.0 notes](RELEASE_NOTES_v0.1.0.md#credits-and-acknowledgements).

Report problems from the launcher's Bug Report page or on the project's GitHub issues, and include the log file. This is an unofficial fan preservation project, not affiliated with or endorsed by DreamWorks, Universal or EA.
