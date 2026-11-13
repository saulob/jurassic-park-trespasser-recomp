# Jurassic Park: Trespasser Recompiled

A static recompilation of *Jurassic Park: Trespasser* (1998) for modern Windows. The retail
executable's machine code is translated to C once, ahead of time, and compiled into a native
host. No CPU emulator, and no launcher around the original program.

You need your own English disc. Nothing from the game is in this repository.

## Status

**Playable, in testing.** The game installs from your disc, starts, and runs its menus and levels with the Direct3D 11
renderer or its own software renderer.

| Milestone | State |
|---|---|
| Disc and executable inventoried, target fingerprinted | done ([ANALYSIS](docs/ANALYSIS.md)) |
| All of `.text` lifted to C: 0 lift errors, 0 unimplemented instructions, 0 unresolvable constant targets | done |
| All 297 imports resolve against this Windows' 32-bit DLLs | done |
| Host, launcher and installer build and run | done |
| Menus, video and audio | working |
| Levels and gameplay | playable, in testing |

## What the port adds

Jurassic Park: Trespasser Recompiled is a PC port powered by static recompilation, with a modern host around the
original game. These are part of the normal Direct3D 11 renderer; there is nothing to turn on:

- **Direct3D 11 renderer**: the game's Direct3D 6 drawing on the graphics card, with the software renderer kept as a
  compatibility fallback (chosen in the launcher; it stays until the game closes).
- **Native high resolutions**: the 3D world is drawn at the monitor's resolution (or 720p to 4K), not upscaled.
- **True widescreen**: 16:9 to 32:9 show more of the world to the sides (Hor+), with no stretching and no holes.
- **Scaled UI**: menus and dialogs keep their original 4:3 layout, scaled to the screen.
- **Modern texture filtering**: anisotropic filtering up to 16x, and MSAA up to 8x where the card supports it.
- **Generated mipmaps**: the 3D world's textures get full mip chains made from the originals at load time (the files
  are never changed), so distant ground and foliage stay steady and alpha-tested plants keep their density.
- **Improved draw distance**: every object stays drawn 1.6 times as far, and stays a 3D model out to 100-150 m before
  the game may swap it for a flat impostor (15 m in 1998).
- **Textured distant terrain**: the ground keeps its own detailed textures out to the far clip instead of a blurry
  map beyond 345 m, with more room for terrain textures and safer terrain memory.
- **Improved LOD behaviour**: models keep their detail measured at the real render size, and textures, terrain and
  impostors use their full 256-pixel resolution instead of the 1998 limit of 128.
- **Modern texture cache**: the game's texture memory grows from 24 MB to 64 MB, with the engine's 32-bit budget
  arithmetic corrected, and textures survive Alt+Tab and menu changes without being rebuilt.
- **Steady texture streaming**: the game's per-frame texture upload window is timed precisely, so newly visible
  scenery gets its textures sooner instead of showing flat colours.
- **Performance**: menus over a paused game and the start-up checks no longer copy the full picture back from
  the graphics card.

The launcher installs the game from your disc image and keeps only the useful choices: resolution, aspect ratio,
anti-aliasing, texture filtering, VSync and renderer ([LAUNCHER](docs/LAUNCHER.md)). Not yet: uncapped frame rate,
controller support, translations, mods, x64.

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
.\tools\build.ps1 -Run     # builds what is out of date, then opens the launcher and the game
.\tools\build.ps1          # builds only
```

Nothing that is already current is redone, so the same command is the normal way to build and to play.
`.\tools\run.ps1` is an optional shortcut that starts the existing build without checking for changes.

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
