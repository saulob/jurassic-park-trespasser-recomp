# Controls

The keyboard, the mouse and gamepads all work at the same time. There is no keyboard or controller mode: walk with the
stick, reach for the mouse, press a key and go back to the stick without restarting anything. The tutorial's
instructions and the other prompts show the buttons of the device you used last.

## Keyboard and mouse

The game's own bindings, changed in its Controls screen (Options > Controls, or F12 in a level). The launcher's Controls
page lists the ones the game loads (its `KeyMap`), so a remapped key shows up there and in the tutorial.

| Action | Default |
|---|---|
| Run | W |
| Walk | S |
| Step left / right | A / D |
| Walk backwards | X |
| Look | Mouse movement |
| Jump | Q |
| Crouch (hold; release to stand) | Z |
| Throw | F |
| Stow / retrieve item | E |
| Replay voice over | R |
| Fire / use | Space |
| Reach with the hand (hold) | Mouse left |
| Move the arm | Mouse left + mouse movement |
| Grab an object | Mouse left + mouse right |
| Drop the object | Mouse right |
| Rotate arm | Ctrl + mouse left + mouse movement |
| Rotate wrist | Shift + mouse left + mouse movement |
| Pause menu | Esc |

Port hotkeys: F11 windowed / borderless full screen, Alt+F4 exit. The game's own: Esc menu, F1 hint, F12 Controls,
Ctrl+F11 cheat input.

## Gamepad

Any controller SDL3 recognises: Xbox (360, One, Series), PlayStation (DualShock 3/4, DualSense), Nintendo Switch Pro and
Joy-Con pairs, the Steam Deck and generic gamepads. Controllers can be connected, disconnected or swapped at any time;
with several connected, the one used last is in use.

| Action | Xbox | PlayStation | Steam Deck |
|---|---|---|---|
| Move: walk, run, walk backwards, sidestep (tilt partly to walk) | Left stick | Left stick | Left stick |
| Look | Right stick | Right stick | Right stick |
| Jump | A | Cross | A |
| Crouch (hold; release to stand) | B | Circle | B |
| Crouch on/off | LS (click) | L3 | L3 |
| Use / fire | RT | R2 | R2 |
| Raise and move the hand: hold, then use the right stick | LT | L2 | L2 |
| Grab / drop | X | Square | X |
| Throw | Y | Triangle | Y |
| Rotate wrist: hold, then use the right stick | LB | L1 | L1 |
| Rotate arm: hold, then use the right stick | RB | R1 | R1 |
| Straighten the hand | LB + RB | L1 + R1 | L1 + R1 |
| Stow / retrieve | D-pad up | D-pad up | D-pad up |
| Replay voice over | D-pad down | D-pad down | D-pad down |
| Pause menu | Menu | Options | Menu |
| Show the controls in the game | View | Create | View |

Every action except the pause menu and the controls card can be given another button in the launcher's Controls page
(Gamepad bindings); choosing a button that another action uses swaps the two. The game can be played with no mouse.

### Using the hand

The arm works as the original's mouse did, with the right stick in the mouse's place:

1. Hold **LT** (L2): Anne raises her hand. While it is held, the right stick moves the hand instead of the view.
2. Move the hand onto an object and press **X** (Square) to grab it. Press **X** again to drop it; while X is held
   after that press, the right stick gives the drop a throw direction, as the original's right mouse button did.
3. **Y** (Triangle) throws what the hand holds; **RT** (R2) uses it (fires a gun, swings with the right stick).
4. Hold **LB** (L1) to rotate the wrist, or **RB** (R1) to roll it, with the right stick. Both raise the hand by
   themselves, because the game only turns the hand while it is up. Holding both straightens the hand.
5. **D-pad up** stows the held item or takes the stowed one back.

### Crouching

Hold **B** (Circle) to crouch and release it to stand, as the keyboard's Z does. **L3** switches crouching on and off
instead. Pressing B or Jump while crouching through L3 hands control back to the button: release B and Anne stands up.
Both drive the game's single crouch command, so the two can never disagree.

### Menus

| | |
|---|---|
| Move between buttons, boxes, sliders and list rows | D-pad or left stick |
| Select / press | A (Cross) |
| Back (as Esc) | B (Circle) or Menu (Options) |
| Change a slider | D-pad left / right on it |
| Scroll a list | D-pad up / down past its last row |
| Move the pointer freely | Right stick |

The focused control shows the game's own hover picture and the game's pointer sits on it. In Load, A on the selected
save loads it. In Save, a new save gets the date and time as its name, which the keyboard can still change. Videos are
skipped with A, B or Menu.

### Settings

In the launcher's Controls page (stored in `user\trespasser.ini`). The page also shows the controller connected now and
the full list of what each button does with the current bindings.

| Setting | Default | Key |
|---|---|---|
| Controller glyphs: Automatic, Xbox, PlayStation, Steam Deck, Generic | Automatic | `gamepad_glyphs` |
| Look sensitivity X / Y (25-300%) | 100% | `gamepad_look_sensitivity_x`, `_y` |
| Invert look Y | Off | `gamepad_invert_y` |
| Stick deadzone (5-40%) | 15% | `gamepad_stick_deadzone` |
| Trigger deadzone (5-50%) | 12% | `gamepad_trigger_deadzone` |
| Bindings | the table above | `gamepad_bind_<action>` |

The game's Invert mouse option stays for the mouse only.

## Instructions in the game

The beach tutorial teaches the controls through subtitles that name keys and mouse buttons ("Press 'Q' to jump, and
'Z' to crouch."). They are part of the voice-over data, not the executable. The port rewrites each of these lines for
the device in use and draws button glyphs inside them:

- keyboard and mouse: keycaps with the key the game has bound now, and a mouse with the button lit;
- gamepad: the bound button in the chosen style, and the sticks where the original says "move the mouse".

Touching the controller switches a line on screen to gamepad glyphs on the next frame; moving the mouse or pressing a
key switches it back. Nothing is restarted or reloaded.

In a level, **View** (Create) shows a card with every control for the device in use, and hides it again; it follows
device switches too. The first time a controller is used in a level, a short line names that button.

### Glyph styles

Automatic shows the buttons of the controller in use: Xbox letters, PlayStation symbols, the Steam Deck's ABXY with
L1/R1/L2/R2/L3/R3, Nintendo's positions (B at the bottom) and, for other controllers, the button's position. A chosen
style always shows that style. Glyphs are drawn as shapes at the text's size, so they stay sharp from 1080p to 4K.

With a gamepad in use, the game's Controls screen shows the gamepad's buttons for its actions, and menus show the
Select and Back buttons in the lower right corner. Using the keyboard or mouse puts the game's own key names back.

## Limitations

- In menus, gamepad glyphs are drawn by the Direct3D 11 HD menus; with a menu shown in its original 640x480 form the
  game's own key names stay and the gamepad still navigates. The in-game instructions and the controls card show
  glyphs with either renderer.
- The keyboard bindings are changed only in the game's own Controls screen, which waits for a key or mouse button.
- No vibration yet: the controller's rumble is available to the port, but no game event drives it.
- Gamepad input reaches the game only while its window is in front.
