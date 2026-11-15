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

**Gamepads** work out of the box: Xbox, PlayStation, Switch Pro, Steam Deck and other SDL3 controllers, at the same time
as the keyboard and mouse, with analog movement, a layout built around Anne's arm, menu, Save and Load navigation,
hotplug, and prompts that follow the device used last in Xbox, PlayStation, Steam Deck or generic glyphs. The beach
tutorial's instructions show the bound keycaps or controller buttons as glyphs and switch the moment you change device,
and View shows every control in a level ([CONTROLS](docs/CONTROLS.md) explains the hand, crouching and each button).

The launcher installs the game from your disc image and keeps only the useful choices: resolution, aspect ratio,
anti-aliasing, texture filtering, VSync and renderer, and the gamepad's layout and feel ([LAUNCHER](docs/LAUNCHER.md)).
It is portable: saves and settings stay in `user_data` next to the program ([user data](docs/user-data.md)).

**Languages** are built into the port: Gameplay > Language in the launcher picks one of English, Brazilian Portuguese,
French, German, Spanish, Russian or Italian. Every language has the whole text of the game: game text, the words on the
menu pictures, subtitles, the tutorial and the port's own panels; the voices, effects, music and videos stay the English
originals. Brazilian Portuguese also translates the launcher, at once; the other languages keep it in English
([Languages](#languages)).

Not yet: uncapped frame rate, vibration, mods, x64.

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
assets/        launcher header art, sponsor heart, page icons and the translated text (assets/translations)
packaging/     third-party licence notices
src/common/    settings file and display arithmetic shared by host, launcher and tests
docs/          ANALYSIS, ARCHITECTURE, BUILD, CONTROLS, LAUNCHER, LEGAL, REFERENCES, user-data
external/      pcrecomp checkout (fetched, ignored by Git)
game/          your installed game files (ignored by Git)
user_data/     next to the program: settings and saves (never in a release;
               build\user_data for tools\build.ps1 -Run)
analysis/      regenerated analysis output (ignored by Git)
logs/          one log per run (ignored by Git)
```

## Languages

Languages are built into the PC port: nothing is downloaded or installed. The translated text is part of the executable,
and the launcher's Gameplay page has one Language setting.

| Language | Game text, menus and subtitles | Voices and videos | Launcher |
|---|---|---|---|
| English | English | English | English |
| Brazilian Portuguese | Brazilian Portuguese | English | **Brazilian Portuguese** |
| French | French | English | English |
| German | German | English | English |
| Spanish | Spanish | English | English |
| Russian | Russian | English | English |
| Italian | Italian | English | English |

The port completes the localizations where the releases of 1998 and the fan translations stop: the official
translations have no subtitles, the Russian strings and the German ones miss a few entries, and there is no Italian game
text. Everything else (the subtitles of four languages, the Italian game text, the words on the menu pictures, the
controls card, the objectives, the debug and cheat menus) was translated for the port. Localized audio and videos are
not distributed: the voices stay the player's own English files.

The words on the menu pictures (NEW GAME, RESUME, CANCEL, the Quit and Sound titles, the legal lines of the main screen)
are taken out of the player's own pictures at run time and set again in the language, in the same stencil face and
colours, hover and pressed states included; no picture of the game is shipped. The tutorial's subtitles name the
controls by the button or key of the device in use in every language. The game's own Show Subtitles option still turns
subtitles on and off.

### Translation credits

| Language | Content | Source | Translation | GameStrings conversion |
|---|---|---|---|---|
| Brazilian Portuguese | game text, menu wording | official | DreamWorks Interactive | Nick3069 |
| French | game text, menu wording | official | DreamWorks Interactive | Nick3069 |
| German | game text, menu wording | official | DreamWorks Interactive | Nick3069 |
| Spanish | game text, menu wording | official | DreamWorks Interactive | Nick3069 |
| Russian | game text and subtitles | community | cynodesmus | |
| Italian | subtitles (v1.0.0, timings adjusted by hand) | community | KeyofBlueS | |
| English | caption timings, restored dialogue and its audio ([below](#subtitle-timing-and-restored-dialogue)) | community | KeyofBlueS | |
| All | subtitles of Brazilian Portuguese, French, German and Spanish; Italian game text; missing German and Russian strings; the menus' words where a release had none; the translations of the restored dialogue; the port's own interface | PC port completion | Trespasser Recompiled | |

The port's own translations are not attributed to DreamWorks Interactive, cynodesmus or KeyofBlueS. Where an official
text had an evident defect it was corrected: the French strings have every "è" and "ê" swapped in the converted file,
and the Spanish OK button and the Brazilian Portuguese Credits button of the 1998 menus show the wrong word.

Source preservation and hosting: the [TresCom](https://www.trescom.org/) community, from whose download pages these
come ([Brazilian Portuguese](https://www.trescom.org/download/brazilian-portuguese-official-translation-audio/),
[French](https://www.trescom.org/download/french-official-translation-audio/),
[German](https://www.trescom.org/download/german-official-translation-audio/),
[Spanish](https://www.trescom.org/download/spanish-official-translation-audio/),
[Russian game strings](https://www.trescom.org/download/russian-unofficial-translation-game-strings/),
[Russian subtitles](https://www.trescom.org/download/russian-fan-translation-subtitles/),
[Italian subtitles](https://www.trescom.org/download/italian-fan-translation-subtitles/)). Only the text of those
packages is used: no audio, menu picture, video or program. The About page lists the same credits. Czech subtitles, the
Fargus and Lost Jungle Russian translations and the Italian cutscene packages are not included. The licence of these
downloads is not stated ([LEGAL](docs/LEGAL.md)).

## Subtitle timing and restored dialogue

The data in this section comes from the *English Stream TPA Fix (Subtitle Timing Fix)* by KeyofBlueS, preserved and hosted
by [TresCom](https://www.trescom.org/download/10423/) (March 14, 2026). Its author's description: a `Stream.tpa` with
adjusted caption timing, which also restores previously cut dialogue lines.

What the port took from it:

- **Corrected caption timings.** Of the 402 voice captions of the retail `Stream.tpa`, 328 are corrected (303 only in
  timing, 25 also in line breaks, pauses or text); the port applies the corrected captions to the English game
  and gives every other language the same temporal events, so a caption appears and ends at the same moment in English,
  Brazilian Portuguese, French, German, Spanish, Russian and Italian. Where the fix splits, merges or adds a caption
  line, the translations were split, merged or given the same pause. The timings of the Russian release, which used
  placeholder durations, were replaced.
- **Restored English dialogue audio.** The retail files lack the audio of four voice lines that the retail levels call
  (`VA24` and `VA34` in the `jr` level, `VA208` in `as2`, the tour guide in `be`) and hold only a short
  version of the Ray computer voice (`VCRAYCOMPUTER`, 10.4 s against 33.8 s with its dialogue). These five records, 4,159,690
  bytes of uncompressed PCM exactly as the fix stores them, are compiled into the executable and presented to the game
  as part of `Stream.tpa` in memory. The player's own file is only read; the audio the retail file has is never replaced
  except `VCRAYCOMPUTER`, and only when its bytes are the known retail ones.
- **Captions the retail file lacked for audio it has** (the phone operator, a diary line, "At center court...").
- The restored lines stay English; only their subtitles are translated. All restored dialogue is translated in every
  supported language (the translations of the restored lines are the port's own, marked as PC port completion).

What it did not take: the modified `Stream.tpa` itself is not distributed and nothing is copied at start-up. No external
translation or audio package is needed, and nothing is downloaded. The fix holds 69 more samples the retail file lacks
(14.5 MB) that no level of the retail game triggers; they are not included because nothing can play them. A restored caption is shown only when
its audio exists, in the player's file or in the embedded records.

Size: restored audio 4,159,690 bytes, its manifest 1,272 bytes, caption and timing text 119,189 bytes (the canonical
English captions 43,827 bytes, the Russian completion 69,780 bytes, the other completions 5,582 bytes); about 4.28 MB in
the executable (about 3.5 MB more in a release archive).

The developer tool `tools/stream_fix_pack.cpp` made `assets/restored_audio` and `assets/translations/en.subtitles.txt`
from the fix and the retail file; it is not part of the build, and a clean clone builds without the fix.

## Credits

Built on [sp00nznet/pcrecomp](https://github.com/sp00nznet/pcrecomp), following the
reconnaissance of [sp00nznet/trespasser](https://github.com/sp00nznet/trespasser).
See [LEGAL](docs/LEGAL.md) and [REFERENCES](docs/REFERENCES.md).
