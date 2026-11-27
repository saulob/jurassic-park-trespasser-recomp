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

**Mouse sensitivity** (0.25-4.00, default 1.00, key `mouse_sensitivity`) scales the mouse's motion for looking and for
moving the hand, on both axes alike, as Trespasser CE's `MouseSensX`/`MouseSensY`: the game turns sensitivity x counts
x 0.005 radians. 1.00 is the original. It is set in the launcher only (Controls > Keyboard & Mouse) and read when the
game starts; Reset all settings on the About page puts back 1.00. The game's own Controls screen is unchanged. Invert
mouse stays the game's own option, independent of it; the gamepad's look keeps its own Look sensitivity.

Port hotkeys: **F7 Quick Save** and **F8 Quick Load** (changed on the launcher's Controls page; Insert, Delete, Home,
End, Page Up, Page Down, F6 and F9 are the other keys they can take, and a key taken by one of them never reaches the
game). Quick Save writes the level in progress to its own slot, `SAVEGAME.QCK` beside the numbered saves, with the
game's own save writer: a normal retail save that the Load screen lists as "Quick Save" (the Save screen leaves it
out, so a normal save can never overwrite it by accident). It works in a level with Anne alive and no host panel open;
a line under the compass says "Quick saved". Quick Load loads that file only, in a level (also after death) or from
the main menu; without a Quick Save it says "No Quick Save yet" and loads nothing. F2 shows or hides the controls card in a level (the same card as the gamepad's View, with the keyboard's
bindings and the hotkeys below), F3 Objectives, F4 Show Objective, F5 the minimap, F6 the frame counter (top right corner, for this session; the Gameplay page's Frame counter sets whether it starts shown; F6 bound to Quick Save or Quick Load belongs to that choice instead), F10 Cheats, F11 windowed
/ borderless full screen, Alt+F4 exit. The game's own: Esc menu, F1 Hint. The card lists F1 Hint, F2 Controls, F3
Objectives, F4 Show Objective, F5 Toggle Minimap, F10 Cheats, F11 Fullscreen and Esc Menu. The gamepad card adds L3 + R3 Toggle Minimap. On a gamepad, D-pad Left opens the
game's Hint (its own F1 path), D-pad Right Objectives and D-pad Down Show Objective, each only while no action is bound
to that direction (the gamepad has no Replay voice over; R on the keyboard still replays it); B closes the first two. Only one of Controls, Objectives, Cheats and the game's Hint is ever up: opening one
closes the other first, and while any is up Anne gets no input at all. Controls closes with F2, Esc, View, B or Menu
(Esc and Menu then do not also open the pause menu). Whatever was held when one of them opened or closed (a key, a
mouse button, a gamepad button, a trigger, a stick) does nothing until it is released. A hint naming the card's key (F2, or View with a gamepad) shows for a few seconds at the
start of a level.

### Trespasser CE script commands

A custom level whose Trespasser CE scripts read `uCMD_USER1` to `uCMD_USER8` gets keys for those
commands only: `script_user1_key` to `script_user8_key` in `trespasser.ini`, Shift and M for the first two by default, the others
unbound. Those two defaults will be reviewed for v0.1.4 ([LEGAL](LEGAL.md#community-levels)). No other level and never the campaign has them. While such a script shows a screen of its own (modal mode) the player's
controls do nothing and the keys go to it; Escape closes it.

### Minimap

A circular map of the level sits in the bottom-left corner of the picture, centred on Anne, with her heading always up
(the map turns as she does; the compass above keeps north) and a fixed 2x zoom. **F5**, or **L3 + R3** together on a gamepad (one toggle per press of the pair; hold it and nothing repeats),
shows or hides it in gameplay and saves the choice to `minimap` in `trespasser.ini`; the launcher's Gameplay page
shows the same setting as **Enable Minimap** (on by default). A line under the compass says "Minimap On" or "Minimap
Off". F5 and L3 + R3 do nothing in a host panel or the controls card. M belongs to the game again; = and - no longer resize the 3D view. L3 or R3
alone act as bound, 50 ms after the press: the pair is waited for that long so the chord never also crouches.

The compass strip and the objective waypoints have their own switches on the launcher's Gameplay page, **Enable Compass** and
**Enable Waypoints** (`compass` and `waypoints` in `trespasser.ini`, both on by default). They have no key or button, and
are independent of each other and of the minimap: Compass Off hides only the strip, Waypoints Off hides the objective's
3D marker, distance, route markers and its marker on the compass and the minimap. F4 Show Objective, objectives and
subtitles are not affected.

Living creatures near Anne are dots, red while their brain is awake and engaged and green otherwise. A dot grows with
the animal's size in four steps (the instance's scale, about 1 px less than a raptor's for a small animal and 1 and 2 px
more for medium and large ones at 1080p: raptors are the standard dot, Triceratops, Stegosaurus and Parasaurolophus
medium, Albertosaurus, T. rex and Brachiosaurus the largest). Ground vehicles (the jeeps, trucks and Unimog of the
levels) are small orange rectangles along the vehicle's long side, one for each, at the place the level puts them.

### Compass and objectives

The compass at the top of the picture shows where the camera faces (north is the world's +Y; Free Camera
turns it with the camera). On it is a diamond for where the current objective is, with Anne's distance in
metres along the ground under it; past the strip's ends the diamond becomes an arrow at the end to turn
towards. In the picture a smaller diamond sits over the objective (an arrow at the edge when it is off screen or
behind), and where the level has real stepping stones on the way, chevrons mark the next ones ahead of Anne. The
objective, its markers and the trail are amber and stay on the picture. The objective's title is not kept there: the
gamepad's **D-pad down** or the keyboard's **F4** (Show Objective) shows it under the distance for 4 seconds, fading
out at the end; pressing again, or a new objective, starts the 4 seconds over; with no objective it says "No objective"
for 2 seconds. F4 never reaches the game (Alt+F4 still exits), and does nothing outside gameplay.

Objectives are the game's own hints: whatever F1's Hint shows, the objective is the step it describes, and it moves
on when the level's trigger sets the next hint, also after loading a save. F3 (or D-pad Right) lists the level's
objectives: those done, the current one with the game's hint text under it, and `???` for the rest. Levels other than
the eight of the campaign say "Objectives unavailable for this level"; the compass still works there. The HUD shows
only in gameplay: F1, F2, F3, F10, the pause menu, dialogs, videos and loading screens hide all of it, and so does
a tutorial prompt or the port's own hint at the top of the picture. The voice subtitles at the bottom do not: the
compass, the markers and the trail stay while Anne speaks.

### Cheats

F10 opens and closes it in a level or with the main menu in front; F10 and F3 replace each other, so only one is ever
shown. It has no teleport key points, checkpoints, coordinates or
level restart: the panel holds only the cheats below.

The panel has a header with the title and an X button (top right), the rows, and a fixed help area under them that
says what the selected row does (Heal: "Fill health to maximum") or why something could not be done; a change that
worked shows in its own row only. The panel keeps its size while you move through it. The main page lists only the
cheats and Close; Teleport (with X, Y, Z) and Level Select ("Current level: Beach", then the level names) have a
`< Back` row. Esc or B close the main page and go back from the others; F10 and the X close the whole menu. The mouse
works on every row: the pointer over a row selects it and a click does what Enter does (a choice steps to its next
value). The panel is kept inside the screen; its rows scroll when the screen is too short.

| Cheat | What it does |
|---|---|
| Movement Speed | 0.5x, 1x, 1.5x, 2x, 3x, 5x, 7.5x, 10x (the most) of Anne's walking and running only (Left/Right or Enter); her hand stays with her as at 1x |
| Jump Height | 0.5x to 5.0x of how high Anne's own jumps go; gravity and falls are unchanged |
| Invulnerability | the game's INVUL: no damage from hits, bites, shots or falls; level-scripted deaths still happen |
| Infinite Ammo | the game's WOO: guns do not use up ammunition |
| Freeze Dinosaurs | the game's DINOS: dinosaurs sleep and stop updating until it is turned off |
| Heal | shows her health as `90 / 100` and, once used, fills it to her maximum (not after death); an action, not a submenu |
| Super Health | four times the normal maximum, full; off brings the maximum back without taking health below it |
| Noclip | Anne walks through walls and other sideways obstacles on her own feet: the camera stays on her head and gravity, floors, ceilings and falls work as always; nothing moves her when it is turned off (a wall she stands inside pushes her out as the game normally does). Jump and Crouch do what they always do; Fly is the cheat that flies. Another level or the main menu turns it off |
| Stealth Mode | creatures keep thinking, walking and animating but never notice Anne: she is kept out of every brain's influences while the AI looks around. Creatures that already knew her forget her as their memory expires. Gunshots still make noise at a place |
| Teleport, Level Select | Teleport: type X, Y, Z (starting at Anne's position) or step them with Left/Right (Shift 10, Ctrl 0.1; gamepad D-pad 1, LB/RB 10). Level Select: the scene files of `data` (`BE - Beach` ... `SUM - Summit`, the two test scenes, then any other scene by file name), loaded the way the game changes level, also from the main menu |
| Fly / Free Camera | a camera that moves on its own while Anne stays where she is |

Free Camera: WASD or the left stick moves, the mouse or the right stick looks, Space or A goes up, Ctrl or B goes
down, Shift or LB is faster.

The launcher's Cheats page sets what every session starts with: Movement Speed, Jump Height, Invulnerability, Infinite
Ammo, Freeze Dinosaurs and Super Health (see [LAUNCHER](LAUNCHER.md)). F10 shows those values and changes them for the
running session only; the settings file is never written from the game, and the next start uses the saved values again.
Heal, Teleport, Level Select, Fly / Free Camera, Noclip and Stealth Mode are F10 only and start Off in every run. Free Camera and
Noclip go back to normal in another level or in the main menu; the other cheats stay for the session. While a menu of the game is open (and whenever it saves or loads)
the game has its own values, so a save never keeps a cheat.

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
| Show Objective (the objective's title for 4 seconds) | D-pad down | D-pad down | D-pad down |
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
