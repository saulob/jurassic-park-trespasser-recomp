# Legal position

This repository contains tools, a runtime host and documentation. It contains no game files,
no game source and no code generated from the game.

## The game

*Jurassic Park: Trespasser* (1998) is © DreamWorks Interactive / Electronic Arts. You need your
own copy. The following stay on your machine and are ignored by Git:

| Path | Contents |
|---|---|
| `game/` | files extracted from your disc image |
| `src/recomp/gen/` | C lifted from the retail executable: a derivative work of it |
| `analysis/` | function catalogs and metadata derived from the executable |
| `config/local.json` | the path to your disc image |

The disc image itself is only ever opened for reading.

No copy protection is circumvented. The executable has none; its start-up media check is a
test for a data file, and it passes because the file is present at the configured location.

## Third-party code

| Project | Licence | How it is used |
|---|---|---|
| [sp00nznet/pcrecomp](https://github.com/sp00nznet/pcrecomp) | MIT | fetched at a pinned commit into `external/` (not committed); its tools produce the lifted C and its `native32` runtime is compiled into the host |
| [sp00nznet/themovies](https://github.com/sp00nznet/themovies) | MIT | `tools/run_lift.py`, `src/host/main.c` and `src/host/diagnostics.c` follow the structure of its `run_lift.py` and `host.c` |
| [sp00nznet/trespasser](https://github.com/sp00nznet/trespasser) | MIT | its reconnaissance documents were the checklist for `docs/ANALYSIS.md`; no code was taken |
| capstone, pefile | BSD-3-Clause, MIT | Python libraries, installed by the user |
| [ocornut/imgui](https://github.com/ocornut/imgui) | MIT | fetched at a pinned commit into `external/` (not committed); compiled into the launcher |
| [TekRantGaming/trg-launcher](https://github.com/TekRantGaming/trg-launcher) | MIT | fetched at a pinned commit into `external/` (not committed) and patched with `launcher/patches/trg-launcher`, the series of Dragon's Crown Pro PC and 13 Sentinels |
| [libsdl-org/SDL](https://github.com/libsdl-org/SDL) 3.4.10 | zlib | fetched at a pinned commit into `external/` (not committed); built as a static library for the launcher window |
| [memononen/nanosvg](https://github.com/memononen/nanosvg) | zlib | vendored in `launcher/third_party/nanosvg` (from 13 Sentinels), draws the sidebar icons |
| Launcher icons and sponsor heart | CC0, CC BY (`display.svg`), MIT (`bug-report.svg`) | `assets/launcher`; sources in `assets/launcher/icons/SOURCES.md`, notices in `packaging/licenses/THIRD-PARTY-LICENSES.txt` |

`src/host/selfmod.c` adapts the register hand-over of pcrecomp's `gen_bridge`.
MIT requires the copyright notice to accompany substantial copies: "Copyright (c) 2026 sp00nz".

## OpenTrespasser and the circulating source

The original development source circulates publicly and
[OpenTrespasser](https://github.com/OpenTrespasser/JurassicParkTrespasser) builds from it. No
licence was ever granted for that source. It was not read, consulted or copied for anything in
this repository: every fact in `docs/ANALYSIS.md` and every address in the code comes from the
retail binary.

## This repository's own code

No licence file has been added yet; that choice belongs to the repository owner.
