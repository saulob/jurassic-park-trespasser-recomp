# Trespasser CE scripts (Squirrel): plan

Status: phase 1 done (the interpreter, tested, not linked into the game); phases 0, 2, 3 and 4 planned. Custom levels with object scripts are refused today as `CE_SCRIPTS`
([CUSTOM_LEVELS](CUSTOM_LEVELS.md)).

## Scope

Scripts run in custom sessions only. The original game's levels have no `.nut` files and no object names a script class;
every CE script feature acts through a script, so there is nothing for the campaign to gain and no option for it. In a
campaign session the script runtime is not created and every script hook returns at its first branch; a test proves that a
campaign load, frame and save touch no script code.

The port implements the API that Trespasser CE documents, as documented. No CE code, data or file is used.

## Source of the API

Trespasser CE 1.0.135 ships its scripting documentation with the CE Patcher install (`doc/scripting/`):

| File | Contents |
|---|---|
| `ScriptingReadme.txt` | the 4 kinds of scripts, the load order, persistent state (`nvram`), key binds, every event handler |
| `API-reference.txt` | Vector3, Rotate3, the entity class tree (CPartition to CScriptTrigger), `TP_Engine`, `CAISystem`, `NMagnetSystem`, constants and enums |
| `samples/*.nut` | 7 samples, among them `Spitter.nut` and `ATX102.nut` |

The interpreter is Squirrel 3 (MIT licence, squirrel-lang.org), built from source with the host.

## Script kinds

| Kind | Created | Lifetime |
|---|---|---|
| GameScript | one per class found, for every level of the session | the session; `nvram` crosses level transitions |
| LevelScript | one per class, limited by an optional `static SCN` | one level |
| ActionType | per action 99 of a trigger, class named by `ScriptAT` | until the next full load |
| Instance script | per object whose `Script` property names a class derived from its engine class (CInstance, CAnimal, CPlayer, CTrigger types, CScriptTrigger) | until the next full load |

Scripts are read from the `scripts` folder of the package (today they are left out of the install and counted in the
manifest). Load order and the call order of the load steps follow `ScriptingReadme.txt` ("The loading process").
Restart Level in a custom session is always a full load, as CE's `DisableSimpleLevelRestart`, so instance and action scripts
are created again on every restart.

## What the corpus uses

Of 262 levels of the TresCom corpus, 5 are refused only for `CE_SCRIPTS`. Their world files show which of them assign a
script to an object:

| Package | Level | Scripts the level uses |
|---|---|---|
| Cloning Facility | `TI~ClnFcty.scn` | the player is `CPlayerTI` (stamina, sprint, maps it carries); GameScripts for a HUD (stamina, health, ammo) and a modal map view; `MapPickUp` objects |
| TresCom Ops Anthology | `trescom.scn` (2022 update) | `Spitter` animals and `SpitterProjectile` objects; LevelScript `SpitterHelper` |
| TresCom Ops Anthology | `tc_prequel.scn`, `tc_rescue.scn`, `tc_isle.scn` | none: no object names a script class; the package's only script is the update's `Spitter.nut` |

API that these scripts call, beyond the language itself:

* Spitter: `OnActBite` (the AI's bite and feint), `pbbBoundaryBox(ebbHEAD)`, `v3GetHeadPos`, `r3Rot`, `Teleport`,
  `MoveWithFixedVelocity`, `TP_Engine.GetElapsedSimTime`, `GetRandFloatRanged`, `nvram`, `OnWorldPurge`, `OnPrepareSave`.
* Cloning Facility: `fGetSpeedFactor`, `fGetJumpFactor`, `OnJump`, `OnSimStep`/`ActivateSimStep`, `Say`, `OnPickUp`,
  `SendMessage`/`SendLGMessage`/`OnMessage`, `OnPaintHW3D` with `GetTexture`/`DrawTexture`/`rgba`, `GetScreenWidth/Height`,
  `u4DisplayFormattedString`, `OnHandleInput` with `bKeyHit`/`bKeyPressed` for `uCMD_USER1` (sprint) and `uCMD_USER2`
  (map), modal mode (`bSetModalMode`, `OnModalKey`, `OnModalAborted`, `OnHandleModalInput`), `OnConsoleCommand`,
  `pinsHeldObject`, `GetHealth`, `bDead`, `r3HeadRotRel`, `IsSoftwareMode`, `GetRandIntRanged`, `nvram` with arrays.

Cloning Facility's `fm.ini` also sets `EnableAiActivityEx=-1` (every CE activity class); the port has none of them
([LAUNCHER](LAUNCHER.md)), so its animals act as the 1998 engine's.

## Phases

Each phase ends with headless tests in `tools/test.ps1`; play is tested by hand.

### 0. A level needs scripts only when it uses them

The requirement moves from the package to the level. A level requires `CE_SCRIPTS` when its world names a script class
(`Script`, `ScriptAT`, action 99) or when the package has a GameScript, or a LevelScript for that scene, with any handler
besides the script's own state (`constructor`, `OnScriptInit`, `OnPrepareSave`, `OnWorldPurge`). Script files no level
needs are information. Expected: `tc_prequel`, `tc_rescue` and `tc_isle` become `READY` if nothing else holds them.

### 1. Interpreter (done)

`src/common/jpt_script_vm.cpp` and `jpt_script_heap.cpp` over Squirrel, built by `cmake/squirrel/CMakeLists.txt` as
`jpt_script`. Nothing of the game links it, so the campaign and custom sessions run exactly as before; the campaign will
never create a VM.

* Squirrel is fetched like the launcher's sources (`config/launcher_deps.txt`: commit `f9267f2`, after the `v3.2` tag; MIT)
  and patched by `launcher/patches/squirrel`: `0001` the instruction budget, `0002` a vector size that would overflow
  (`array(-1)`, `array(0x7fffffff)` on x86) reaches the allocator as an impossible size instead of wrapping. Its sources
  build without the project's warnings, as Dear ImGui; the port's two files build with them as errors (`/W4 /WX`,
  `-Werror`). On MSVC the folder uses `/EHs` instead of `/EHsc`: a refused allocation is an exception that crosses
  Squirrel's `extern "C"` API, which `/EHsc` turns into the end of the process. Windows x86 has CE's 32-bit integers; the
  64-bit Linux host has Squirrel's 64-bit ones, so the engine API will mask colours and handles to 32 bits.
* The base, math, string and blob libraries only; no file, system or auxiliary library. Removed from the base library:
  `seterrorhandler`, `setdebughook`, `enabledebuginfo`, `setroottable`, `setconsttable`, `newthread`, `suspend`. A script
  will reach files only through `GetTexture`, `GetBitmap` and `ResolveFileName`, inside the package.
* Each entry into a script (loading a file, a call) gets 1,000,000 instructions. Running out is an error that `try` and
  `catch` cannot keep alive, also inside callbacks such as a sort's comparator; loops and recursion are instructions too.
* Memory: 16 MB per VM (`JPT_SCRIPT_DEFAULT_MEMORY`; the host may pass another `memory_limit`, scripts cannot change it).
  The budget is the whole VM's, not a script's: Squirrel has one heap per VM and its objects, strings and closures are
  shared by every script of it, so no allocation belongs to one script. An empty VM takes 45 KB; all the CE scripts of the
  corpus (Cloning Facility's five, Spitter) load in 140 KB at their peak on x86, so 16 MB is over a hundred times what they
  need and small beside the 32-bit game's address space. Every block Squirrel asks for carries a header with its heap and
  size and is charged to that heap, header included; the heap keeps the bytes held, the peak, allocations,
  reallocations, frees, bytes freed and refusals by budget and by the system. Sizes are checked before any sum, so no
  count wraps.
* An allocation past the budget is refused before it reaches the system. Squirrel cannot continue from a refused
  allocation, so the VM is abandoned: none of its code runs again, every block it held is freed from the heap's list, and
  `[script] disabled: memory budget exceeded (limit N bytes) while running <owner>` and `[script] every script of this VM
  stops` are logged. Every later call or load answers `JPT_SCRIPT_DISABLED`; other VMs, the instruction budget and the game
  go on. A refusal by the system below the budget is logged as `out of memory (the system refused N bytes)`. A VM that
  cannot open within its limit is not created. Closing a VM returns its heap to zero.
* An error, compile or runtime, is a `[script]` line with the file, line and function. The owner the host called (a class,
  from phase 2) stops until the level ends (`jpt_script_vm_end_level`); the others go on, and so does the game.
* `print` and `error` go to the log as `[script] print:` lines, 200 per level. Control characters become spaces, so a script
  never writes a line of its own. Files over 1 MB are not read; a UTF-8 BOM is skipped.
* Test: `script_vm_tests`, Windows x86 and Linux, in `tools/test.ps1`. Squirrel's licence is in
  `packaging/licenses/THIRD-PARTY-LICENSES.txt`.

### 2. Lifecycle and values

* Vector3, Rotate3 and their operators as native classes; the entity classes as handles that become invalid when the
  object is gone (a call on a gone object is a script error, never a dangling pointer).
* Load steps, `OnWorldPurge`, `OnScriptInit`, `OnGameStart`, `OnLevelStart`, `OnLevelEnd`, `OnLevelTransitionRequested`,
  `OnGameCompleted` and `OnSimStep` from the host's existing level, load, purge, transition and frame hooks.
* Instance scripts bound from the world file's `Script` property and found by name after the load.
* `nvram` saved behind the package's mark in the session's saves, as the ATX and CE action state is; restored on load.

### 3. Engine API, corpus first

Spitter first (fewest calls), then Cloning Facility:

* AI: the bite and feint activity calls `OnActBite`; head box and head position from the animal's boundary boxes.
* Physics: `Teleport`; `MoveWithFixedVelocity` over the fixed speed the port already has for actions 110 and 111.
* Player: speed and jump factors at the points where the 1998 code reads its run speed and jump force; `OnJump`, `Say`,
  pick-up, drop, stow and use events.
* Drawing: `DrawTexture`, `DrawSolidRect` and `DrawBitmap` into the host overlay, through the managed user textures the map
  picture already uses, in every renderer (CE draws `OnPaintHW3D` in hardware only; the port has one path).
* Text: `u4DisplayFormattedString` and `u4DisplayPositionedString` through the game's own text overlay.
* Input: `uCMD_USER1` to `uCMD_USER8` as binds shown only when the session's scripts read them, on keyboard and gamepad;
  modal mode takes keys and the mouse, and Escape (Start on the gamepad) always leaves it.

### 4. The rest of the documented API

Every remaining function of `API-reference.txt` is implemented or answers with a script error naming it. The launcher
scans each package's scripts for the functions they call: a level whose scripts call one the port does not have is refused
with `CE_SCRIPTS` and that name, so the rule stays generic for packages outside the corpus.

## Tests (headless)

* interpreter (done, `script_vm_tests`): load, compile errors, instruction budget, memory budget and the heap's accounting
  (limits, overflow, refusals by the system, abandoned VMs, leaks), disabled libraries, `print` cap;
* values: Vector3 and Rotate3 operators against the documented results;
* lifecycle order for a new level, a transition, a save load and a restart, over a model engine as `level_actions_tests`;
* `nvram`: every documented type, arrays, the 65536-element and 256 kB limits, save and load;
* binding: class kinds, `static SCN`, wrong engine class refused with a log line;
* campaign isolation: a campaign session creates no interpreter and calls no script;
* analysis: the phase 0 rule over fixtures; scripts calling a missing function refuse their level.
