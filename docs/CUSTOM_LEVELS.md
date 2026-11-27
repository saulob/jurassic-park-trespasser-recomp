# Custom levels

Levels and fan missions made by the community, imported in the launcher's **Custom Levels** page and played with the
installed game version selected on the **Play** page; there is no second selector. Nothing is added to or replaced in
`game\versions`.

## Import

ZIP, 7z, RAR or an extracted folder; the format is read from the file's contents. RAR is read by the launcher's own decoder
(`launcher/rar_*.cpp`): RAR 2.0 to 4.x and RAR 5 archives, solid or not, with the methods of RAR 2.0 (LZ and audio), 2.9 (LZ,
PPMd and the standard filters: x86, Itanium, delta, RGB, audio) and 5.0 / 7.0 (LZ with the delta, x86 and ARM filters); every
file is checked against its CRC32 or BLAKE2sp. Encrypted files or headers, archives split in volumes, the methods of RAR 1.5,
filters that are not the standard ones and dictionaries over 256 MB are refused as unsupported: extract such an archive and use
Import Folder. A RAR is expanded once into a temporary folder that is removed after the import. A 7z block up to 128 MB is
expanded in memory; a larger one is decoded as a stream into a temporary folder when its coders allow it (Copy, LZMA or LZMA2,
with at most one x86, ARM, ARM Thumb, PowerPC, SPARC or delta filter), otherwise it is refused above 768 MB. When a file of the
package cannot be read, the analysis says so (unreadable, damaged, unsupported) instead of judging its levels. A package is unpacked and checked in a staging folder first, so a failed or cancelled import leaves nothing behind.

A level that is already extracted does not need an archive: Import Folder reads that folder and installs a copy of the files the
level needs under `user_data\custom_levels\<package>`, which is what the port plays. The folder chosen is only read, never moved or
changed. The copy keeps the package's saves, settings and checks apart from everything else, and takes disk space of its own; Delete
removes it and keeps its saves. Playing from an external folder without a copy is not implemented. The game itself is installed from a
disc image only ([LAUNCHER](LAUNCHER.md#import-game)).

| Refused | |
|---|---|
| paths | absolute, drive, `..`, reserved names, the same name twice (any case), a file where a folder is, more than 16 folders deep |
| links | symbolic links and reparse points |
| size | archive 1 GB, file 512 MB, package 2 GB, 8192 entries, 7z block 768 MB when it cannot be streamed, RAR dictionary 256 MB |
| contents | encrypted or unknown archives, no Trespasser `.scn` |

Level data, texts, pictures and Trespasser CE scripts (`.nut`, never run unless a level needs them) are kept; everything else
(programs, libraries, other scripts, ATX or CE components, shipped saves) is left out, counted and named with the reason in the
manifest. In a package that an earlier version installed without its `.nut` files, a level whose world names a script class is refused as
`CE_SCRIPT_FILES` until the package is imported again; its other levels are not. A package with the same files as an installed one is refused
as a duplicate.

`READY` is the result of an automated scan: no known blocking issue was found. It does not mean the level was played, or that its
scripts, triggers, graphics and performance behave as intended.

The rule is binary per level: each scene is `READY` or refused on its own, and a sibling's state never changes it. A package is
analysed in a staging folder first and installed (one rename) when at least one of its levels is `READY`; its list shows every
level, with Play for the `READY` ones and Bug Report for the others (the reasons, of that level only, are in the log). A package
without a `READY` level leaves nothing behind and the page says `Could not import this custom level` / `This custom level is not
fully supported`, with Bug Report and Close. The listing record and `manifest.json` (`level_status`) keep each level's state,
blockers and scripts; the preflight of Play judges the level asked for.

| Class | |
|---|---|
| `READY` | its worlds (`.grf`) are in the package or the game; each world of the package has its texture set (`<world>-130.pid` and `.swp`/`.spz`/`.spp`), or the installed game has the set of that name, or loose pictures are in `Map\<world>`; the terrain data its world names (`.wtd`) is there; its textures are found, or only some are not (see below); no feature, setting or ActionType the port does not have |
| `REJECTED_MISSING` | a required file, texture set or terrain is nowhere, or none of a world's textures is found (its texture set is not that world's) |
| `REJECTED_UNSUPPORTED` | something its content requires is not `FULLY_SUPPORTED` or `NOT_REQUIRED` in `src/common/jpt_level_features.c`, a scene format newer than 26, or content beyond the engine's limits |
| `REJECTED_MALFORMED` | a scene or world that cannot be read |
| `REJECTED_OTHER` | anything else the port cannot vouch for: settings files in conflict or unreadable, a readme instruction that cannot be followed, two files for one game path |

Information never refuses: a readme, the author's own note (Restart Level does not work), a world without terrain as the level
was made, a scene 23 to 26 read as the game's own, a dependency only mentioned or recommended, heaps and texture pool sized for the
level, and some textures of a world that are nowhere (`MISSING_TEXTURES`): the level plays as in Trespasser CE, which draws its grey
texture for each of them; and a level a trigger loads that neither the package nor the game has (`MISSING_TRANSITION_LEVEL`):
the launcher's row says where the level leads, and when the trigger fires the game stays in the current level with the notice
"The next level is not included in this package". Such lines are logged with `info:`.

A package installed by an earlier version is analysed with today's rule: refused, it shows `Not supported` with Bug Report, Open
Folder and Delete, and no Play. Nothing of it is removed. The list is ordered by the date of import (newest or oldest first) or by
name; a package of an earlier version takes the date of its oldest file.

The page lists the packages off the UI thread. Its first frame shows `Loading custom levels...` with a spinner, the scan starts
on the next one and the page adopts the result when the job ends (a result asked for before a newer request is dropped). An
import runs on its own worker and shows the launcher's task bar in place of the import buttons: its phase (Reading the package,
Copying the level files, Checking the level files), a percentage that only rises (copying moves with the bytes, a phase without
counts keeps what the one before reached), Cancel, and 100% only when the package is installed. Each package keeps `.runtime\listing.txt`, its listing record, under a key
made of its files (path, size, time), the installed game's files, the analysis version and the engine's limits; while the key
holds, the package is not analysed again. The log has one line per refresh with the counts and times.

### Before Play

When Play is pressed the level is checked with the runtime's own lookup, and again by the game's host before every level
load (`src/common/jpt_level_gate.c`), while the game is still as it was. A level that fails is not started: one sentence, Bug
Report and Close. Bug Report selects this session's log, where the reasons were just written. The log has the package, the level, the file, its type, where it was looked for and what it is needed for.

| Checked | |
|---|---|
| world | read whole and bounds checked (`src/common/jpt_groff.c`): the engine builds it from these bytes without a look at them |
| terrain data | the file its `TerrainPlacement` names; without it the engine goes on with a null world |
| texture set | each texture by the engine's own key; a missing one is a warning |
| curved bump maps | in the set by the engine's own key: see below |
| engine limits | mesh arrays and their indices, 58 corners a polygon, 16 actions a trigger, 16 elements an expression, 10 submodels of a compound, 10 boxes held by magnets, 32 joints and what they link, substitutes of a skeleton, 64 faces in a point, 64 points of a planted mesh, animated surfaces, 95 characters a name, the memory of its meshes (128 MB) and textures (224 MB), palettes, 260 characters of paths |
| counts of the scene | animals and joints against their sections |
| level name | 19 characters, what the game keeps |

A level that comes without a texture set uses the installed game's of the same name, which may have been made for another build of
the world. Polygons with a curved bump map that such a set lacks would be built from a source that does not exist. In memory, as
the world is read, they are lit flat when their texture has a picture of its own; a texture without one is read as a missing
texture (`src/common/jpt_level_world.c`). The files stay as they are.

### Bug Report

Levels with warnings or errors have their own Bug Report button, with the log of that level's last run chosen, else this
session's. A failed import or Play chooses this session's. A log picked by hand stays chosen.

The outcome of an import stays below the import buttons until it is closed or another import starts. The technical side is in the
log alone.

The log holds the whole import under `custom level import:`: source, size and SHA-256 of the archive, every text of the package
(a readme between `README BEGIN` and `README END`, any other between `TXT BEGIN` and `TXT END`, each once, up to 64 KB of each;
never run or followed from the log), every file by type, what was ignored or rejected and why, runtime paths, each level with
its scene version, files and what of its format is left out, settings files, dependencies, session settings and the result
(`installed`, `rejected`).

```text
user_data\custom_levels\<package>\
    ...            the package's own files, folders and names, never changed
    manifest.json  what the analysis found (schema_version, source_hash, levels, settings, actions, warnings)
    runtime.map    what the game reads of it: each game path and the package's file that answers it
    .runtime\      loading pictures in the game's own format, the world facts and the listing record
```

`manifest.json`, `runtime.map` and `.runtime` are generated. The launcher makes them again when they are missing, damaged or
no longer match the package, which is how a package installed by an earlier version gets them. A manifest of a newer schema
is left alone. Without a `runtime.map` a package is still read by its folders.

## Analysis

One implementation (`launcher/level_analysis.*`) serves the import, the page and the command line:

```powershell
.\tools\analyze-custom-level.ps1 <ZIP, 7z, RAR or folder> [-Json]
.\tools\analyze-custom-level.ps1 <folder of packages> -All [-Report <folder>] [-Json]
trespasser_recomp.exe --analyze-custom-level <ZIP, 7z, RAR or folder> [--json]
trespasser_recomp.exe --analyze-custom-levels <folder of packages> [--report <folder>] [--json]
```

It installs nothing, opens no window and runs nothing of a package. The second form reads every package below a folder (ZIP, 7z, RAR,
extracted folders, at most 12 levels deep), tells copies apart, and reports for each level its files and where they come from, its
class (`READY`, `REJECTED_MISSING`, `REJECTED_UNSUPPORTED`, `REJECTED_MALFORMED`, `REJECTED_OTHER`) and its package's, its
sizes against the 1998 limits and the most frequent problems; with `-Report`, as text and JSON.

| Layout | Mod folder |
|---|---|
| `loose` | the folder of the scene |
| `data_folder` | the folder that holds `data` |
| `fan_mission` | the folder of `fm.ini`, or the one the readme puts in `mods` |
| `mod_tree` | `ActiveFM` of `tp_mod.ini`, under `FMPath`, beside the file, or the only folder of that name |
| `standalone` | the same, in a package that carries its own copy of the game; that copy is not used |

Files are searched as Trespasser CE searches them: `UberModPaths`, the active mod (its `data` folder, then its own folder),
`ModPaths`, then the installed game. Every layer is a folder of the package; other packages are never seen.

### Scene formats

The game's own scenes are version 22. Trespasser CE writes 23 to 26, and what differs is known section by section from its
loader; a later scene is read as a version 22 one, in memory, when the game opens it (`src/common/jpt_scene.c`). The package's
file stays as it is, saves are the game's own version 22.

| Version | Differs from 22 | |
|---|---|---|
| 23, 24 | nothing the loader reads | |
| 25 | render section 4 (a colour, a mask, optional tables), water block 2 (per-second values), texture swaps of animated meshes | water and meshes converted, the rest left out |
| | AI section: active animals counted instead of 10 places | converted; past 10 they start inactive |
| | CE's own input and action sections | not read by the game's loader |
| | player section, marked: the second stowed object's placement | left out |
| 26 | a script mark after the common part of every object | read past (`src/host/scene_scripts.c`) |
| | CE's script section | not read by the game's loader |

Sizes, counts, offsets and the closing of each section are checked; a scene that does not close is `Unsupported`. A format
version says nothing about what a level needs: dependencies come from the package's own files.

### fm.ini

| Key | |
|---|---|
| `StartLevel` | the package's first level |
| `StreamTPA+=`, `EffectsTPA+=`, `AmbientTPA+=` | samples added to the game's own file; one of the same ID stands for the game's |
| `StreamTPA=` ... | the file stands for the game's own |
| `[level.scn]` | the same keys for one level, and `LoadingImage` (TGA of 16, 24 or 32 bits, any size) |
| `WinMovie=0` | the ending video is skipped |

### Settings

| In the manifest | |
|---|---|
| `mapped` | `EnableDualStow`, `EnhancedSky`, `WaterRefraction`, `MaxActiveAi`: the port's setting, for this session only |
| `built_in` | already so: quick save, no installation check, no opening videos, own saves |
| `recommended` | `ForceMaxObjectDetail`: the player's setting stays |
| `user_choice` | renderer, GPU, resolution, MSAA, VSync, sound device, controls, language, everything in `tpass.ini` |
| `obsolete` | memory sizes, draw distances and debugging of the engine the file was written for |
| `session_limit` | `MeshHeapSize` (40 to 128 MB, raised to what the level's meshes need), `ExtraTerrainNodes` (thousands, up to 300): reserved by the host for this session |
| `required` | `DisableSimpleLevelRestart`, `EnableAiActivityEx` and others in `trespasser.ini`, `EndGameDDF`, `MenuTPA`: a feature that is judged by its status |
| `ineffective` | a key that Trespasser CE does not read where it stands |
| `unknown` | any other key |

`fm.ini` goes before `trespasser.ini`. `trespasser.ini`, `trespasser.<name>.ini`, `tpass.ini` and `tp_mod.ini` are read inside
the package and copied nowhere; nothing is saved to the launcher's settings or the registry.

### Readme files

A fixed set of sentence shapes over the package's own file and folder names (`launcher/level_readme.*`); the text is never
run. The destinations are `data`, `data/anim`, `data/menu`, `data/texhw`, `maps`, and "in the mods folder", which names the
mod. A file named twice, a destination that is not one of these or a file that does not fit it moves nothing and is noted
(`readme_not_followed`).

## Play

### Minimap and objectives

The launcher writes a guide of every level it can start, `.runtime/guide/<scene>.txt` (`launcher/level_guide.cpp`), after the
package's analysis and, when one is missing, before Play. Its minimap is made from the level's world by the rules that made the
campaign's (`tools/minimap_bounds.py`, `tools/minimap_map.py`; the retail levels come out identical). Its objectives follow the
level's own triggers as the campaign's do: each hint the level sets leads to the triggers that set the next one, the last to its
exits (`LoadLevel`, `EndGame`); a level that sets no hint has its exit as its one objective. A title is the first sentence of the
hint's text in the game's language, else `Objective <n>`.

The campaign's minimap and objectives hold only for a level whose world is the installed game's own (a scene over the game's
`as.grf`, or a level of the game the package leads to). A package's level named as one of the campaign's (`as2.scn`, `be.scn`)
with a world of its own gets its own guide, never the campaign's. Without a guide, or when the world gives no map or no
objective, the minimap shows "No map information" and Show Objective "No objective information". The campaign never reads a
guide.

### Files

A package is active for one run. The game still asks for `<Data Drive>\data\X` and the package answers first
(`src/common/jpt_custom_level.c`), the original files second. The names in a package may differ from each other
(`LostJungle.scn` with `LJ.wtd`) and be in any case; two files told apart only by case answer by their exact name alone.
Files outside `data` (`Map\<level>\x.bmp`, any other folder of the mod) are answered by their path below the game's folder.

A texture the level's set (`.pid`, `.swp`) lacks is asked for by its path, `Map\<level>\<name>.bmp`; `<level>` is the folder in the
request, from the name of the world, and the game's current directory plays no part. The places are Trespasser CE's
(`jpt_custom_level_texture_try`), each in the package (the mod's folder before its `data` folder, a layer before the next) and
then in the installed game:

1. `texhw\Map\<level>\<name>.bmp`
2. `texhw\Map\<name>.bmp`, shared by every level
3. `Map\<level>\<name>.bmp`

The `texhw` places exist only for a package that has `texhw` files of its own. The first 8 bit uncompressed BMP answers. Names match in
any case, `..` and drives are refused, and another level's folder is never searched. A `.dds`, `.tga` or `.bmp` of `texhw` also takes the
place of a texture of the set where the hardware renderers draw it (`src/common/jpt_texhw.c`, `src/host/texhw_guest.c`): DXT1 to DXT5
and uncompressed pictures with their mips, the texture's own format deciding its alpha. A `.norm.dds`, `.norm.tga` or `.norm.bmp` beside
that picture is its normal map, its alpha the share of the highlight: the polygon is lit per pixel with Trespasser CE's own formula by the
composite of the level's lights (`src/common/jpt_lit.c`), in Direct3D 11 and Vulkan. Meshes with a bump map of the original game are drawn
by the engine itself, so overrides of those are not supported. For a texture that is nowhere the game reads Trespasser CE's own stand-in, 4 x 4 and grey, made in memory
(`src/host/missing_textures.c`); no file is written and nothing that exists is hidden by it. The log names each one once, with the
level, the package and every place. Textures of one name are never mixed with the original game's. While a package is active nothing under `data` is written: the
game's own `.asb`, `.gop` and `Map` files are refused.

- Windows: `CreateFileA`, `GetFileAttributesA`, `LZOpenFileA`, `_open`, `fopen` and MSVCIRT's `_sopen` (terrain and
  animation scripts). Level files and paths LZ32 cannot take (128 characters) are read by the host.
- Linux: `jpt_vpath_resolve_read` with an overlay on `C:\data`.
- Sound files: one virtual file per handle (`src/host/stream_vfile.*`, `src/host/tpa_merge.*`); the corrected captions and the
  restored audio stay under the added samples.

The scene is loaded by the same calls as Level Select (Direct Load) on the main menu's first turn, when the engine is ready and
before the menu has drawn. Nothing is presented until then (5 seconds at most), so the loading screen is the first picture. The
opening video is skipped for that run. Saves and level swap files go to `<saves>\custom_levels\<package>`; the game's own settings stay
the version's. A scene whose name the game cannot keep in a save (more than 16 characters, or other than letters,
digits, `_` and `-`) is loaded as `LEVELn.SCN`.

A package that names its `StartLevel` is one row, which starts it; its other scenes stay installed for the mission's own
transitions. Any other package has a row for each playable level. `Installed levels (N)` counts the rows.

The engine builds each mesh in fixed arrays (2048 polygons, 8192 vertices) without checking them, and community levels go past
them: Misty Jungle has a mesh of 2368 polygons. A package whose meshes need more gets arrays of that size, up to 8192 polygons
(`src/host/engine_heaps.c`), and one whose loaded meshes need more than the engine's 32 MB, or whose `MeshHeapSize` says so, a larger
heap. `ExtraTerrainNodes` goes to the terrain's tree, the tree of its queries and its scaling arrays as in Trespasser CE. A texture set
beyond the engine's 128 MB pool gets a larger one, up to 256 MB. The buffer that the sections of scenes and saves pass through,
1 MB, has 4 MB in every custom level's session: that of some 130 000 objects. A package that fits asks for none of it and runs with the engine's
own sizes. The log has the limits of the session.

### Trespasser CE and ATX belong to the session

What Trespasser CE and ATX add to the game is never a mode of the port. Each piece is a capability of the running custom level
(`jpt_level_cap_t`, `src/common/jpt_custom_level.h`): the package's content asks for it, and the place where it takes effect asks
`jpt_custom_level_requires`. Nothing is on because the code for it exists.

| Capability | Asked for by |
|---|---|
| `scene_format`, `script_state` | the version of the scenes that are read (23 to 26; 26) |
| `texhw` | `texhw` files in the runtime map |
| `sound_added`, `sound_replaced` | `StreamTPA`, `EffectsTPA`, `AmbientTPA` with `+=`; with `=` |
| `loading_image`, `skip_ending` | `LoadingImage`; `WinMovie=0` |
| `dual_stow`, `water_refraction`, `enhanced_sky`, `active_ai` | the settings of that meaning |
| `mesh_faces`, `mesh_heap_mb`, `terrain_nodes`, `texture_pool_mb` | the size of the content, or `MeshHeapSize` and `ExtraTerrainNodes` |
| `terrain_cull_mm` | `TerrainBackfaceCullBias` above 0 |
| `full_restart`, `sky` | `DisableSimpleLevelRestart` in `trespasser.ini`; a `.sky` file |
| each ActionType from 99 to 121 | the triggers of the package's worlds |

- Without a custom level there are none, and every lookup, setting and limit of this layer answers nothing.
- A session is what started it, not what it loads. One that a package started is custom to its end: the package may use files of the
  original game and lead to its levels, and its capabilities, sounds, textures, settings, limits and saves hold there too. The
  original game's own session, Play in the launcher, is a process without any of this.
- When the session ends (`jpt_custom_level_deactivate`) its files, sounds, settings, limits and what the hosts kept of them
  (`jpt_custom_level_on_end`) are gone; another package starts from none.
- Safeguards are not capabilities: the checks of saves, levels, heaps and allocations, and the grey picture for a texture that is
  nowhere, only keep a load from corrupting memory. Those that end a load or stand in for a file act in a custom level's session alone.

### What a load ends with

| | |
|---|---|
| `BLOCKED_PRECHECK` | refused before the engine is asked: nothing of the game changed |
| `CONTROLLED_LOAD_FAILURE` | ended by the host while loading or playing, before anything was written: `ENGINE_LIMIT_EXCEEDED` (no room) or `ALLOCATION_FAILED` (no memory) |

Every array and heap of the engine counts its elements, asks for their pages (`pvCommitMem`) and only then writes them. The
request is measured against the reservation first (`src/host/engine_heaps.c`); a commit that would leave its own region, a failed
`VirtualAlloc`, `operator new` or `malloc` is caught where the game asks the system (`src/host/alloc_shims.c`, Linux in
`linux_hle.c` and `linux_msvcrt.c`). The session is then ended (`src/host/level_abort.c`): one sentence for the player, the cause in
the log, none of the game's destructors run over what is partly built, and the process ends with it. A scene of more polygons than
the engine holds leaves out the shape without room, as the game does. Outside custom levels nothing of this applies.

Behind each heap of such a session is a guard that is never committed. No known limit reaches it; a write there is a bug of the
port, marked `GUARD_PAGE_HIT` in the fault report.

### Saves

The game's reader trusts every size and count of a save. One whose sections or names do not close, of another version or without
objects is neither listed nor loaded. A save is written to another file and takes the place of the previous one when it is
whole; otherwise the previous one stays and the player is told. In a custom level's session the package and the content of its
worlds are kept behind the save's own data, which the game does not read: a save of another package, or of one whose worlds
changed, is refused (`save incompatible with current custom level`). Saves without that mark, made earlier, are taken.

### ActionTypes, skies and menus

The actions of ATX and Trespasser CE (`src/host/level_actions.c`) are made by the trigger factory as marked objects of the engine's own
hint action: slow motion (100), game speed (101), map picture (102, shown below the objective by Show Objective, F4 or D-pad Down; never in the minimap), reload (103), health (104), video (105), hint (106), particle
colour (109), fixed speed of an object and its end (110, 111), controls (112), AI type of the player (113), randomized teleport (114),
use of an object (115), jump of an animal (116), particles (117), texture swap (118), fire count and probability of a trigger (120) and
reload of any gun (121). Not supported: scripted actions (99). Where ATX and Trespasser CE differ it is as in Trespasser CE: 120 takes a
fire count of 0, 121 takes every gun it names. They keep no bytes in scenes or saves of the game's own format; what they set is behind
the mark of the package's saves, and what they changed in the game's objects is put back before each load. A video (105) is played on the
next frame in the game's own video window, from the package's `menu` folder or the game's. Numbers that neither ATX nor Trespasser CE
knows do nothing, as there; neither do 107, 108 and 119 (rain, an option of ATX's that is off by default). A level's `.sky` (`src/common/jpt_sky.c`) is
drawn by the hardware renderers: DOME, BODY and STRIP layers with the `OPAQUE`, `ALPHA`, `BURN`, `2X_ALPHA` and `2X_BURN` blend modes. A
level with STARS or METEORS layers is not supported.

A map action names its picture as ATX reads it, `MAPS\<name>`: the package's file of that path below its own `MAPS` folder, or else its
only file of that path anywhere, is installed below the package's virtual `maps/`, whatever its readme says; a name that leaves the folder
is never read, and a picture that is nowhere or no picture leaves the map as it was, as in ATX, without refusing the level. The lookup takes
any case and `\` or `/`. Every uncompressed layout GDI draws is read: the 12-byte OS/2 header and the 40-byte one or later, 1 to 32 bits.
Nothing of it outlives the session.

Of ATX's `MAPS` folder the port reads what Trespasser CE reads: the map pictures, `LBG/<level>.tga` (or `LBG/default.tga`), the
picture while the level loads, and `LVL/<level>.ini`, the sound files it adds. Animals may name 30 kinds of sounds instead of the 1998
game's 7 (`src/host/ai_vocals.c`). What ATX has off by default is not asked for: `HUD`, `RAN`, `NMD`, `RNE`, the texts of its menus. That
a readme requires ATX refuses nothing by itself. A package it says so of is refused for what neither the port nor Trespasser CE has:
`JMP/<level>.ini`, `SPT/<level>.ini`, plugins, and rules of the game changed in its own `ATXconfig.ini`.

Menus, menu sounds and videos that no action plays are kept in a package and not used: the launcher and the game's Level Select start every level of a
package, and a session starts in its level. `tools/custom-level-compatibility.ps1` prints and
writes the state of a whole folder of packages.

Trespasser CE scripts ([CE_SCRIPTS](CE_SCRIPTS.md)) are read from the `scripts` folder of each game path of the package (its mod
paths, the fan mission, its uber mod paths: CE's load order, the lowest priority first), and judged per level, never per package. A
level needs them when its world names a script class (an object's `Script`, a trigger's action 99 `ScriptAT`) or when a game script, or
a level script for that scene, acts in it; then the analysis lists its classes (`SCRIPT_CLASS`), each file's required script functions
(`SCRIPT_REQUIRES`) and refuses it as `CE_SCRIPT_API` while one of them is not in the port yet: the names the port has are the
registry `src/common/jpt_script_registry.inc`, the same table the VM is checked against. A level that needs none is played
without a VM, beside levels of the same package that need one. The runtime map lists the scripts (`nut`) and what the worlds name
(`script`); the game decides again with the same code (`src/common/jpt_script_plan.cpp`). A listing made before a name was added
is judged again: the registry is part of the listing's key.

Restart Level in a custom session restarts the current level whole: the world is purged and the level is loaded again through the
game's own level load, with its loading screen, as when the session entered it. The player, objects, triggers, AI, physics and the actions
of ATX and Trespasser CE start over; the session keeps its package, capabilities, settings and saves. The level is the one the game names
now, so after a transition the second level restarts, and a level of the original game the package leads to restarts as the game's own
files within the same session. The 1998 Restart Level only read the scene again over the world it had, which left a custom level's player
and objects where they were; that is why some readmes say Restart Level does not work. Such a note is information and refuses nothing.
`DisableSimpleLevelRestart` also purges the world before a save of the same level loads. The campaign keeps the game's own Restart Level.

The Readme button opens the original file in the system's own application.
