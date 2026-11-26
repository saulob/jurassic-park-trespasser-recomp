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

`src/common/jpt_script_vm.cpp` over Squirrel, built by `cmake/JptSquirrel.cmake` as `jpt_script`. Nothing of the game links
it yet, so the campaign and custom sessions run exactly as before.

* Squirrel is fetched like the launcher's sources (`config/launcher_deps.txt`, a pinned commit after 3.2) and patched by
  `launcher/patches/squirrel/0001-instruction-budget.patch`. Its sources build without the project's warnings, as Dear
  ImGui; `jpt_script_vm.cpp` builds with them as errors (`/W4 /WX`, `-Werror`). Windows x86 has CE's 32-bit integers; the
  64-bit Linux host has Squirrel's 64-bit ones, so the engine API will mask colours and handles to 32 bits.
* The base, math, string and blob libraries only; no file, system or auxiliary library. Removed from the base library:
  `seterrorhandler`, `setdebughook`, `enabledebuginfo`, `setroottable`, `setconsttable`, `newthread`, `suspend`. A script
  will reach files only through `GetTexture`, `GetBitmap` and `ResolveFileName`, inside the package.
* Each entry into a script (loading a file, a call) gets 1,000,000 instructions. Running out is an error that `try` and
  `catch` cannot keep alive, also inside callbacks such as a sort's comparator; loops and recursion are instructions too.
  Memory is not limited yet.
* An error, compile or runtime, is a `[script]` line with the file, line and function. The owner the host called (a class,
  from phase 2) stops until the level ends (`jpt_script_vm_end_level`); the others go on, and so does the game.
* `print` and `error` go to the log as `[script] print:` lines, 200 per level. Control characters become spaces, so a script
  never writes a line of its own. Files over 1 MB are not read; a UTF-8 BOM is skipped.
* Test: `script_vm_tests`, Windows x86 and Linux, in `tools/test.ps1`.
* Before the game links it (phase 2): Squirrel's licence in `packaging/licenses/THIRD-PARTY-LICENSES.txt`.

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

* interpreter (done, `script_vm_tests`): load, compile errors, budget, disabled libraries, `print` cap;
* values: Vector3 and Rotate3 operators against the documented results;
* lifecycle order for a new level, a transition, a save load and a restart, over a model engine as `level_actions_tests`;
* `nvram`: every documented type, arrays, the 65536-element and 256 kB limits, save and load;
* binding: class kinds, `static SCN`, wrong engine class refused with a log line;
* campaign isolation: a campaign session creates no interpreter and calls no script;
* analysis: the phase 0 rule over fixtures; scripts calling a missing function refuse their level.
