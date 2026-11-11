# Jurassic Park: Trespasser Recompiled

A static recompilation of *Jurassic Park: Trespasser* (1998) for modern Windows. The retail
executable's machine code is translated to C once, ahead of time, and compiled into a native
host. No CPU emulator, and no launcher around the original program.

You need your own English disc. Nothing from the game is in this repository.

## Status

**Bring-up. The game has not been run yet.** What exists has been checked statically only.

| Milestone | State |
|---|---|
| Disc and executable inventoried, target fingerprinted | done ([ANALYSIS](docs/ANALYSIS.md)) |
| All of `.text` lifted to C: 0 lift errors, 0 unimplemented instructions, 0 unresolvable constant targets | done, static evidence |
| All 297 imports resolve against this Windows' 32-bit DLLs | done, static evidence |
| Host written: loader, import binding, SelfMod bridge, registry store, logs, fault reports | written, **not yet compiled** |
| Host builds | not attempted |
| Recompiled executable starts | not attempted |
| Original subsystems initialise | not attempted |
| Game files load | not attempted |
| Video and audio initialise | not attempted |
| Main menu | not attempted |
| First level loads | not attempted |
| Gameplay | not attempted |

A launcher installs the game from your disc image and sets display and graphics options:
render resolution up to 4K and native, true widescreen (Hor+) up to 32:9, MSAA, anisotropic
filtering, VSync ([LAUNCHER](docs/LAUNCHER.md)). Not yet: uncapped frame rate, controller support,
translations, mods, draw distance and LOD changes, x64.

## How it works

- The target is `setup\tpassp6.exe` from the disc: retail 1.0.116, Pentium Pro build,
  SHA-256 `d88bb548…5679ed`.
- [pcrecomp](https://github.com/sp00nznet/pcrecomp) recovers the functions in `.text` and
  lifts them to C.
- The host is a 32-bit process that maps the original image at `0x00400000`, forwards its
  imports to the real Windows DLLs, and runs the lifted C in place of `.text`.
- `SelfMod`, the self-patching rasteriser section, runs as the original machine code.

[ARCHITECTURE](docs/ARCHITECTURE.md) lists exactly which parts are recompiled, which are
original code, and which are Windows.

## Quick start

```powershell
.\tools\build.ps1          # fetches, extracts, analyses, lifts and builds what is out of date
.\tools\run.ps1            # opens the launcher, then the game, from the current build (builds nothing)
```

`build.ps1` is for the first build and after changing code; `run.ps1` is the normal way to play.

The first run needs your disc image (`-Iso`, `TRESPASSER_ISO` or `config\local.json`) and about
20 minutes of analysis. Later runs redo only what changed. Details, options and what to do when
a run stops: [BUILD](docs/BUILD.md).

## Layout

```
config/        target fingerprint, decoder fixups, extra entry points
tools/         disc extraction, analysis and lift drivers
src/host/      the runtime host (hand-written)
src/recomp/gen lifted C (generated, ignored by Git)
launcher/      the launcher: pages, disc image reader, installer (C++)
assets/        launcher header art, sponsor heart and page icons
packaging/     third-party licence notices
src/common/    settings file and display arithmetic shared by host, launcher and tests
docs/          ANALYSIS, ARCHITECTURE, BUILD, LAUNCHER, LEGAL
external/      pcrecomp checkout (fetched, ignored by Git)
game/          your installed game files (ignored by Git)
user/          trespasser.ini, the game's registry store, saves (ignored by Git)
analysis/      regenerated analysis output (ignored by Git)
logs/          one log per run (ignored by Git)
```

## Credits

Built on [sp00nznet/pcrecomp](https://github.com/sp00nznet/pcrecomp), following the
reconnaissance of [sp00nznet/trespasser](https://github.com/sp00nznet/trespasser).
See [LEGAL](docs/LEGAL.md).
