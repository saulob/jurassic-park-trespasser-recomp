# Jurassic Park: Trespasser Recompiled v0.1.2

**A HUD and Linux compatibility update.** v0.1.2 gives the minimap, the compass and the objective waypoints independent switches, fixes the Linux game folder and disc check behind the "Please Insert the Trespasser CD into C:\." prompt, writes one log format on Windows and Linux, and prepares the AppImage for the AppImage catalog.

**No game files are included.** You need your own legally obtained *Jurassic Park: Trespasser* disc image. The launcher imports it, verifies it and keeps your installation, saves and settings beside the program.

## Introduction

v0.1.2 is an update to [v0.1.1](https://github.com/saulob/jurassic-park-trespasser-recomp/blob/main/docs/RELEASE_NOTES_v0.1.1.md). It adds no levels or content. Its work is on the navigation HUD, on the Linux installation and disc detection, on logging and bug reports, and on the metadata the AppImage catalog reads.

Everything in the v0.1.0 and v0.1.1 notes still applies, including the native builds, the three renderers, native resolutions and Hor+ widescreen on Windows, cheats, gamepad support, seven languages, the TRG launcher and all four supported game versions. Those features are not repeated here.

Windows remains the primary platform. The Linux AppImage remains experimental, and the Steam Deck has not been validated on physical hardware.

## Highlights

- **Independent Minimap, Compass and Waypoint switches.** Each of the three navigation aids can be turned off on its own in the launcher. Compass Off hides only the strip; Waypoints Off hides only the objective markers; Minimap Off hides only the map. All three are on by default.
- **Linux game folder and disc detection fixed.** The game now runs from the version folder that the launcher installed, so the retail disc check finds its data and the game no longer stops at "Please Insert the Trespasser CD into C:\." in a normal installation ([issue #2](https://github.com/saulob/jurassic-park-trespasser-recomp/issues/2)).
- **One log format on Windows and Linux.** New Linux logs are named `trespasser_recomp_NNN.log`, as on Windows. Numbering never reuses a name, old `trespasser_linux_NNN.log` files stay readable, and the Bug Report page accepts both.
- **AppImage catalog compatibility.** The desktop entry and AppStream metadata describe a game launcher that opens before the game is installed, as the catalog expects.

## Gameplay and HUD Improvements

The minimap, compass, objective markers and waypoint are additions of this port; the 1998 interface had none of them. v0.1.2 lets each element be controlled separately instead of tying them together.

### New settings

The launcher's Gameplay page now has three independent rows, all On by default:

| Setting | INI key | On by default | What Off hides |
|---|---|---|---|
| Enable Minimap | `minimap` | Yes | The circular minimap only |
| Enable Compass | `compass` | Yes | The compass strip only |
| Enable Waypoints | `waypoints` | Yes | The objective markers only |

- **Minimap Off** hides the circular map in the bottom-left corner. F5, or L3 + R3 on a gamepad, still toggles only the minimap during play; the launcher setting is the state it starts in.
- **Compass Off** hides the strip at the top of the screen and its cardinal directions. The minimap and the objective markings are not affected.
- **Waypoints Off** hides every presentation of the current objective: the 3D diamond and the edge arrow, the distance readout, the route chevrons, the marker on the compass strip and the marker on the minimap. Objectives themselves, the objective list and F4 Show Objective still work.
- **Compass and Waypoints have no hotkey.** They are read when the game starts, so a change in the launcher applies to that run. F5 and L3 + R3 remain the minimap toggle.

### Compatibility and parity

- The two new keys are written separately from `minimap`, so changing one never changes another.
- A settings file written by v0.1.1 keeps its existing `minimap` value; because it has no `compass` or `waypoints` key, those two start enabled.
- The launcher rows, their help text and the Portuguese (Brazil) interface strings are present on Windows and Linux, and the runtime settings are the same on both hosts.
- Existing behaviour is unchanged: F5 and L3 + R3 toggle only the minimap, F3 (or D-pad Right) lists objectives, F4 (or D-pad Down) shows the current objective, and the HUD still hides in menus, dialogs, videos and loading screens.

## Linux Fixes

### The disc prompt on Linux

The retail game checks for its data on `C:\` when a level starts. The launcher installs each game version into its own folder, `game/versions/<version>`, and previously the Linux host mounted `game/` itself as `C:\`. The guest then looked for files that were one folder below the mount, failed the check and showed *Please Insert the Trespasser CD into C:\.*.

v0.1.2 uses the resolved version folder as the game folder for the whole run: the guest mount and the host features both receive `game/versions/<version>`, and the saves lookup is resolved for the same version. A normal launcher installation now passes the disc check.

The same run also handles the failure without hanging. A message box with **OK / Cancel**, **Retry / Cancel** or **Cancel / Try Again / Continue** now returns **Cancel** instead of the default button, so a guest retry loop cannot spin forever. If the data is genuinely missing, the game exits cleanly instead of printing the prompt every second.

### Text encoding

The guest's 8-bit strings are converted from the active code page (Windows-1252, or Windows-1251 for the Russian release) to UTF-8 before they reach the host log, the window title and the message box. Valid UTF-8 is passed through unchanged, so text is not converted twice. The log no longer contains invalid byte sequences for accented or Cyrillic game text, and the guest's own memory is never modified.

### Other Linux corrections

- Saves are resolved from the version folder the launcher selected, alongside the shared user-data folder.
- Feature teardown restores the default display code page at the end of a run.
- Headless tests cover an isolated launcher layout with spaces in its path booting past the disc check, and a missing media file exiting cleanly without a prompt loop.

## Bug Report and Logging

- **Canonical log name.** New logs on both platforms are `trespasser_recomp_NNN.log` in the `logs` folder next to the program. Windows and Linux use the same name format and numbering.
- **Monotonic numbering.** The next number is taken from the highest existing `trespasser_recomp_*`, historical `trespasser_linux_*`, older `trespasser_*` and crash-dump names (including secondary `_N.dmp` dumps). A new session never reuses or overwrites an existing log.
- **No collisions.** Linux creates the file with exclusive creation and append mode; if another session takes the number first, the next one is used. An explicit `--log-file` appends without truncating.
- **Predictable failures.** An empty path, a read-only folder or a path under a regular file fails cleanly and leaves no file handle or stale path behind.
- **Historical compatibility.** `trespasser_linux_NNN.log` files written by v0.1.1 remain valid input for the Bug Report page and remain accepted by the report service; they are not renamed or deleted.
- **Bug Report discovery.** The page recognises a Linux game session from its own start marker, so the correct log is offered on both platforms, and file-name validation still rejects paths, unrelated names and wrong extensions.
- The report service's validation was updated for the two accepted log names. Nothing was deployed as part of this release.

## AppImage Compatibility

The AppImage is being prepared for the [AppImage catalog](https://github.com/AppImage/appimage.github.io) (external pull request [#12460](https://github.com/AppImage/appimage.github.io/pull/12460)). The application is a launcher as well as a game, so the catalog's launcher exception applies. The packaged metadata now says so explicitly:

- **Desktop entry:** `Name=Jurassic Park: Trespasser Recompiled`, `GenericName=Game launcher`, and a comment that names the launcher and the requirement to bring a disc image. `Exec=AppRun`, `Terminal=false`, the icon, `StartupWMClass`, the AppStream ID and the desktop action are unchanged.
- **AppStream:** the name matches the desktop entry, the summary names the disc requirement, and the description states that no game files are included.
- **Launcher before installation:** the launcher opens without a game and shows **Game not installed** on the Play page; Play does not start the game until the files are ready. That is a normal state, not a startup failure.
- The AppImage still contains no commercial game files and does not change the disc import flow.

The catalog entry has not been approved yet. The external pull request is not modified by this release.

## Other Fixes

- The v0.1.1 release notes now use absolute links, so they work from the documentation folder and from the release page.
- The Linux documentation and the Linux package README describe the log name and the *Game not installed* state.

## Supported Game Versions

| Release | Version | Windows | Linux |
|---|---|---|---|
| English retail (UK `Trespass_UK_116`, US `Trespasser_116d`) | 1.0.116 | Yes | Yes |
| Russian Fargus (`TRESPASSER_116D`) | 1.0.116 | Yes | Yes |
| Spanish (`TRESPASSER_S`) | 1.1.117 | Yes | Yes |
| Brazilian Portuguese | 1.1.117 | Yes | Yes |

The disc is recognised by file sizes and SHA-256, never by its file name or volume label. The original executables are import inputs only and are never copied into the installation. The game versions, their fingerprints and offsets are unchanged in this release.

## Compatibility

- **Windows 10 and 11 (x86)** is the primary platform, with the native high-resolution, Hor+ widescreen and Direct3D 11 rendering paths.
- **Linux x86_64 and Steam Deck** run the experimental AppImage with the Vulkan or software renderer. Gameplay stays in the 4:3 mode, scaled to the window.
- The AppImage targets Ubuntu 20.04 and a glibc baseline of 2.31.
- Settings and saves written by v0.1.1 are read unchanged; the only new keys are the two optional HUD switches, which default to On.

### Validation

The headless release gate passed on Windows and Linux: 219 suite results, 0 failures and 688,817 checks, including the four game versions, Windows/Linux feature parity and the packaged release files. The desktop entry and AppStream metadata were read back from the final AppImage. Real GPU rendering, audio, controllers and the Steam Deck still need manual testing; headless tests are not a substitute for it.

## Known Limitations

- **Windows is the primary platform.** Linux is experimental, and the Steam Deck has not been validated on physical hardware.
- **Linux does not have every Windows display feature.** Native high resolutions and true 21:9 / 32:9 Hor+ gameplay remain Windows-only, and the Linux guest stays at its 4:3 mode scaled to the window.
- **The AppImage catalog entry is pending.** The metadata is prepared, but the external pull request has not been approved.
- **Issue #2 awaits the reporter's confirmation.** The cause identified in the supplied logs is corrected and covered by tests; a retest on the reporter's system is still the final word.
- **No validated uncapped or 120 FPS mode.** VSync can be turned off, but there is no frame-rate guarantee above 60 FPS.
- **Windows Vulkan** is selectable, but Direct3D 11 remains the recommended renderer.
- **Unsigned Windows executables.** SmartScreen may warn on first launch.
- All other limitations in the [v0.1.1 notes](https://github.com/saulob/jurassic-park-trespasser-recomp/blob/main/docs/RELEASE_NOTES_v0.1.1.md#known-limitations) and the [v0.1.0 notes](https://github.com/saulob/jurassic-park-trespasser-recomp/blob/main/docs/RELEASE_NOTES_v0.1.0.md#known-issues-and-limitations) still apply.

## Related Issues

- [#2 — "Please Insert the Trespasser CD into C:\" on Linux](https://github.com/saulob/jurassic-park-trespasser-recomp/issues/2). The supplied logs show a complete, playable English installation whose guest still asked for the disc once per second after the launcher started it. The Linux host mounted `game/` instead of the selected `game/versions/en-116`, so the guest's own `C:\` check could not see the installed files, and the message box answered with the default button on every pass. v0.1.2 mounts the selected version folder and returns Cancel for cancelable prompts. The headless tests boot an isolated launcher layout past the disc check and confirm that a genuinely missing file exits instead of looping.

The issue is not closed, and no comment or metadata change was made on GitHub as part of this release.

## Credits and Acknowledgements

Special thanks to Nick3069 from the TresCom community for testing the port and providing feedback on the minimap, compass and navigation markers. His suggestion helped inspire the independent HUD visibility settings introduced in this release.

Thanks also to everyone who tested v0.1.1, reported problems and sent logs, including the reporter of issue #2. The full credits are in the [v0.1.0 notes](https://github.com/saulob/jurassic-park-trespasser-recomp/blob/main/docs/RELEASE_NOTES_v0.1.0.md#credits-and-acknowledgements).

## Downloads

These links point to the v0.1.2 assets of the v0.1.2 release and work once it is published.

| Platform | File | Checksum file |
|---|---|---|
| Windows 10 / 11 (x86) | [`JurassicParkTrespasserRecompiled-v0.1.2-windows-x86.zip`](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.2/JurassicParkTrespasserRecompiled-v0.1.2-windows-x86.zip) (29,449,338 bytes) | [`JurassicParkTrespasserRecompiled-v0.1.2-windows-x86.zip.sha256`](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.2/JurassicParkTrespasserRecompiled-v0.1.2-windows-x86.zip.sha256) |
| Linux x86_64 / Steam Deck (experimental) | [`JurassicParkTrespasserRecompiled-v0.1.2-x86_64.AppImage`](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.2/JurassicParkTrespasserRecompiled-v0.1.2-x86_64.AppImage) (49,269,056 bytes) | [`JurassicParkTrespasserRecompiled-v0.1.2-x86_64.AppImage.sha256`](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.2/JurassicParkTrespasserRecompiled-v0.1.2-x86_64.AppImage.sha256) |

Debug symbols, development builds, game data, saves and logs are not published.

## Checksums

The two `.sha256` files published with the release contain:

```
54f6867ab2d0b0d2ab121faa14b10f5ad1dc6af1b6eef5bca8dc9019d8b80a54  JurassicParkTrespasserRecompiled-v0.1.2-windows-x86.zip
0ef5e21e3229847cbfea8ea37482b9a8f8a411daf30dfbbb47ead7ed5bc4f8f6  JurassicParkTrespasserRecompiled-v0.1.2-x86_64.AppImage
```

Inside the Windows ZIP:

| File | SHA-256 |
|---|---|
| `trespasser_recomp.exe` | `e2be737d8b47f3a8a50a747f8105bee82f0e8fd2d291e9e5a2ace44fe3821aa9` |
| `trg-updater.exe` | `01986f5196a66c0c6ec8556f1d475c402d7450259cb23fd6249a4a8ee349b72d` |

## Security

The four final files were checked manually on VirusTotal before publication, and the project maintainer reported OK for all four. The links below are the reports for the exact file hashes.

VirusTotal is an additional verification step, not an absolute guarantee of safety. Verify the SHA-256 values before running a file, and download only from this repository's Releases page.

| File | SHA-256 | VirusTotal |
|---|---|---|
| Windows ZIP: `JurassicParkTrespasserRecompiled-v0.1.2-windows-x86.zip` | `54f6867ab2d0b0d2ab121faa14b10f5ad1dc6af1b6eef5bca8dc9019d8b80a54` | [VirusTotal](https://www.virustotal.com/gui/file/54f6867ab2d0b0d2ab121faa14b10f5ad1dc6af1b6eef5bca8dc9019d8b80a54?nocache=1) |
| Linux AppImage: `JurassicParkTrespasserRecompiled-v0.1.2-x86_64.AppImage` | `0ef5e21e3229847cbfea8ea37482b9a8f8a411daf30dfbbb47ead7ed5bc4f8f6` | [VirusTotal](https://www.virustotal.com/gui/file/0ef5e21e3229847cbfea8ea37482b9a8f8a411daf30dfbbb47ead7ed5bc4f8f6?nocache=1) |
| Windows program: `trespasser_recomp.exe` (inside the ZIP) | `e2be737d8b47f3a8a50a747f8105bee82f0e8fd2d291e9e5a2ace44fe3821aa9` | [VirusTotal](https://www.virustotal.com/gui/file/e2be737d8b47f3a8a50a747f8105bee82f0e8fd2d291e9e5a2ace44fe3821aa9?nocache=1) |
| Windows updater: `trg-updater.exe` (inside the ZIP) | `01986f5196a66c0c6ec8556f1d475c402d7450259cb23fd6249a4a8ee349b72d` | [VirusTotal](https://www.virustotal.com/gui/file/01986f5196a66c0c6ec8556f1d475c402d7450259cb23fd6249a4a8ee349b72d?nocache=1) |

The project does not distribute commercial game files. You must supply your own compatible disc image.
