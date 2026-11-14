# Controls

The keyboard, the mouse and gamepads all work at the same time. There is no keyboard or controller mode: walk with the
stick, reach for the mouse, press a key and go back to the stick without restarting anything. The tutorial's
instructions and the other prompts show the buttons of the device you used last.

## Keyboard and mouse

The game's own bindings, changed in its Controls screen (Options > Controls). The launcher's Controls
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

Port hotkeys: F2 shows or hides the controls card in a level (the same card as the gamepad's View, with the keyboard's
bindings), F5 the Debug / Test Menu, F10 Cheats, F11 windowed / borderless full screen, Alt+F4 exit. The game's own:
Esc menu, F1 hint, Ctrl+F11 cheat console. A hint naming the card's key (F2, or View with a gamepad) shows for a few seconds at the
start of a level.

### Debug / Test Menu

F5 opens and closes it in a level or with the main menu in front (elsewhere F5 does nothing). While it is open Anne
does not move or look and the game gets no keys; Esc (or B) closes it first, or goes back from Level Select.

| Item | What it does |
|---|---|
| Level Select | the scene files of `data` (the game's own Level Select list) as `BE - Beach` ... `SUM - Summit`, the two test scenes, then any other scene by file name; loads one the way the game changes level |
| Restart Level | the game's Restart Level (as after death or from the pause menu) |
| Reload Current Level | loads the current level again from its files, with the loading screen |
| X, Y, Z, Teleport | starts at Anne's position; type a value, or Left/Right (Shift 10, Ctrl 0.1; gamepad D-pad 1, LB/RB 10) |
| Previous / Next Key Point | the level's teleport points, as the TNEXT cheat walks them |
| Set / Return to Checkpoint | a position kept for this run only, in this level only |
| Show Coordinates | level, X, Y, Z and yaw in the top-left corner (off at every start) |
| Copy Coordinates | `Level=BE X=123.456 Y=78.900 Z=-45.120 Yaw=1.570` to the clipboard |
| Freeze AI | the DINOS cheat: dinosaurs sleep and stop updating until it is turned off |

Keyboard: Up/Down, Enter. Gamepad: D-pad, A to use, B back, Menu to close. Yaw is in radians, counter-clockwise from
+Y, the game's facing axis. Nothing here writes a save or the game's progress.

### Cheats

F10 opens and closes it where F5 works; F5 and F10 replace each other, so only one is ever shown. The game's own
console (Ctrl+F11) still works.

| Cheat | What it does |
|---|---|
| Movement Speed | 0.5x to 5.0x of Anne's walking and running only (Left/Right or Enter) |
| Jump Height | 0.5x to 5.0x of how high Anne's own jumps go; gravity and falls are unchanged |
| Invulnerability | the game's INVUL: no damage from hits, bites, shots or falls; level-scripted deaths still happen |
| Infinite Ammo | the game's WOO: guns do not use up ammunition |
| Freeze Dinosaurs | the game's DINOS, the same switch as the Debug Menu's Freeze AI |
| Heal | Anne's health back to her maximum (not after death) |
| Super Health | four times the normal maximum, full; off brings the maximum back without taking health below it |
| Teleport, Level Select, Show Coordinates | the Debug Menu's own |
| Third Person | the camera behind and above Anne, following her head |
| Fly / Free Camera | a camera that moves on its own while Anne stays where she is |

Free Camera: WASD or the left stick moves, the mouse or the right stick looks, Space or A goes up, Ctrl or B goes
down, Shift or LB is faster. Third Person and Free Camera exclude each other. Movement Speed, Jump Height, Super Health
and the camera modes go back to normal in another level or in the main menu; while a menu of the game is open (and
whenever it saves or loads) the game has its own values, so a save never keeps a cheat. Nothing is remembered between
runs.

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
| Rotate wrist: hold with Move Hand, then use the right stick | LB + LT | L1 + L2 | L1 + L2 |
| Rotate arm: hold with Move Hand, then use the right stick | RB + LT | R1 + L2 | R1 + L2 |
| Straighten the hand | LB + RB + LT | L1 + R1 + L2 | L1 + R1 + L2 |
| Stow / retrieve | D-pad up | D-pad up | D-pad up |
| Replay voice over | D-pad down | D-pad down | D-pad down |
| Pause menu | Menu | Options | Menu |
| Show the controls in the game | View | Create | View |

Every action except the pause menu and the controls card can be given another button in the launcher's Controls page
(Gamepad bindings); choosing a button that another action uses swaps the two. The game can be played with no mouse.

### Using the hand

The right stick is the mouse: the game receives its motion exactly as mouse motion, so the hand, the wrist, the arm
and the view do what the mouse would make them do, with the same limits and the same body turn at the edge.

1. Hold **LT** (L2), the left mouse button's Move Hand: Anne raises her hand, and the right stick moves it as the
   mouse does; it stays where you leave it. LT stays held through anything that pauses the game's reading for a moment
   (a long frame, a menu), as the mouse button does.
2. Move the hand onto an object and press **X** (Square) to grab it, with LT held or not. Press **X** again to drop
   it. While X is held after that press, the right stick gives the drop a throw direction, as the original's right mouse button did.
3. **Y** (Triangle) throws what the hand holds; **RT** (R2) uses it (fires a gun, swings with the right stick).
4. With LT held, hold **LB** (L1) to rotate the wrist, or **RB** (R1) to roll it, with the right stick: the mouse's
   Shift and Ctrl with the left button. Holding both straightens the hand.
5. **D-pad up** stows the held item or takes the stowed one back.

The hand is the game's pointer in a level: no mouse cursor is ever shown over gameplay, whichever device moves the
hand. LT is read with a little hysteresis, so a trigger resting near its threshold does not drop and raise the hand.

While X is held with the hand out, the game reaches for the object under it and crouches by itself when the object is
low; releasing X ends the reach and Anne stands up again.

### Crouching

Hold **B** (Circle) to crouch and release it to stand, exactly as the keyboard's Z does (the game itself only knows a
held crouch). **L3** is the port's on/off switch for the same command: press it to stay crouched, press it again
to stand, with LT held or not. Pressing B or Jump while crouched through L3 hands the posture back to B: release B and Anne stands up, so a
press and release of B always ends up standing. Opening a menu, leaving the window or switching to the keyboard also
ends an L3 crouch.

### Menus

| | |
|---|---|
| Move between buttons, boxes, sliders and list rows | D-pad or left stick |
| Select / press | A (Cross) |
| Back (as Esc) | B (Circle) or Menu (Options) |
| Change a slider | D-pad left / right on it |
| Scroll a list | D-pad up / down past its last row |
| Move the pointer freely | Right stick |

The Controls screen is drawn 1.75 times as large as the original, with wider columns, so every action and binding
reads whole on one line at full size; its buttons, check box and binding fields work as before with the mouse and the gamepad.

The focused control shows the game's own hover picture, its letters lit a brighter red than the original's dark red
(with the mouse too), and the game's pointer sits on it. While the gamepad moves the focus no pointer is drawn,
neither the game's nor the system's; moving the mouse, clicking or pressing a key shows it again where the mouse is.
The right stick's free pointer is shown, since it is then used as a pointer. In Load, A on the selected save loads it. In Save, a new save gets the date and time as its name, which the keyboard can still change. Videos are
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
device switches too. The card is two columns (moving and the body on the left, the arm on the right), each button
next to what it does, in text smaller than the game's own and sized to the screen, so nothing wraps or overlaps at any
resolution or aspect ratio. It and the first-time hint (a short line naming the View button, shown the first time a
controller is used in a level) sit below whatever the game is showing at the top of the screen and clear of the
subtitles at the bottom; opening the card ends the hint.

### Glyph styles

Automatic shows the buttons of the controller in use: Xbox letters, PlayStation symbols, the Steam Deck's ABXY with
L1/R1/L2/R2/L3/R3, Nintendo's positions (B at the bottom) and, for other controllers, the button's position. A chosen
style always shows that style. Glyphs are drawn as shapes at the text's size, so they stay sharp from 1080p to 4K.

With a gamepad in use, the game's Controls screen shows the gamepad's buttons for its actions, and menus show the
Select and Back buttons in the lower right corner. Using the keyboard or mouse puts the game's own key names back.

## Limitations

- The Controls screen's rusted panel is the game's 318x198 picture scaled up about eight times; it stays soft until an
  HD redraw of `Controls_bg.tga` exists. Its text, fields and frames are drawn at the screen's resolution.

- In menus, gamepad glyphs are drawn by the Direct3D 11 HD menus; with a menu shown in its original 640x480 form the
  game's own key names stay and the gamepad still navigates. The in-game instructions and the controls card show
  glyphs with either renderer.
- The keyboard bindings are changed only in the game's own Controls screen, which waits for a key or mouse button.
- No vibration yet: the controller's rumble is available to the port, but no game event drives it.
- Gamepad input reaches the game only while its window is in front.
