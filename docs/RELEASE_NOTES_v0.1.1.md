# Jurassic Park: Trespasser Recompiled v0.1.1

*Jurassic Park: Trespasser Recompiled* v0.1.1 is a patch release over [v0.1.0](RELEASE_NOTES_v0.1.0.md). It brings full four-version parity to the native Linux x86_64 runtime, improves feature lifecycle stability, removes the obsolete `Ctrl+F11` shortcut, and updates compatibility documentation.

No commercial game files are included. Bring your own legally obtained *Jurassic Park: Trespasser* disc image (`.iso`).

---

## Highlights

- **Full Linux game version parity**: English 1.0.116, Russian Fargus 1.0.116, Spanish 1.1.117, and Brazilian Portuguese 1.1.117 are now all supported by both the Windows and Linux executables.
- **Native Linux runtime improvements**: dynamic selection across all three lifted builds (`lift116`, `lift_es117`, `lift_ptbr117`), 117-specific SelfMod handling, per-version overrides and profiles, and shared lifted `MSVCIRT`.
- **Stability fixes**: cleaner feature lifecycle management, patch disarming during shutdown, and fixes to the PowerShell test runner, Linux staging, and test reporting.
- **Launcher and documentation cleanup**: removed the obsolete `Ctrl+F11` cheat console path and references while preserving `F10` (Cheats), `F11` (Fullscreen), and all other active port hotkeys.

---

## Full Linux Game Version Parity

| Release | Game Build | Windows | Linux | Lifted Build |
|---|---|---|---|---|
| English retail (UK `Trespass_UK_116`, US `Trespasser_116d`) | 1.0.116 | Yes | Yes | `lift116` |
| Russian Fargus (`TRESPASSER_116D`) | 1.0.116 | Yes | Yes | `lift116` |
| Spanish (`TRESPASSER_S`) | 1.1.117 | Yes | Yes | `lift_es117` |
| Brazilian Portuguese | 1.1.117 | Yes | Yes | `lift_ptbr117` |

- The disc is identified by file sizes and SHA-256 digests, never by file name or volume label.
- The original retail executables remain import inputs only and are never copied into the installed game folder.
- Unlike the earlier v0.1.0 Linux AppImage (which ran only the 1.0.116 releases), the v0.1.1 Linux executable carries all three lifted builds.

---

## Native Linux Runtime Improvements

- **Dynamic three-lift selection**: the Linux host selects `lift116`, `lift_es117`, or `lift_ptbr117` at startup according to the installed game version.
- **117-specific SelfMod**: SelfMod handling now covers the Spanish and Brazilian Portuguese 1.1.117 lifted builds alongside 1.0.116.
- **Per-version overrides and profiles**: active profile selection, guest address maps, and host overrides apply consistently across all four releases.
- **Shared lifted `MSVCIRT`**: the lifted `MSVCIRT` guest module is mapped and shared by all four releases on Linux.
- **Address and initialization fixes**: corrected guest address resolution and startup paths needed to boot all four releases cleanly on x86_64 Linux.

---

## Stability

- **Feature lifecycle**: fixed host feature initialization and teardown ordering across sessions.
- **Shutdown patch disarming**: host patches and hooks are disarmed cleanly during shutdown.
- **Test runner and staging**: fixed PowerShell test runner handling, Linux artifact staging checks, and parity/test reporting.

---

## Launcher and Documentation

- **Four-version support**: launcher and documentation now reflect full Windows and Linux parity across English, Russian Fargus, Spanish, and Brazilian Portuguese releases.
- **Removed obsolete `Ctrl+F11` shortcut**: `Ctrl+F11` is now dropped by the shared hotkey policy (`key_policy`) and ignored by the launcher fullscreen handler so it neither opens the obsolete retail console nor toggles fullscreen accidentally.
- **Preserved active hotkeys**: `F10` (Cheats panel), `F11` (Windowed / Borderless Fullscreen), `F2` (Controls), `F3` (Objectives), `F4` (Show Objective), `F5` (Minimap), `F6` (Frame counter), `F7` / `F8` (Quick Save / Quick Load), and `Alt+F4` (Exit) remain unchanged.
- **Updated compatibility documentation**: `README.md`, `docs/LINUX.md`, `docs/GAME_VERSIONS.md`, `docs/CONTROLS.md`, `docs/ARCHITECTURE.md`, and `docs/LAUNCHER.md` have been updated to match v0.1.1 while preserving v0.1.0 historical records.

---

## Testing

The full Linux parity test gate (`tools/test.ps1`) verified the following headless and build checks prior to v0.1.1 release preparation:

- **218 PASS, 0 FAIL** (**688,495 checks**)
- **125 / 125 global features PASS** in the Windows / Linux parity contract
- **All four game versions** (English 1.0.116, Russian Fargus 1.0.116, Spanish 1.1.117, Brazilian Portuguese 1.1.117) passed the **headless first-frame boot** on Linux
- **Windows build PASS**, **Linux build PASS**, **Linux staging PASS**
- **Maximum GLIBC symbol version**: `2.30` (within the Ubuntu 20.04 / GLIBC `2.31` baseline)
- **Linux FFmpeg (LGPL)**: enabled for native Smacker video decoding

> **Important scope note:** These automated results validate headless boot, parity contracts, and build/staging hygiene. They do **not** replace manual validation of full gameplay, real Vulkan GPU rendering, audio hardware, or controllers on physical Linux or Steam Deck hardware.

---

## Known Limitations

- **Linux support remains experimental**, and **Steam Deck has not been validated on physical hardware**.
- **No native True Ultrawide on Linux**: the guest remains in its 4:3 mode scaled to the window; native 4K and 21:9 / 32:9 Hor+ gameplay remain Windows-only.
- **No Windows HD video pipeline on Linux**: Linux plays the original Smacker videos with subtitles.
- **Vulkan GPU validation in WSL**: real Vulkan GPU rendering is not validated in WSL; Software rendering is validated in the headless environment.
- **Other port limitations carried over from v0.1.0**: no validated uncapped/120 FPS mode, no HD 3D world texture pack, no dedicated ultrawide HUD, no HDR, no controller vibration, no mod support, no custom waypoints, and unsigned Windows binaries. See [v0.1.0 Known Issues and Limitations](RELEASE_NOTES_v0.1.0.md#known-issues-and-limitations) for the full baseline list.

---

## Downloads and Checksums

Release artifacts, file sizes, SHA-256 checksums, and VirusTotal results will be published on the [GitHub Releases](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/latest) page when v0.1.1 is built and released:

- Windows 10 / 11 (x86): `JurassicParkTrespasserRecompiled-v0.1.1-windows-x86.zip` (+ `.sha256`)
- Linux x86_64 / Steam Deck (experimental): `JurassicParkTrespasserRecompiled-v0.1.1-x86_64.AppImage` (+ `.sha256`)
