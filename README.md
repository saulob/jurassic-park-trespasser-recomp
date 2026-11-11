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

Out of scope until boot is stable: widescreen, high resolutions, uncapped frame rate,
controller support, translations, mods.

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
.\tools\build.ps1 -Run     # the same, then starts the game (trespasser.exe --run)
```

The first run needs your disc image (`-Iso`, `TRESPASSER_ISO` or `config\local.json`) and about
20 minutes of analysis. Later runs redo only what changed. Details, options and what to do when
a run stops: [BUILD](docs/BUILD.md).

## Layout

```
config/        target fingerprint, decoder fixups, extra entry points
tools/         disc extraction, analysis and lift drivers
src/host/      the runtime host (hand-written)
src/recomp/gen lifted C (generated, ignored by Git)
docs/          ANALYSIS, ARCHITECTURE, BUILD, LEGAL
external/      pcrecomp checkout (fetched, ignored by Git)
game/          your extracted game files (ignored by Git)
analysis/      regenerated analysis output (ignored by Git)
logs/          one log per run (ignored by Git)
```

## Credits

Built on [sp00nznet/pcrecomp](https://github.com/sp00nznet/pcrecomp), following the
reconnaissance of [sp00nznet/trespasser](https://github.com/sp00nznet/trespasser).
See [LEGAL](docs/LEGAL.md).
