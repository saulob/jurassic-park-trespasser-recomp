# References

Outside projects and documentation this port draws on, and what was taken from each. Code is only reused where its
licence allows and it says so in the file; everything else listed here was a reference for behaviour or architecture.

## Foundations

| Reference | Used for |
|---|---|
| [sp00nznet/pcrecomp](https://github.com/sp00nznet/pcrecomp) | the static recompiler and its runtime (fetched, patched in `patches/pcrecomp`) |
| [sp00nznet/trespasser](https://github.com/sp00nznet/trespasser) | the first reconnaissance of the target |

## Gamepad and prompts

| Reference | Kind | Used for |
|---|---|---|
| [saulob/dragons-crown-pro-pc](https://github.com/saulob/dragons-crown-pro-pc) | architecture and behaviour | SDL3 gamepads beside the keyboard with no exclusive mode; hotplug with the controller in use chosen by activity; the last device used deciding prompts, where only the start of input counts (its `ControllerActivityFilter`: new presses, sticks and triggers crossing a threshold, never held input or drift); the launcher's Controls page with a prompt style choice. Adapted to this port's Win32 window and guest thread, not copied. |
| [OpenTrespasser/JurassicParkTrespasser](https://github.com/OpenTrespasser/JurassicParkTrespasser) | behaviour (reading only) | the game's input model, confirmed against the retail binary: `CInput`/`SInput` (`v2Move`, `v2Rotate`, command bits and hits), the 15-entry key map and its registry value, the player's use of move speed and of the hand (Move Hand, Grab/Drop, wrist and arm rotation), the Controls window's key capture, the Save window's need for a typed name, and how UI windows take the pointer and clicks. No code from it is in this port. |
| [SDL3 gamepad API](https://wiki.libsdl.org/SDL3/CategoryGamepad) | documentation | positional buttons (`SDL_GAMEPAD_BUTTON_SOUTH`...), `SDL_GetGamepadType`, connection events, rumble capability, `SDL_HINT_JOYSTICK_RAWINPUT` |
| [Trespasser manual (TresCom)](https://www.trescom.org/files/MANUAL.pdf) | documentation | the retail controls and their names (Run, Walk, Sidestep, Jump, Crouch, Move Hand, Grab/Drop, Use/Fire, Throw, Stow, Rotate Wrist/Arm), checked against the retail key table at 0x0066AE78 |
| [TresCom](https://www.trescom.org/) | community documentation | the tutorial's voice-overs and subtitles, and how players describe using the arm |
| Community DualSense layout (2026) | ergonomics evidence | a tested layout: left stick move, right stick look, L1 raise arm, R1 grab, L2 throw, R2 use/fire, Cross crouch toggle, Circle stow/retrieve, Square jump. Taken only as evidence that the triggers and shoulders suit the arm; this port keeps its own layout (LT/L2 raises the hand, RT/R2 uses) so that every action stays bound to the button the game's own Controls screen and tutorial name. Not copied; its link is not recorded here. |

## Tutorial text

The beach tutorial's instructions are subtitles inside the voice-over samples of `data\Stream.tpa` (11 samples, 20
lines, such as "Press 'Q' to jump, and 'Z' to crouch."); the 1.0.116 executable has none. The subtitle draw path was
read from the retail binary: the overlay at `[0x0066F03C]` adds each line with `0x00468710` (centred at the bottom,
category 2), `0x0050E8C0` lays it out with the font's advance widths, `0x0050EAF0` draws each laid-out line twice
(shadow, then colour) through `TextOutA`.

The player's use of the commands was read in OpenTrespasser's `CPlayer` (behaviour only): crouching follows the held
crouch command, the wrist and arm turn only inside the held Move Hand command, Grab/Drop picks up while held with the
hand raised and drops on the next press, Throw and Stow act on presses.
