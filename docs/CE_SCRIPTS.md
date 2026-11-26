# Trespasser CE scripts (Squirrel)

Status: phase 0 (a level needs scripts only when they act in it), phase 1 (the interpreter) and phase 2 (the lifecycle of a custom
level's VM, its saves and loads) are done. Phase 3, CE's engine API, is pending: a level whose scripts call any of it is refused as
`CE_SCRIPT_API`, with the names it needs ([CUSTOM_LEVELS](CUSTOM_LEVELS.md)).

## Scope

Scripts run in custom sessions only. The original game's levels have no `.nut` files and no object names a script class;
every CE script feature acts through a script, so there is nothing for the campaign to gain and no option for it. A campaign
session never creates the script session: no VM, no parser, no callback, no budget, no script state in its saves, nothing per frame
(`src/common/jpt_level_scripts.cpp` returns at its first test; `script_session_tests` proves it before and after a custom session).

The port implements the API that Trespasser CE documents, as documented. No CE code, data or file is used.

## Source of the API

Trespasser CE 1.0.135 ships its scripting documentation with the CE Patcher install (`doc/scripting/`); CE 1.07e
(`tpass-ce_v107e`) has the same files and its binary only:

| File | Contents |
|---|---|
| `ScriptingReadme.txt` | the 4 kinds of scripts, the load order, persistent state (`nvram`), key binds, every event handler |
| `API-reference.txt` | Vector3, Rotate3, the entity class tree (CPartition to CScriptTrigger), `TP_Engine`, `CAISystem`, `NMagnetSystem`, constants and enums |
| `samples/*.nut` | 7 samples, among them `Spitter.nut` and `ATX102.nut` |

The names `API-reference.txt` declares (classes, namespaces, functions, members, enums and their constants: 898) are the table
`src/common/jpt_script_api.inc`. The interpreter is Squirrel 3 (MIT licence, squirrel-lang.org), built from source with the host.

## Script kinds

| Kind | Created by CE | In the port now |
|---|---|---|
| GameScript | one per class found, for every level of the session; `nvram` crosses level transitions | made in every level whose VM exists; its `nvram` travels between levels, VM or not |
| LevelScript | one per class, limited by an optional `static SCN` | made in the levels it applies to |
| ActionType | per action 99 of a trigger, class named by `ScriptAT` | needs CE's API (the `ActionType` class): the level is refused |
| Instance script | per object whose `Script` property names a class derived from its engine class | needs CE's API (the engine classes): the level is refused |

## Phase 0: which scripts a level needs (done)

`src/common/jpt_script_plan.cpp`, used by the launcher and by the game alike, reads the scripts without running them: their classes
and bases, the handlers they declare, `static SCN`, the globals each file declares and the names it uses (comments and strings left
out, locals and parameters never taken for engine names).

* Scripts: every `.nut` of the `scripts` folder of each game path of the package (mod paths, the fan mission, uber mod paths), the
  lowest priority first and by name within a folder, as CE loads them; `.nut` files anywhere else (documentation samples) are never
  scripts. A class declared twice is the later one's.
* A level needs scripts when its world names a script class (`Script`, `ScriptAT` of action 99), or when a game script, or a level
  script whose `static SCN` names the scene (file name, any case; none or a faulty one: every scene), declares a handler besides its
  own state (`constructor`, `destructor`, `OnScriptInit`, `OnPrepareSave`, `OnWorldPurge`) or calls CE's engine. A game or level
  script that only keeps its own state changes nothing in a level.
* The files a level loads: those that declare its classes and their bases, and every file whose globals that code reads, in load
  order. Never the package's other scripts.
* Required script functions: the names of CE's API the loaded code uses, and the handlers its classes declare that the port does not
  call yet, per level and per file. A class the world names that no script declares, or one of the wrong kind, is a problem of the
  level. The launcher refuses a level with either (`CE_SCRIPT_API`, `CE_SCRIPTS`) and lists them (`SCRIPT_CLASS`, `SCRIPT_REQUIRES`).

## Phase 1: the interpreter (done)

`src/common/jpt_script_vm.cpp` and `jpt_script_heap.cpp` over Squirrel, built by `cmake/squirrel/CMakeLists.txt` as `jpt_script`.

* Squirrel is fetched like the launcher's sources (`config/launcher_deps.txt`: commit `f9267f2`, after the `v3.2` tag; MIT)
  and patched by `launcher/patches/squirrel`: `0001` the instruction budget, `0002` a vector size that would overflow
  (`array(-1)`, `array(0x7fffffff)` on x86) reaches the allocator as an impossible size instead of wrapping. Its sources
  build without the project's warnings, as Dear ImGui; the port's files build with them as errors (`/W4 /WX`, `-Werror`). On MSVC
  the folder uses `/EHs` instead of `/EHsc`: a refused allocation is an exception that crosses Squirrel's `extern "C"` API, which
  `/EHsc` turns into the end of the process. Windows x86 has CE's 32-bit integers; the 64-bit Linux host has Squirrel's 64-bit ones.
* The base, math, string and blob libraries only; no file, system or auxiliary library. Removed from the base library:
  `seterrorhandler`, `setdebughook`, `enabledebuginfo`, `setroottable`, `setconsttable`, `newthread`, `suspend`.
* Each entry into a script (loading a file, a callback) gets 1,000,000 instructions. Running out is an error that `try` and
  `catch` cannot keep alive, also inside callbacks such as a sort's comparator; loops and recursion are instructions too.
* Memory: 16 MB per VM (`JPT_SCRIPT_DEFAULT_MEMORY`), the whole VM's: Squirrel has one heap per VM and its objects, strings and
  closures are shared by every script of it. An empty VM takes 45 KB; all the CE scripts of the corpus load in 140 KB at their peak
  on x86. Every block carries a header with its heap and size and is charged to that heap, header included; the heap keeps the
  bytes held, the peak, allocations, reallocations, frees, bytes freed and refusals by budget and by the system. Sizes are checked
  before any sum, so no count wraps.
* An allocation past the budget is refused before it reaches the system; the VM is abandoned (none of its code runs again, every
  block it held is freed) and every later call answers `JPT_SCRIPT_DISABLED`. A VM that cannot open within its limit is not created.
* `print` and `error` go to the log as `[script] print:` lines, 200 per level, control characters as spaces. Files over 1 MB are
  not read; a UTF-8 BOM is skipped. Instances are made from the root table's classes, their methods called with this, booleans,
  floats and strings, and their `nvram` read and written as values (`jpt_script_state.h`).

## Phase 2: the lifecycle (done)

`src/common/jpt_script_session.cpp` keeps one VM for the level being played, `src/common/jpt_level_scripts.cpp` connects it to the
host's hooks, on the game's thread (no thread of its own):

| Host event | Hook |
|---|---|
| session start | `level_session_start` (`src/host/level_session.c`): the package's `nut` and `script` lines of its runtime map |
| a level begins to load | the game's level load (`src/host/loading_hd.c`), with the reason of the `[level]` line; the fallback Restart (`level_actions.c`) |
| the load returned | the same hook, after it |
| a simulation step | `level_actions_step`: the game's simulation time since the last step, nothing while it stands still |
| a save is written | `jpt_save_finish` (`src/common/jpt_save.c`), stamped saves of a custom session only |
| the session ends | `jpt_custom_level_deactivate` |

A level begins:

1. The VM of the level before, if any: at a transition `OnLevelEnd(true)` on its game scripts then its level scripts, and their game
   scripts' `nvram` is kept; then `OnWorldPurge` and `destructor` on every script, and the VM is destroyed (its heap back to zero).
2. The level's plan (phase 0), over the files the launcher found for that level (`uses` lines of the runtime map). A level without
   any reads no `.nut`, has no VM and logs nothing. Its scripts need what the
   port does not have: no VM (the launcher refuses such a level; a transition into one plays it without scripts).
3. A new VM: the base classes `GameScript` and `LevelScript`, the level's files in order, then one instance of each game script and
   each level script that acts in the level (`constructor`), in load order, game scripts first.
4. Their `nvram`: game scripts get what travelled from the level before (a transition), the level's start (Restart Level) or the
   save; level scripts get the save's. Saved slots are put into the table the constructor made; slots it has that the save does not
   stay as they are (`ScriptingReadme.txt`).

The load returned: `OnScriptInit(bSaveGame)` on the game scripts (true after a save or a transition, since their saved `nvram` is
what they continue from), then on the level scripts (true after a save); `OnGameStart` on the game scripts of a new game (the
package's first level, or one chosen to start from); then, unless a save was loaded, the game scripts' `nvram` is kept as the
level's start and `OnLevelStart` is called on the game scripts, then the level scripts. A load that failed destroys the VM.

Every simulation step, `OnSimStep(seconds)` on the scripts that declare it, each with its own instruction budget.

Restart Level (always a full load in a custom session, as CE's `DisableSimpleLevelRestart`): the VM is destroyed and a new one made;
level scripts start from their constructors, game scripts from the `nvram` they had right before `OnLevelStart` the first time on
that level (`ScriptingReadme.txt`), `OnScriptInit(false)`, `OnLevelStart`. A transition into a level of the original game in the same
custom session follows the same rule as any level: a VM only if a game script or an unfiltered level script acts in it.

Where the port differs from CE, and why:

* One VM per level, not per session: a game script's members outside `nvram` start again at each level (CE keeps its instance
  across a transition); its `nvram`, all CE saves of it, travels.
* `OnScriptInit` and `OnLevelStart` run after the game's level load returned, so after the start triggers (CE calls
  `OnScriptInit` before them). Without CE's API a script cannot see the difference.
* `OnLevelEnd` is called at transitions only. `OnLevelTransitionRequested`, `OnGameCompleted`, the paint, input, console and modal
  handlers, and every handler of instance and action scripts are not called yet: a level whose scripts declare one is refused, with
  its name.

### Errors and budgets

* A compile or runtime error, or a script out of its instruction budget, stops that script until the level ends; the others and the
  game go on: `[script] level=<scene> script=<class> file=<file> callback=<handler> line=<n> result=disabled error=<text>`, once.
* Memory past the VM's budget, or refused by the system: the VM is abandoned, its memory freed, no script of the level runs again:
  `[script] level=<scene> script=<class> callback=<handler> result=vm_disabled error=memory budget exceeded (limit N bytes)` and
  `[script] level=<scene> result=vm_disabled every script of this VM stops`. The next level has a new VM. The game scripts' `nvram`
  keeps what it was before.
* A script file that is gone: `result=missing`; a class that is not one once loaded: `result=disabled`. No line per frame.

The lifecycle lines: `[script] vm create level=<scene> generation=<n> scripts=<n> files=<n> memory_limit=<bytes>`, `[script] loaded
file=<file>`, `[script] callback level_start|level_end|script_init level=<scene> scripts=<n>`, `[script] save level=<scene>
scripts=<n> bytes=<n>`, `[script] vm destroy level=<scene> generation=<n> peak=<bytes> abandoned=<0|1>`, `[script] session end vms=<n>`.

### Saves

Before a save is written, `OnPrepareSave` on every script; then the `nvram` of each game script and level script, by class, and the
game scripts' `nvram` at the level's start. Nothing of the VM itself: values only, of the types CE saves (int, float, bool, string,
blob, arrays of the first four, and null); arrays are cut at 65536 elements and blobs at 256 kB as in CE, other values are left out
with one `nvram slot not saved` line.

The block (`src/common/jpt_script_state.cpp`) follows the stamp of the package and the actions' state (`JPTSTATE`) in stamped saves
only, so never in a save of the campaign: `JPTSCRPT`, its size (at most 16 MB), then version 1, the number of scripts and for each its
role (current or level start), kind, class, object and slots (name, type, value), little endian. It is written with the save, whole or
not at all (`jpt_save_finish`). A save of the game's display rebuild (`temp.scx`) has it too and loads as a save.

Loading reads the block before the level loads: one that is truncated, of an unknown version, too large or with bytes after its last
script is ignored with its reason (`save state ignored:`), and the scripts start from their constructors. A script the save names that
the level no longer has is skipped with a line; a script without an `nvram` table now is named. The save's package and the content of
its worlds are checked as before (`jpt_save_belongs`).

## What the corpus uses

Of 262 levels of the TresCom corpus, 5 were refused for `CE_SCRIPTS` alone before phase 0. Required script functions, by file:

| Package | Level | Scripts | What they ask that the port does not have |
|---|---|---|---|
| Cloning Facility | `TI~ClnFcty.scn` | `CPlayerTI` (player), GameScripts `GameTI` with its HUD and inventory | `CPlayer`, `TP_Engine`, `GetPlayer`, `DrawTexture`, `GetTexture`, `u4DisplayFormattedString`, `bKeyHit`, `uCMD_USER1/2`, `fGetSpeedFactor`, `fGetJumpFactor`, `OnJump`, `OnPaintHW3D`, `OnHandleInput`, `OnModalKey`, `Say`, `SendLGMessage`, ... |
| TresCom Ops Anthology | `trescom.scn`, `tc_isle.scn` (2022 updates) | `Spitter` animals, `SpitterProjectile` objects | `CAnimal`, `CInstance`, `OnActBite`, `TP_Engine`, `GetElapsedSimTime`, `GetRandFloatRanged`, `pbbBoundaryBox`, `ebbHEAD`, `v3GetHeadPos`, `r3Rot`, `Teleport`, `MoveWithFixedVelocity`, `Vector3`, ... |
| TresCom Ops Anthology | `tc_prequel.scn`, `tc_rescue.scn` | none act: no object names a class, `SpitterHelper` only keeps its state | nothing: no VM; `READY` |

Each level is judged alone, so a sibling refused for CE's API does not refuse them: the corpus has 250 of 262 levels `READY`.

`custom_level_scan --analyze-custom-level` and the compatibility report list them per level.

## Phase 3: CE's engine API, corpus first (pending)

* Vector3, Rotate3 and their operators as native classes; the entity classes as handles that become invalid when the object is
  gone (a call on a gone object is a script error, never a dangling pointer). Instance and action scripts made from the world file's
  `Script` and `ScriptAT`, with `OnReset`, `OnPostLoad` and their own `nvram` by object.
* Spitter: `OnActBite` from the AI's bite and feint, head box and position, `Teleport`, `MoveWithFixedVelocity` over the fixed speed
  of actions 110 and 111, `TP_Engine.GetElapsedSimTime`, CE's random numbers.
* Cloning Facility: player speed and jump factors, `OnJump`, `Say`, pick-up and messages, `DrawTexture`/`DrawSolidRect`/`DrawBitmap`
  into the host overlay, `u4DisplayFormattedString`, `uCMD_USER1` to `uCMD_USER8` binds, modal mode (Escape and Start leave it).
* `OnLevelTransitionRequested`, `OnGameCompleted` and `OnLevelEnd(false)` at the end of the game.

## Phase 4: the rest of the documented API (pending)

Every remaining function of `API-reference.txt` is implemented or answers with a script error naming it. Until then the analysis
keeps refusing a level whose scripts call one the port does not have, with its name.

## Tests (headless, `tools/test.ps1`)

| Group | Suite | What |
|---|---|---|
| Script VM | `script_vm_tests` | load, compile and runtime errors, instruction and memory budgets, heap accounting, instances, methods, nvram values and limits, libraries, `print` |
| Script lifecycle | `script_plan_tests` | the scan, kinds, the per-level rule, files and order, required functions |
| Script lifecycle | `script_session_tests` | creation and destruction without a leak, callback order, frame step, transitions, restarts (repeated), levels without scripts in the same package, retail levels, errors and budgets per script and per VM, the campaign before and after |
| Script save | `script_save_tests` | the block's contents, loads, restart after a load, damaged, truncated and unknown blocks, scripts changed since, the block in stamped saves only |
| Custom level session | `custom_level_session_tests` | a scripted fan mission imported, listed in the runtime map, read back by the game, a VM only in its level |
| Restart lifecycle | `level_actions_tests` | the host's Restart and step hooks tell the scripts |

All of them run on Windows x86 and Linux; the new ones also under ASan and UBSan on Linux.
