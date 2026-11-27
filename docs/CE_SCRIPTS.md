# Trespasser CE scripts (Squirrel)

The port embeds the Squirrel interpreter and implements a compatibility layer that lets Trespasser CE scripts call functions of
the game engine. Status: phases 0 to 2 (which levels need scripts, the interpreter, the lifecycle, saves) are done; phase 3, CE's
engine API, is partly done (the registry below lists what exists). A level whose scripts use a name the port does not have is
refused as `CE_SCRIPT_API`, with the names ([CUSTOM_LEVELS](CUSTOM_LEVELS.md)).

## What is verified, and how

| Level of evidence | Meaning |
|---|---|
| Implemented API | the names in `src/common/jpt_script_registry.inc`, each with its binding and a headless test |
| Static analysis | the launcher reads a level's scripts without running them and finds every CE name they use in the registry; it says nothing about whether the level's own logic works |
| Synthetic tests | headless suites run small scripts written for the project over a model world ([Tests](#tests-headless-toolstestps1)) |
| Real gameplay | no scripted community level has been confirmed in a full manual play-through in the port yet |

A level that passes the static analysis is `READY` for its scripts; that is not a statement that the level's features work in play.

## Scope

Scripts run in custom sessions only. The original game's levels have no `.nut` files and no object names a script class;
every CE script feature acts through a script, so there is nothing for the campaign to gain and no option for it. A campaign
session never creates the script session: no VM, no parser, no callback, no budget, no script state in its saves, nothing per frame
(`src/common/jpt_level_scripts.cpp` returns at its first test; `script_session_tests` proves it before and after a custom session).

The port implements part of the API that Trespasser CE documents, following that documentation. No CE code, data or file is used.

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
| ActionType | per action 99 of a trigger, class named by `ScriptAT` | not yet (the `ActionType` class): the level is refused |
| Instance script | per object whose `Script` property names a class derived from its engine class | made when the level's load returned, for each object the world names (phase 3) |

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
* Once a level needs a VM, every game script and every level script that applies to it is made, those that only keep their state
  too (CE makes them all; a helper such as the Spitter's resets the globals its animals share).
* Required script functions: the names of CE's API the loaded code uses that `src/common/jpt_script_registry.inc` does not list,
  and the handlers its classes declare that the port does not call, per level and per file. A class the world names that no script declares, or one of the wrong kind, is a problem of the
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
  closures are shared by every script of it. An empty VM takes 45 KB. Every block carries a header with its heap and size and is charged to that heap, header included; the heap keeps the
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
* `OnLevelEnd` is called at transitions only. `OnLevelTransitionRequested`, `OnGameCompleted` and the handlers of action scripts
  are not called: a level whose scripts declare one is refused, with its name. `OnConsoleCommand` is reached by the headless tests
  only (see Console commands).
* The nvram of game and level scripts is put back when the level's load returned, before `OnScriptInit` (CE: when the save's state
  is read, step 3), not before the load.

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
the level no longer has is skipped with a line; a script without an `nvram` member now is named. A declared `nvram` that is still
`null` (a script that makes its table only when not loading a save) gets a new table of the saved slots, as in CE. The save's package and the content of
its worlds are checked as before (`jpt_save_belongs`).

## Per-level results

`custom_level_scan --analyze-custom-level` lists, for one level, its script classes (`SCRIPT_CLASS`) and each file's required script
functions (`SCRIPT_REQUIRES`). That is static analysis only ([What is verified, and how](#what-is-verified-and-how)).

## Phase 3: CE's engine API (partly done)

One registry, `src/common/jpt_script_registry.inc`, lists every name of CE's API the port implements and every handler it calls, by
script kind. The launcher's analysis (`jpt::script::IsImplemented`, `IsDrivenHandler`) refuses a level whose scripts use a CE name
that is not in it, and `script_bindings_tests` proves that each entry exists in a VM on its owner. A name enters it only with its
binding, its arguments checked, its effect done by the engine and a test of it. The registry is part of the launcher listing's key,
so a level refused before is judged again when names are added.

### Values

`Vector3` and `Rotate3` (`src/common/jpt_script_math.h`, `jpt_script_api.cpp`): constructors, components, the operators of
API-reference.txt, `Scale`, `Normalise`, `Dot`, `Cross`, `Length`, `LengthSqr`, `XAxis`/`YAxis`/`ZAxis`. Rotate3 is the engine's quaternion
(w, then x y z) rotating row vectors: `v * (a * b) == (v * a) * b`, `-r` its inverse, `r * f` the same axis with the angle scaled. A zero
vector stays zero when normalised and a zero quaternion becomes the identity; a division by zero, an argument of the wrong type and a
position or velocity that is not finite are script errors, never values handed to the engine.

### The world's objects

`src/common/jpt_script_engine.h` is what scripts reach of the game: the host implements it over the guest
(`src/host/script_engine.c`), the tests over a world of their own. An object is named by its unique handle (`u4GetUniqueHandle`, the
engine's hash of its name, `[instance+0x2C]`) and found again through the world's `pinsFindInstance` at every call; an entity instance
keeps the handle, the box it stands for and its world generation, never a pointer. A call is a script error naming the method when
the object is gone, when the world is being loaded or purged (between a level's begin and its ready), when the instance belongs to an
earlier world (the VM's generation), or when a script made the instance itself. Nothing in a script keeps a game object alive.

Bound: `CPartition` (`v3Pos`, `r3Rot`), `CInstance` (`GetInstanceName`, `u4GetUniqueHandle`, `Teleport`, `MoveWithFixedVelocity`,
`ActivateSimStep`, `bIsSimStepActive`), `CBoundaryBox`, `CEntity`, `CAnimate` (`pbbBoundaryBox`, `GetHealth`: its hit points, `bDead`: the
engine's own test), `CAnimal` (`v3GetHeadPos`, the engine's), `CPlayer` (`pinsHeldObject`), `CGun` (`iAmmo`, `iMaxAmmo`, kept
`0 <= iAmmo <= iMaxAmmo`), `EBoundaryBoxes` (the 8 boxes of an animate, the aliases sharing theirs), `TP_Engine` (`GetElapsedSimTime`: the
game's simulation time; `GetRandInt`, `GetRandIntRanged`, `GetRandFloatRanged`: a generator seeded per VM; `GetPlayer`; `rgb`, `rgba`,
`frgb`, `frgba`; `u4DisplayFormattedString`, `u4FindSequenceEnd`, `TEXT_FORMAT_*`, `ETextType`), `SendLGMessage`, `SendMessage`.

* `Teleport` places the object through the engine's teleport action (that of ActionType 114), physics included. `MoveWithFixedVelocity`
  is the fixed velocity of ATX's action 110: the velocity is applied every step for the time given (a negative time: until action 111),
  kept in saves with the actions' state; `false` when the object has no physics or, asked to fail then, already moves at one.
* Text goes to the game's own text overlay (`CTextOverlay`, as the level TEXT action and the voice captions): formats `CENTER` 1,
  `LEFT` 2, `RIGHT` 4, `TOP` 8, `BOTTOM` 0x10, `SOLID` 0x200 (the overlay's own bits, the captions use `CENTER|BOTTOM` of type 2);
  `ettALL` 0, `ettUNKNOWN` 1, `ettSUBTITLE` 2, `ettTUTORIAL` 3. `rgb` is 0x00RRGGBB as the overlay keeps it. A text is at most 1023
  bytes and its bytes reach the game as written. The overlay follows `handle_prev` as a pointer: only a handle of an entry it holds is
  passed on, any other value shows the text now.
* `SendLGMessage` calls `OnMessage` of every game and level script but the sender, at once, in their order; `SendMessage` calls the
  target's instance script and returns its reply (null when the target has no script or is the sender). A message spends what is left
  of its sender's instruction budget, and messages nested deeper than 16 are a script error.

### Instance scripts

For each object of the world whose `Script` names a class (the runtime map's `script` lines), when the level's load returned: the
object found by its name, its engine class (by vtable: `CInstance`, `CBoundaryBox`, `CEntity`, `CAnimate`, `CAnimal`, `CPlayer`, `CGun`)
compared with the engine class the script class derives from (a mismatch or a missing object is logged, the others go on), an
instance of the class bound to it, its constructor. Then, in CE's order: nvram from the save, `OnPostLoad` (a save only),
`OnScriptInit(bSaveGame)` on instance scripts, then on game and level scripts. Each runs as its own script (`Class:object` in the log): an
error or a budget stops that object's script only. `OnSimStep` runs after `ActivateSimStep(true)`, which saves keep. Saves keep each
instance script's nvram under its object (block version 2, which also holds Vector3, Rotate3 and objects by unique handle; version 1
is still read). Restart, a transition or a save loaded destroy them with their VM (`destructor`) and the new world gets new ones.
`OnReset` is never called: a custom level always loads whole, as CE with `DisableSimpleLevelRestart`.

`OnActBite(pinsTarget, v3TargetLoc, fTargetDist, bFeint)`: the host overrides the `Act` of the AI's Bite and Feint activities
(`0x004329A0`, `0x00432B10`); after the game's own, the animal the AI is thinking with gets the call with the influence's target,
location and distance.

### Drawing

`OnPaintHW3D` runs once per frame of gameplay, on the game's thread, from the host's interface loop (`src/host/debug_menu.c`):
never over a load, a menu of the game or a world being purged, never on a script that does not declare it. Only there may
`TP_Engine.DrawTexture` be called; it records a command (`src/common/jpt_script_draw.h`): a texture of the level's VM by id, a source
rectangle in texels, a rectangle on the game's screen, the ARGB modulation and CE's blend mode (0 opaque, the texture's alpha
ignored; 1 alpha; 2 additive). Nothing of a renderer reaches a script. At most 4096 commands a frame; past it, and for a blend mode
that is not 0 to 2 or an argument that is not a texture, a script error. A script that fails or runs out of its budget while
painting draws nothing that frame; a VM out of memory draws nothing.

`TP_Engine.GetTexture(name)` loads a picture of the package (TGA, DDS, BMP; without an extension the first of `.tga`, `.dds`, `.bmp`
that exists) by a path relative to its game paths, once per name in a level; a
missing picture is `null`, as in CE. The texture is the VM's: 256 at most, 64 MB of pixels; `GetWidth` and `GetHeight` read its size, and
it is gone with its VM at Restart, a transition or a load. `GetScreenWidth` and `GetScreenHeight` are the game's main rendering surface
(its raster), the space the commands are in. `IsSoftwareMode` is `false`: the port draws these textures with every renderer.

The overlay every renderer composes (`src/host/debug_menu_panel.cpp`) turns the commands into its draw lists, under the port's HUD and
menus, scaled from the game's screen to the picture; mode 2 is bracketed by a marker each backend honours: Direct3D 11 sets an
additive blend state, the Vulkan overlay pipeline has an additive twin, the software rasteriser adds. The RGBA layer that Vulkan
composites in straight alpha when its GPU list cannot be used (`JPT_VULKAN_CPU_OVERLAY`, or a failed submission) cannot add to the
picture under it: there additive commands are drawn as alpha and counted. The Vulkan overlay holds 33 textures a frame; a level's
textures are released when the next level's are drawn.

### Input and modal mode

* After the game read its controls (`CInput`, Windows and Linux alike), `OnHandleInput` on the game scripts that declare it, where
  `bKeyHit`, `bKeyPressed` (of `uCMD_USER1` to `uCMD_USER8`) and `fGetInputMoveAmountY` (the movement input before any speed factor) may be
  read; elsewhere they are a script error. A USER key is bound only in a level whose scripts name that command
  (`LevelPlan::user_commands`); the keys are `script_user1_key` to `script_user8_key` of `trespasser.ini` (1 to 8 by default;
  letters, digits, Shift, Control, Alt, Tab, Space, Enter, Backspace, the arrows, Insert, Delete, Home, End,
  PageUp, PageDown or None). They still reach the game. No USER command exists in the campaign.
* `bSetModalMode(true)` succeeds unless another game script is modal; then the player's controls do nothing, keys of the keyboard go
  to `OnModalKey(vk, pressed, shift, control, alt)` (virtual key codes, `VK_LEFT`, `VK_RIGHT`, `VK_HOME`, `VK_END`, `VK_PRIOR`, `VK_NEXT`
  among them; F1 to F24 stay the port's), `OnHandleModalInput` runs instead of `OnHandleInput` and its `true` keeps mouse look. Escape and
  the player's death end it with `OnModalAborted`. It ends with its script stopped by an error or a budget, and with the VM (Restart,
  transitions, loads, the session's end); it is never saved.

### The player

The instance script of the player's object (a class that extends `CPlayer`) is called where CE calls it, in the game's own player
code (`src/host/script_engine.c`; CE's `tpassp6.exe` 1.0.7e read for each):

* `fGetSpeedFactor(fSpeedFactor)`: for every movement request `CPlayer::Process` sends (its walk speed, 0.5 walking, about 0.78
  walking and strafing, 1.0 running), just before it sends it; the result is the request's speed. `fGetJumpFactor()`: for a request
  that asks to jump (the key's hit frame, and while it is held when the official Patch 1.1 fix applies); the result is the jump
  impulse's scale, which the hand-off arms instead of 1. A negative factor is 0; a call that fails, runs out of its budget or returns
  no finite number keeps the game's value (the speed unchanged, 1).
* `OnJump()`: when the pelvis jumps for the player's own request, after its velocity is set and before the jump vocal (the game's
  0x004F8F00, called only for a voluntary jump that leaves the ground): never for a jump that cannot happen, a dead player, a
  modal script holding the controls, or each frame a key is held.
* `Say(strSample)`: the player's own voice (the game's `CPlayer` speech, as its pain and ammunition lines): the sample by its name,
  case and path separator aside (its ID the CRC-32 of the name in lower case). A sample the game does not have, or a player who
  cannot speak, says nothing; a value that is not a string is a script error.
* `r3HeadRotRel()`: the rotation the player looks with relative to its body (the game's own view, body rotation then this one), so
  `Vector3.YAxis() * (r3HeadRotRel() * r3Rot())` is where the player looks.

Nothing of this is state of the port: the factors are asked again for every request, so a Restart, a load or a transition starts
from the script's own state (`nvram`, saved as CE saves it), and the campaign has no script session. On the 1.1 builds the two
places in `CPlayer::Process` and the hand-off are found in their own code (`config/guest_decisions`).

### Console commands

`OnConsoleCommand(strCmd, strArgs)` is CE's handler of a console command the game does not know: game scripts in their order until
one returns `true` (a bool; another value does not handle it). The retail game has no console and the port adds none, so the
dispatcher (`jpt_script_level_console`) exists for the headless tests only and no command reaches it in play; a level that defines
the handler is not refused for it.

### Still missing

* `OnLevelTransitionRequested`, `OnGameCompleted`, `OnLevelEnd(false)`.

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
| Script bindings | `script_bindings_tests` | the registry against a VM, Vector3 and Rotate3, time and random numbers, handles of the world (gone, older world, world loading), player, guns, health, colours, text, nvram values |
| Script drawing | `script_draw_tests` | textures, commands, the screen, errors and budgets while painting, Restart; software, GPU list, RGBA and Direct3D 11 (WARP) backends |
| Script input | `script_input_tests` | USER commands per level, the input handlers, modal mode, its keys and its end |
| Script player | `script_player_tests` | the speed and jump factors, their defaults and failures, OnJump, Say, r3HeadRotRel, handles after Restart, loads and transitions, the campaign |
| Script console | `script_console_tests` | OnConsoleCommand's order, results, errors and budgets, Restart, the campaign |
| Script instances | `script_instance_tests` | instance scripts over a model world: a spitting animal, OnActBite, Teleport and MoveWithFixedVelocity, a gone projectile, budgets, Restart, saves, messages, ActivateSimStep |
| Custom level session | `custom_level_session_tests` | a scripted fan mission imported, listed in the runtime map, read back by the game, a VM only in its level |
| Restart lifecycle | `level_actions_tests` | the host's Restart and step hooks tell the scripts |

All of them run on Windows x86 and Linux; the new ones also under ASan and UBSan on Linux.
