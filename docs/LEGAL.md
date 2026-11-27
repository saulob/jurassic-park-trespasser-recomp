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

## Translated text

The port embeds the text of seven community and official translations, converted to its own format
(`assets/translations`): the game strings of the official Brazilian Portuguese, French, German and Spanish translations
(DreamWorks Interactive; GameStrings conversion by Nick3069), the Russian game strings and the Russian subtitles
(cynodesmus) and the Italian subtitles (KeyofBlueS), all hosted and preserved by the TresCom community. Credits are in
the [README](../README.md#languages) and on the launcher's About page.

The text the port adds to complete every language (`*.port.txt` and `*.interface.txt` in `assets/translations`:
subtitles, the Italian game text, missing strings, the words of the menus and the port's own interface) was written for
the port and is credited to it, not to the authors above.

None of these downloads states a licence, and the repository holds no permission from their authors. Including the text
is the repository owner's decision and responsibility; nothing else of the packages is included: no audio, menu picture,
video or program, and the voices stay the player's own English files. The translated menu words are drawn over the
player's own menu pictures at run time; no picture of the game or of the packages is in the repository. `tools/translation_pack.cpp` makes the files from
the extracted downloads and recognises each source by name, size and SHA-256; the downloads themselves are not in the
repository. The tests use the embedded text and made-up fixtures.

## Subtitle timing and restored dialogue

The corrected caption timings and the restored English dialogue (five audio records, 4,159,690 bytes of PCM) come from
the *English Stream TPA Fix (Subtitle Timing Fix)* by KeyofBlueS, preserved and hosted by the TresCom community
(https://www.trescom.org/download/10423/, March 14, 2026). The download states no licence and the repository holds no
permission from its author; including this data is the repository owner's decision and responsibility, as for the
translations above. Only the captions and the five records the port needs were extracted: the modified `Stream.tpa`
is not in the repository, in a release or on a player's disk, and the game's audio the retail file already has is
not copied. The records are the voice lines of the game's own English cast. `tools/stream_fix_pack.cpp` made them from
the download and the retail file; the download is not needed to build or run the port. The translations of the restored
lines were written for the port (PC port completion).

## Third-party code

| Project | Licence | How it is used |
|---|---|---|
| [sp00nznet/pcrecomp](https://github.com/sp00nznet/pcrecomp) | MIT | fetched at a pinned commit into `external/` (not committed); its tools produce the lifted C and its `native32` runtime is compiled into the host |
| [sp00nznet/themovies](https://github.com/sp00nznet/themovies) | MIT | `tools/run_lift.py`, `src/host/main.c` and `src/host/diagnostics.c` follow the structure of its `run_lift.py` and `host.c` |
| [sp00nznet/trespasser](https://github.com/sp00nznet/trespasser) | MIT | its reconnaissance documents were the checklist for `docs/ANALYSIS.md`; no code was taken |
| capstone, pefile | BSD-3-Clause, MIT | Python libraries, installed by the user |
| [ocornut/imgui](https://github.com/ocornut/imgui) | MIT | fetched at a pinned commit into `external/` (not committed); compiled into the launcher |
| [TekRantGaming/trg-launcher](https://github.com/TekRantGaming/trg-launcher) | MIT | fetched at a pinned commit into `external/` (not committed) and patched with `launcher/patches/trg-launcher`, the series of Dragon's Crown Pro PC and 13 Sentinels |
| [libsdl-org/SDL](https://github.com/libsdl-org/SDL) 3.4.20 | zlib | fetched at a pinned commit into `external/` (not committed); built as a static library for the launcher window, the game's window input and gamepads, and its audio output |
| [GameTechDev/ASSAO](https://github.com/GameTechDev/ASSAO) at `c21d57a95b43c700ee01a2eb18f5589d7becefb2` | MIT | vendored in `third_party/assao` (three files of the DX11 effect, licence and provenance in its README); compiled into the host as the D3D11 renderer's ambient occlusion |
| [KhronosGroup/Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) `1.3.275` | Apache-2.0 | vendored in `third_party/vulkan` (`vk_platform.h`, `vulkan_core.h`, `vulkan.h`); compiled with `VK_NO_PROTOTYPES` and loaded dynamically at run time by SDL3 for the Vulkan renderer |
| [FFmpeg](https://ffmpeg.org/) libavformat, libavcodec and libavutil n7.1.1 | LGPL-2.1-or-later for the Smacker demuxer and decoders it uses | not in the Windows release (it runs the original SMACKW32.DLL); the Linux release statically links the LGPL-only build of `tools/ffmpeg-bootstrap.sh` (`src/host/video/smacker_backend_ffmpeg.c`), with the LGPL-2.1 text and the source offer in the AppImage (`packaging/licenses`) |
| [OpenSSL](https://www.openssl.org/) 3.5.9 | Apache-2.0 | statically linked into the Linux executable by `tools/openssl-bootstrap.sh` for the Bug Report's HTTPS; the licence text is in the AppImage (`packaging/licenses/OpenSSL-LICENSE.txt`) |
| [memononen/nanosvg](https://github.com/memononen/nanosvg) | zlib | vendored in `launcher/third_party/nanosvg` (from 13 Sentinels), draws the sidebar icons |
| [ip7z/7zip](https://github.com/ip7z/7zip) LZMA SDK 26.06 at `d3296e39e1f1f92c7b41c0c3848e65640428d42d` | public domain (Igor Pavlov) | the 7z, LZMA and LZMA2 decoders vendored in `launcher/third_party/lzma`; read custom level packages |
| Launcher icons and sponsor heart | CC0, CC BY (`display.svg`), MIT (`bug-report.svg`) | `assets/launcher`; sources in `assets/launcher/icons/SOURCES.md`, notices in `packaging/licenses/THIRD-PARTY-LICENSES.txt` |

`src/host/selfmod.c` adapts the register hand-over of pcrecomp's `gen_bridge`.
MIT requires the copyright notice to accompany substantial copies: "Copyright (c) 2026 sp00nz".

## OpenTrespasser and the circulating source

The original development source circulates publicly and
[OpenTrespasser](https://github.com/OpenTrespasser/JurassicParkTrespasser) builds from it. No
licence was ever granted for that source, and none is claimed here.

OpenTrespasser was consulted as a high-level reference, to learn the engine's names and to find which functions and
paths are relevant (`docs/REFERENCES.md` lists what was read and for what). No code from it was copied into this
repository. Every offset, address and behaviour this port relies on was verified independently against the retail
executable and the code lifted from it, and `docs/ANALYSIS.md` and the addresses in the code rest on that verification.
The port's code is its own implementation. This is not a clean-room reimplementation.

## Trespasser CE

Trespasser CE and the CE Patcher were studied statically, as references for behaviour (`docs/REFERENCES.md`); neither
was run. No part of them (executable, DLL, shader, data, menu picture or configuration file) is included, required or
read by the port, and nothing is downloaded. The features the port took as ideas are its own implementation over the
retail executable.

## Official v1.1 build 117 patch

DreamWorks Interactive's official v1.1 (build 117) patch was studied statically, by comparing its executable and level
files with the retail ones (`docs/REFERENCES.md`); it was never run or installed. No part of it (executable, level file,
installer or text) is included, required or read by the port, and nothing is downloaded. What the port keeps from it
are the level values it reproduces (two terrain tolerances per level and the transforms of 178 objects, each stored
with the retail bytes it replaces) and the retail files' sizes and SHA-256 fingerprints that guard them; the executable
fixes are the port's own code.

## Community levels

No community level, fan mission or mod is included in this repository or in the release downloads. Players import levels
from their own files into `user_data/custom_levels`, which Git ignores. Each level remains its author's work.

During development, the levels of the TresCom download catalog were downloaded and analysed locally with the port's
automated compatibility scanner. The project is developed with AI coding assistance, and some of that catalog's files
were processed in AI-assisted sessions, including parts of the scripts of one community level whose author does not
permit the use of their work with generative AI tools.

v0.1.3 shipped script tests modelled on that level's behaviour. They have been replaced by independent synthetic tests
written from Trespasser CE's scripting documentation and the port's own interfaces. The earlier tests, and the earlier
documentation that described the level, remain in this repository's Git history and in the v0.1.3 source archives. The
default keys of the first two script commands (Shift and M) were chosen during that work; since v0.1.4 the eight script
commands default to the keys 1 to 8, a generic choice independent of any level (a key set in `trespasser.ini` is kept).
None of the release programs contains any of that level's files or names.

The development corpus is under review. Packages by that author, including co-authored ones, and packages whose
authorship is uncertain are excluded from AI-assisted work, and the corpus tools skip excluded packages before downloading,
extracting or scanning them ([provenance policy](PROVENANCE.md)). Removing local copies does not withdraw what the Git
history, published source archives or external services already hold.

## This repository's own code

No licence file has been added yet; that choice belongs to the repository owner.
