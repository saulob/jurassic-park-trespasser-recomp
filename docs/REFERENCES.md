# References

Outside projects and documentation this port draws on, and what was taken from each. Code is only reused where its
licence allows and it says so in the file; everything else listed here was a reference for behaviour or architecture.

## Foundations

| Reference | Used for |
|---|---|
| [sp00nznet/pcrecomp](https://github.com/sp00nznet/pcrecomp) | the static recompiler and its runtime (fetched, patched in `patches/pcrecomp`) |
| [sp00nznet/trespasser](https://github.com/sp00nznet/trespasser) | the first reconnaissance of the target |

## Audio and Smacker portability

| Reference | Kind | Used for |
|---|---|---|
| [sp00nznet/pcrecomp `runtime/win32hle`](https://github.com/sp00nznet/pcrecomp/tree/main/runtime/win32hle) (`README.md`, `dsound.c`, `dsound_selftest.c`, `bink.c`) | architecture (reading only) | the split of guest Win32 ABI, guest memory, COM objects, host synchronisation, audio mixer and video decoder; guest objects independent of host objects; no code copied, SDL3 here and not its SDL2 |
| [FFmpeg](https://ffmpeg.org/) `libavformat/smacker.c` (Smacker demuxer) | library and documentation | the demuxer behind the Linux Smacker backend (LGPL-2.1-or-later), used through the libraries |
| FFmpeg `libavcodec/smacker.c` (`smackvid`, `smackaud`) | library and documentation | the Smacker video and audio decoders behind the Linux Smacker backend (LGPL-2.1-or-later), used through the libraries |
| [SDL3 audio](https://wiki.libsdl.org/SDL3/CategoryAudio) | documentation | device streams, `SDL_HINT_AUDIO_DRIVER` and `SDL_AUDIODRIVER`, the dummy driver |

## Gamepad and prompts

| Reference | Kind | Used for |
|---|---|---|
| [saulob/dragons-crown-pro-pc](https://github.com/saulob/dragons-crown-pro-pc) | architecture and behaviour | SDL3 gamepads beside the keyboard with no exclusive mode; hotplug with the controller in use chosen by activity; the last device used deciding prompts, where only the start of input counts (its `ControllerActivityFilter`: new presses, sticks and triggers crossing a threshold, never held input or drift); the launcher's Controls page with a prompt style choice. Adapted to this port's Win32 window and guest thread, not copied. |
| [OpenTrespasser/JurassicParkTrespasser](https://github.com/OpenTrespasser/JurassicParkTrespasser) | behaviour (reading only) | the game's input model, confirmed against the retail binary: `CInput`/`SInput` (`v2Move`, `v2Rotate`, command bits and hits), the 15-entry key map and its registry value, the player's use of move speed and of the hand (Move Hand, Grab/Drop, wrist and arm rotation), the Controls window's key capture, the Save window's need for a typed name, and how UI windows take the pointer and clicks. No code from it is in this port. |
| [SDL3 gamepad API](https://wiki.libsdl.org/SDL3/CategoryGamepad) | documentation | positional buttons (`SDL_GAMEPAD_BUTTON_SOUTH`...), `SDL_GetGamepadType`, connection events, rumble capability, `SDL_HINT_JOYSTICK_RAWINPUT` |
| [Trespasser manual (TresCom)](https://www.trescom.org/files/MANUAL.pdf) | documentation | the retail controls and their names (Run, Walk, Sidestep, Jump, Crouch, Move Hand, Grab/Drop, Use/Fire, Throw, Stow, Rotate Wrist/Arm), checked against the retail key table at 0x0066AE78 |
| [TresCom](https://www.trescom.org/) | community documentation | the tutorial's voice-overs and subtitles, and how players describe using the arm |
| Community DualSense layout (2026) | ergonomics evidence | a tested layout: left stick move, right stick look, L1 raise arm, R1 grab, L2 throw, R2 use/fire, Cross crouch toggle, Circle stow/retrieve, Square jump. Taken only as evidence that the triggers and shoulders suit the arm; this port keeps its own layout (LT/L2 raises the hand, RT/R2 uses) so that every action stays bound to the button the game's own Controls screen and tutorial name. Not copied; its link is not recorded here. |

## Cheats

| Reference | Kind | Used for |
|---|---|---|
| [OpenTrespasser/JurassicParkTrespasser](https://github.com/OpenTrespasser/JurassicParkTrespasser) | behaviour (reading only) | `ExecuteCheat` (`Trespass/gamewnd.cpp`: LOC, TELE compiled out, TNEXT, DINOS), `PlayerTeleportToNextLocation` and `PlayerTeleportToXYZ` (`EntityDBase/Teleport.cpp`), `CWorld::DeferredLoad` (`WorldDBase.cpp`), `CAISystem::bBoring` (`Game/AI/AIMain.cpp`), `CPlayer::bInvulnerable` and the player's jump and walk requests
(`DesignDaemon/Player.cpp`), `g_bUnlimitedAmmo` (`Gun.cpp`), hit points (`Animate.cpp`), `rSpeedMultiplier`
(`Physics/InfoSkeleton.cpp`), `Pelvis_Jump` (`Physics/foot.cpp`), `CEntityAttached` (`EntityDBase/Entity.cpp`), the
`CPlacement3`/`CRotate3` layout; each confirmed in the retail 1.0.116 binary before use. No code from it is in this port. |
| [TresCom: Level select and cheats](https://www.trescom.org/hosted/trespasser_nl/secrets.php) | community documentation | the original Level Select (Ctrl+Shift+Q+W in the main menu) and what LOC, TELE, TNEXT and DINOS do for players |
| [ATX readme (TresCom)](https://www.trescom.org/articles/atx-readme/) | community documentation | the engine's two reload paths (ATX's "fast restart" against a full load); what INVUL, SUPERHP, NORMALHP, THIRDP and FLY do and where they break (scripted deaths, saves and loads, restarts); ATX patches its own executables, so none of its addresses or code apply |

## Tutorial text

The beach tutorial's instructions are subtitles inside the voice-over samples of `data\Stream.tpa` (11 samples, 20
lines, such as "Press 'Q' to jump, and 'Z' to crouch."); the 1.0.116 executable has none. The subtitle draw path was
read from the retail binary: the overlay at `[0x0066F03C]` adds each line with `0x00468710` (centred at the bottom,
category 2), `0x0050E8C0` lays it out with the font's advance widths, `0x0050EAF0` draws each laid-out line twice
(shadow, then colour) through `TextOutA`.

The player's use of the commands was read in OpenTrespasser's `CPlayer` (behaviour only): crouching follows the held
crouch command, the wrist and arm turn only inside the held Move Hand command, Grab/Drop picks up while held with the
hand raised and drops on the next press, Throw and Stow act on presses.

## Ambient occlusion

| Reference | Kind | Used for |
|---|---|---|
| [GameTechDev/ASSAO](https://github.com/GameTechDev/ASSAO) | code (MIT), `c21d57a` | the effect itself: `ASSAO.h`, `ASSAO.hlsl` and `ASSAODX11.cpp` in `third_party/assao` (one marked change, see its README); its quality levels, defaults and inputs (`ASSAO_Settings`, `ASSAO_InputsDX11`) |
| [OpenTrespasser/JurassicParkTrespasser](https://github.com/OpenTrespasser/JurassicParkTrespasser) | behaviour (reading only) | what a Direct3D vertex's `sz`/`rhw` hold (`CCamera::fInvZAdjust`, `fGetScreenZ`): 0.1 / camera-space distance, clamped to 0.99. Confirmed in the retail binary (`CCamera::SetProperties` stores 0.1f / far at camera + 0x120, `0x0051FF0B`; 0.99 at `0x0064E708`) before use. No code from it is in this port. |

[Kirides/GD3D11](https://github.com/Kirides/GD3D11) (GPLv3) and [AMD AOFX](https://github.com/shadercoder/AOFX) were
named as possible architecture references; neither was consulted for this implementation and nothing from them is in
this port. NVIDIA HBAO+, FidelityFX CACAO and XeGTAO are not used.

## Official v1.1 build 117 patch

| Reference | Kind | Used for |
|---|---|---|
| [Trespasser v1.1 (117) Full Patch](https://www.trescom.org/download/trespasser-patch-v1-1-117-full/) by DreamWorks Interactive (hosted by TresCom), and its executable-only edition | behaviour (static comparison of build 117 against the retail build 116, never run) | the patch notes; the executable's changes (function-level comparison of both builds); the level files' changes (objects, properties, transforms, scene sections, terrain). The port reproduces the scenes' terrain tolerances, the executable's jump, hand, box-contact and partition fixes, and 178 object placements; the patch's files are not used by the port. |

## Trespasser CE

| Reference | Kind | Used for |
|---|---|---|
| [Trespasser CE](https://www.trescomforum.org/viewtopic.php?t=10674) by Lee Arbuco (1.0.135, as installed by the CE Patcher) | behaviour (static analysis only, never run) | which improvements exist and how they behave: the quicksave (`EnableQuickSaveKey`: F9, slot `SAVEGAME.qck`, name "Quicksave"), Dual stow (`EnableDualStow`: stow-only and back retrieval with Rotate Arm/Wrist held, a retrieve pose per slot), `ForceMaxObjectDetail` (the renderer's `bDetailReduce` cleared), `MaxActiveAi` (the active-animal limit and its physics-box guard), `EnableAiActivityEx` (studied: not applicable to the retail executable), the DX9 water refraction and enhanced sky (what they sample and how they blend, reimplemented as Direct3D 11 shaders of the port's own; no shader bytecode copied) and the bump shader (studied: the port already draws the original bump maps). Each behaviour was matched to the retail code it changes and reimplemented over that; no CE code, data, shader or file is in this port. |
| [CE Patcher Large Version](https://www.trescom.org/download/ce-patcher-large-version/) by yochenhsieh (TresCom) | behaviour (static comparison of its installed files) | its main menu Quick Load button: a second `UICTRL` with id 1004 in `menu\mainscreen.ddf`, which both executables route to the Direct Load window (it loads no quicksave). Not used by the port. |
| Trespasser CE scripting documentation (`doc/scripting/`: `ScriptingReadme.txt`, `API-reference.txt`, samples), installed by the CE Patcher and shipped with CE 1.07e | API documentation | the script kinds, where scripts are found and their load order, the loading process and the handlers called in it, `nvram` and its save limits, `static SCN` ([CE_SCRIPTS](CE_SCRIPTS.md)); the names `API-reference.txt` declares, as the table of `src/common/jpt_script_api.inc` that tells which a script asks for. No sample or code of CE is in the port. |
| [Squirrel](http://www.squirrel-lang.org/) by Alberto Demichelis (MIT) | language | the interpreter of CE scripts ([CE_SCRIPTS](CE_SCRIPTS.md)), fetched at commit `f9267f2` (after `v3.2`) with two patches of the port's own (an instruction budget, a checked vector size) and the port's allocator with a memory budget per VM. Linked into the game, used by custom sessions only. Licence in `packaging/licenses/THIRD-PARTY-LICENSES.txt`. |
| [Trespasser Mod Manager](https://github.com/miketheraptor/Trespasser-Mod-Manager) by MikeTheRaptor | configuration reference | the list of CE's options (`EnableQuickSaveKey`, `EnableDualStow`, `EnableAiActivityEx`, `MaxActiveAi`, `WaterRefraction`, `EnhancedSky`, `ForceMaxObjectDetail`). Its code and architecture are not used. |

## Community levels

| Reference | Kind | Used for |
|---|---|---|
| [TresCom download catalog](https://www.trescom.org/download-category/levels/) | test corpus (local analysis only, not distributed; under provenance review, with excluded packages left out per [PROVENANCE](PROVENANCE.md)) | which file layouts, formats, settings, ActionTypes and Trespasser CE features community levels use, to test the custom level scanner. No level file is in the repository or the releases ([LEGAL](LEGAL.md#community-levels)). |
