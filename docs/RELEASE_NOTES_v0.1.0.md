# Jurassic Park: Trespasser Recompiled v0.1.0

**The first public release.** *Jurassic Park: Trespasser* (1998) running as native code on modern Windows and Linux through static recompilation.

**No game files are included.** You need your own legally obtained Trespasser disc image. The launcher imports it, verifies it and keeps your installation, saves and settings beside the program.

## Introduction

*Jurassic Park: Trespasser Recompiled* is a PC port built by translating the retail game's machine code to C once, ahead of time, and compiling that C into a native host. There is no CPU emulator, no Wine or Proton, and no original executable needed to play.

The port keeps the original game: its levels, physics, dinosaurs, weapons, interface, audio and videos. Around that core it adds a modern renderer, native display modes, a functional minimap and compass, quality-of-life controls, cheats, localisation, a launcher, an installer and an updater.

This is **v0.1.0**, the first public build. The game is playable and in testing, not a final remaster. Windows is the primary platform; the native Linux build is experimental in this release.

### Platform at a glance

| | Windows | Linux |
|---|---|---|
| Host | Native x86 (32-bit) on 64-bit Windows | Native x86_64 |
| Renderers | Direct3D 11 (default), Vulkan (selectable), Software | Vulkan (default), Software |
| Native high-resolution rendering | Yes, including 4K presets | Not yet: guest stays at its 4:3 mode |
| True widescreen / Hor+ (21:9, 32:9) | Yes | Not yet |
| HD menus and loading screens | Yes | Yes |
| HD video presentation | Yes (renderer path) | Original video presentation with subtitles |
| Minimap, compass, objectives, cheats | Yes | Yes |
| Gamepad | Yes | Yes |
| Steam Deck | Not applicable | Experimental, not hardware-validated |
| 120 FPS / uncapped frame rate | No validated mode | No validated mode |

## Highlights

- **Native static recompilation** of the retail game, with the original logic running as C on the host CPU. No emulator and no Wine or Proton.
- **Direct3D 11 renderer** on Windows with the original software renderer kept as a fallback, plus a native **Vulkan 1.2** backend on Windows and Linux.
- **Native high resolutions** on Windows, from 720p through 1440p and 4K, including custom sizes and the monitor's native resolution.
- **True widescreen (Hor+)** from 4:3 through 16:9, 16:10, 21:9 and 32:9. Wider screens show more of the world at the sides instead of stretching it.
- **Field of view** from 60 to 110 degrees across the 4:3 view, with the original 75 degrees as the default.
- **HD front end, loading screens and video presentation**, rebuilt from the game's own art at the screen's resolution. No replacement texture pack is included.
- **Minimap and compass**, with an objective waypoint, distance, direction, creature and vehicle markers, and a full objectives list.
- **Cheats** for movement speed, jump height, invulnerability, infinite ammo, frozen dinosaurs, health, noclip, stealth, teleport, level select and a free camera.
- **Modern controls**: keyboard and mouse, SDL3 gamepads with hotplug and glyphs, analog movement, rebindable gamepad buttons, deadzones and look sensitivity.
- **Seven languages** for game text, menus and subtitles, with English voices and videos; video subtitles and restored cut dialogue.
- **Trespasser CE-inspired improvements** and selected fixes from the official 1.1 patch, reimplemented natively.
- **Portable launcher** with disc import, version selection, updates, a privacy-filtered Bug Report page and no online play.
- **No commercial game data** in the repository or the downloads.

Two points are deliberately not advertised beyond what exists:

- **120 FPS is not a validated feature of v0.1.0.** VSync can be disabled and presentation can run above 60 Hz, but there is no uncapped-frame-rate mode and no 120 FPS guarantee. See [High Frame Rate and Performance](#high-frame-rate-and-performance).
- **"HD" here means the interface, loading screens and video presentation, not HD replacement textures.** The 3D world keeps the original textures, with generated mipmaps and the original art. See [Graphics and Visual Improvements](#graphics-and-visual-improvements).

## Graphics and Visual Improvements

**Platform note:** the native-resolution, draw-distance, LOD, terrain, texture-cache and generated-mipmap work below is part of the Windows renderers (Direct3D 11 and Vulkan). The Linux Vulkan backend has its own filtering, MSAA, ambient-occlusion, water, sky and HD front-end paths, but it does not yet have the Windows widescreen mode override. See [Native Linux and Steam Deck](#native-linux-and-steam-deck).

### Renderers

- **Direct3D 11** is the Windows default. The game's Direct3D 6 drawing is translated to Direct3D 11 instead of being rasterised by the CPU. The software renderer remains available as a compatibility fallback and keeps the original look.
- **Vulkan 1.2** is a native backend for Windows and Linux. On Windows it is selectable in the launcher; Direct3D 11 remains the primary and recommended backend. On Linux, Vulkan is the default GPU backend.
- **Software** is the fallback when no usable GPU backend is available. It is the game's own renderer and does not receive the GPU-only effects.

The renderer is chosen at start and stays for the whole run. There is no runtime renderer switch during gameplay.

### Native resolution and scaling

- **Render resolution presets**: Original (compatibility size), 720p, 1080p, 1440p, 4K, Native and custom `WxH`.
- With Direct3D 11, the game plays at the render size itself: its viewport, camera, text and 3D target use those pixels. The window can be smaller or larger without changing the render size.
- **4K presets**: 3840x2160 at 16:9, 5120x2160 at 21:9 and 7680x2160 at 32:9. Extremely large boxes use an internal gameplay-size budget; the final image is still rasterised at the selected box. This keeps the 32-bit host's buffers within its 2 GB address space.
- **Menus and videos** keep the game's 640x480 canvas and its 4:3 shape. They are centred and scaled, pillarboxed on widescreen displays, with the mouse mapped to the visible rectangle.
- **Gameplay text** follows the screen height instead of staying at the original pixel size, so hints, tutorial lines and subtitles remain readable at 1440p and 4K.

### Textures, filtering and anti-aliasing

- **Texture filtering**: Original, Bilinear, Anisotropic 4x, 8x and 16x, with automatic fallback to what the card supports.
- **Generated mipmaps**: the 3D world's textures get full mip chains at load time. The game's texture files are not modified.
- **MSAA**: Off, 2x, 4x and 8x, limited to the levels the graphics card supports for the game's colour and depth formats.
- **Texture detail**: textures, terrain and impostors use their full 256-pixel resolution instead of the 1998 limit of 128.
- **Texture cache**: the game's texture memory grows from 24 MB to 96 MB, with the engine's 32-bit budget arithmetic corrected. Textures survive Alt+Tab and menu changes without being rebuilt.
- **Texture streaming**: the per-frame upload window is timed precisely, so newly visible scenery gets its textures sooner instead of showing flat colours.

### Draw distance, LOD and terrain

- **Draw distance**: every object stays drawn 1.2 times as far.
- **Impostors**: objects stay 3D models out to 25-150 m, by size, instead of the original 15 m. They no longer flicker between the model and its flat impostor.
- **Terrain**: the ground keeps its detailed textures out to the far clip, up to 750 m, instead of a blurry map beyond 345 m.
- **LOD**: models keep their detail measured at the real render size, and the terrain and impostor systems use the full texture resolution.
- **Maximum object detail** is an optional setting that keeps every object at its most detailed model, as Trespasser CE's `ForceMaxObjectDetail`.

### Ambient occlusion, water and sky

- **Ambient occlusion**: Off, Low, Medium, High and Ultra. The Direct3D 11 path uses Intel ASSAO over the 3D scene and needs feature level 11.0; the Vulkan backend has its own AO pass over the same presets. If the effect cannot be created, it stays Off and the log says why.
- **Water refraction**: the scene behind water is distorted by the game's own wave heights, as a GPU pass, after Trespasser CE's refractive water. It works with Direct3D 11 and Vulkan and is unavailable with the Software renderer.
- **Enhanced sky**: the sky fades into the level's fog colour towards the horizon by view direction, using the level's own sky fog band, after Trespasser CE's enhanced sky. It works with Direct3D 11 and Vulkan.
- **Bump mapping** needs no option: the original bump maps are already drawn by the game's own bump-span path into the modern frame.

### HD front end, loading screens and video

- **HD front end**: the game's front-end menus and windows are read from its own window state and drawn again at the screen's resolution. The game keeps its own hit tests, hover states, values and callbacks. Text is sharp at 1440p and 4K; the original 640x480 canvas remains the fallback whenever a window or control is not recognised.
- **HD loading screens**: the level loading screen redraws the game's own 640x480 level picture at the window's resolution, with its progress plate and captions. Optional external HD loading art can be dropped into `assets/frontend/loading_hd`; no such art is included in the release.
- **HD video presentation** on Windows: the four Smacker videos are presented through the renderer with deblocking, reconstruction of the interlaced rows, a Catmull-Rom scale to the picture's shape and edge extension on wider screens. The video files and their audio are never modified.
- **HD save previews**: the save screen can show a picture of the frozen gameplay frame for each save. Windows writes JPEG previews and Linux writes PNG previews; a preview written on one platform is not read on the other in this release.

**Important:** no HD texture pack is included or required. The 3D world uses the player's original textures, improved by mipmaps, filtering, a higher texture-memory budget and a higher resolution cap. "HD" in these notes always refers to the interface, loading screens and video presentation.

### Window, presentation and VSync

- **Windowed** and **Borderless Fullscreen** display modes. F11 switches between them and saves the choice.
- **Monitor** selection on multi-monitor systems.
- **Window size**: Auto follows the picture's shape; a fixed size is also available. The game's render size does not change when the window is resized.
- **Presentation**: Letterbox (keep the picture's shape, with black bars) or Stretch (fill the window). Stretch is a fit mode, not widescreen: it does not add world to the sides.
- **VSync**: On waits for the display's refresh; Off presents without waiting. See [High Frame Rate and Performance](#high-frame-rate-and-performance).
- **FPS in title** and **Frame counter** (F6) use the same measured present rate. The title can also show the render resolution.

## Ultrawide and True Ultrawide

This release supports **21:9** and **32:9 Super Ultrawide** on Windows, in addition to 4:3, 16:9 and 16:10.

- **True widescreen means Hor+.** The camera keeps the vertical field of view of the original 4:3 view and adds world to the left and right. A 16:9 screen shows more than 4:3; 21:9 shows more than 16:9; 32:9 shows more than 21:9. The image is not stretched.
- **Field of view** is adjustable from 60 to 110 degrees across the 4:3 view. At 16:9 that is about 125 degrees horizontally, and at 32:9 about 151 degrees. The value is clamped and the game's own level-specific camera angles keep their ratio.
- **Presentation Stretch is not true ultrawide.** It only stretches the final picture to fill a window or display with a different shape. Use Letterbox if you want the correct geometry.
- **32:9 render presets** include 3840x1080 at 1080p, 5120x1440 at 1440p and 7680x2160 at 4K. The 4K 32:9 box is above the internal gameplay-buffer budget, so the game's own screen is reduced to the largest 32:9 mode within budget while the final image is still rasterised at the selected box.
- **No dedicated ultrawide HUD.** Menus and dialogs stay 4:3 and are pillarboxed. In gameplay, hints, subtitles and the HUD keep the game's screen anchors, and a separate ultrawide safe-area layout has not been implemented. The minimap and compass are positioned relative to the picture, so they remain usable, but very wide screens can place ordinary text near the edges.
- **Linux does not yet have the Windows mode-table override.** The Linux build keeps the guest at its 4:3 mode and presents that picture scaled to the window. Native 4K, 21:9 and 32:9 Hor+ gameplay are Windows features in v0.1.0.

## High Frame Rate and Performance

The port includes substantial performance work:

- Texture conversion and mipmap generation are accelerated.
- Perspective-textured and bump-mapped polygons can be drawn by the GPU instead of the CPU.
- Water rendering and surface synchronisation were optimised for Direct3D 11.
- Texture uploads are timed with the performance counter instead of the 15.6 ms system tick.
- Menus over a paused game and start-up checks no longer copy the full picture back from the graphics card.
- The 96 MB texture cache and generated mipmaps reduce repeated uploads and rebuilds.
- The water simulation pays its owed time in steps no longer than the model's own maximum step, so its behaviour is stable at different frame rates.

**Frame rate status in v0.1.0:**

- **There is no uncapped-frame-rate option and no validated 120 FPS mode.** The project's own README lists uncapped frame rate as not yet implemented.
- **VSync Off removes the present wait.** Depending on the scene, GPU and display, the game may present above 60 Hz. That is not the same as a tested 120 FPS feature, and this release makes no guarantee that physics, animation, input or audio remain correct at 120 FPS.
- **VSync On uses the display's refresh rate.** A 120 Hz monitor can therefore present at 120 Hz when the game and renderer can keep up, but the simulation still runs on the game's own timing, including its 10 ms physics steps.
- **No frame limiter or frame-rate target is exposed** in the launcher.
- **Frame counter**: F6 toggles an on-screen FPS panel for the session. The launcher can make it start visible. The window title can show FPS and render resolution.

If you need a specific frame rate for competitive play or for testing, treat v0.1.0 as unvalidated and use VSync as the only supported pacing control. No high-frame-rate mode is part of v0.1.0.

## Minimap, Compass and Waypoints

### Minimap

- **Toggle**: F5 on the keyboard, or **L3 + R3 together** on a gamepad. One toggle per press; holding the pair does not repeat.
- **Placement**: a circular map in the bottom-left corner of the gameplay picture, centred on Anne.
- **Orientation**: player-up. The map turns as Anne turns; the compass above keeps north.
- **Zoom**: fixed 2x.
- **Creatures**: living creatures near Anne appear as dots. Red means the brain is awake and engaged; green means it is not. Dot size grows with the animal's size in four steps, from small animals through raptors, medium dinosaurs and the largest dinosaurs.
- **Vehicles**: ground vehicles such as jeeps, trucks and the Unimog appear as small orange rectangles along their long side, at the position the level places them.
- **Persistence**: the on/off choice is saved to `minimap` in `trespasser.ini`. The launcher's Gameplay page exposes the same setting as **Enable Minimap** (on by default).
- **Feedback**: a line under the compass says "Minimap On" or "Minimap Off".

### Compass

- The compass strip at the top of the picture shows where the camera faces. North is the world's +Y axis. Free Camera turns the compass with the camera.
- The current objective is a diamond on the strip, with Anne's ground distance in metres underneath.
- When the objective is beyond the strip's ends, the diamond becomes an arrow pointing towards it.
- In the 3D picture, a smaller diamond marks the objective. It becomes an edge arrow when the objective is off-screen or behind Anne.
- Where a level has real stepping stones on the route, chevrons mark the next ones ahead.
- The objective, its markers and its trail are amber.

### Waypoints and objectives

- The **waypoint is the current objective**, not a user-placed map marker. There is no custom waypoint placement in v0.1.0.
- **F4** (Show Objective) or **D-pad Down** shows the objective's title under its distance for 4 seconds. It fades out at the end. Pressing again, or a new objective, restarts the 4 seconds. With no objective it says "No objective" for 2 seconds.
- **F3** (Objectives) or **D-pad Right** lists the level's objectives: completed ones, the current one with the game's hint text, and `???` for the rest.
- Objectives follow the game's own hints: the current objective is the step the game's F1 Hint describes and advances when the level's trigger sets the next hint, including after loading a save.
- Levels outside the eight campaign levels say "Objectives unavailable for this level"; the compass still works there.

### Visibility rules

- The minimap, compass and markers show only in gameplay.
- F1 Hint, F2 Controls, F3 Objectives, F10 Cheats, the pause menu, dialogs, videos and loading screens hide them.
- A tutorial prompt or the port's own top-of-screen hint also hides the compass and markers.
- Voice subtitles at the bottom do **not** hide them: the compass, markers and trail stay while Anne speaks.
- F5 does nothing while a host panel or the controls card is open.

## Gameplay Improvements and Cheats

### Quick Save and Quick Load

- **F7 Quick Save** writes the level in progress to its own slot, `SAVEGAME.QCK`, using the game's own save writer.
- **F8 Quick Load** loads that file, in a level or from the main menu.
- The file is a normal retail save and appears in the Load screen as "Quick Save". The Save screen leaves it out, so a normal save cannot overwrite it by accident.
- Both keys can be changed in the launcher's Controls page. The other choices are F6, F9, Insert, Delete, Home, End, Page Up and Page Down.
- Quick Save works in a level with Anne alive and no host panel open. Quick Load works after death and from the main menu. Without a Quick Save it says "No Quick Save yet" and loads nothing.

### Trespasser CE-inspired improvements

These are the port's own implementations, studied from Trespasser CE. Trespasser CE is not included, not required and not read at run time.

- **Dual stow** (On by default): one item on the belt and one on the back while the hand stays free. Holding Rotate Arm or Rotate Wrist while pressing Stow/Retrieve stows into the matching slot instead of swapping. Saves remain retail saves.
- **Active AI** (Enhanced by default): up to 6 creatures active at once instead of the retail 3, as CE's `MaxActiveAi`. Original restores the retail limit of 3. The port keeps the retail activation rule and adds CE's physics guard: an animal whose collision boxes do not fit stays inactive instead of receiving no physics. Saves keep the retail layout.
- **Water refraction** (On by default): see [Graphics and Visual Improvements](#graphics-and-visual-improvements).
- **Enhanced sky** (On by default): see [Graphics and Visual Improvements](#graphics-and-visual-improvements).
- **Maximum object detail** (Off by default): every object keeps its most detailed model, as CE's `ForceMaxObjectDetail`.
- **Mouse sensitivity**: one value for both axes, 0.25 to 4.00, in the launcher's Controls page. The original is 1.00.
- **Footstep and collision sounds** no longer go silent after loading a save or restarting a level.

Every CE-inspired option has an Off or Original choice that keeps the retail behaviour.

### Official Trespasser 1.1 build 117 fixes

DreamWorks Interactive released an official v1.1 (build 117) patch in November 1998. The port runs the retail 1.0 build 116 and reimplements selected fixes natively. The patch is not required, not distributed and not read.

The **Official Patch 1.1** setting (Gameplay page, On by default) controls:

- **Terrain tolerances**: `TrrPixelTol` from 8 to 10 and `TrrPixelTolFar` from 20 to 50, the patch's "Adjusted terrain texture resolution", applied only when the level file is the retail one.
- **Occasional object disappearance**: a moved object is no longer filed inside a more detailed partition that the renderer skips at lower render quality.
- **Jump consistency**: a jump request stays pending until it fires or 25 physics steps fail to find ground; a held jump key keeps asking. A press between physics steps is no longer lost.
- **Object pickup, keypad interaction and centring aim**: build 117's hand behaviour, including the 0.7 m reach, the tucked hand when blocked, the longer hold before pulling an object in, and the view no longer chasing a settling hand.
- **Compound-object interpenetration and box-box interaction**: corners already inside a box are pushed out, contact normals use compound sub-box rotation, edge-edge contacts run, and pairs that ignore each other's collisions still wake each other.
- **Floating objects**: 177 vegetation placements in IJ and IT and one rifle in AS, where the official fix does not depend on rebuilt terrain or partitions.

Off runs the retail code and data. Saved games are not changed and remain retail saves.

### Cheats

Press **F10** in a level or with the main menu in front. F10 and F3 replace each other. The game's own console (Ctrl+F11) still works. The port's panel has no saved teleport points, checkpoints or level restart.

| Cheat | What it does |
|---|---|
| Movement Speed | 0.5x, 1x, 1.5x, 2x, 3x, 5x, 7.5x or 10x of Anne's walking and running speed only; her hand stays with her as at 1x |
| Jump Height | 0.5x to 5.0x of Anne's own jump height; gravity and falls are unchanged |
| Invulnerability | The game's INVUL: no damage from hits, bites, shots or falls; level-scripted deaths still happen |
| Infinite Ammo | Guns do not use ammunition |
| Freeze Dinosaurs | Dinosaurs sleep and stop updating until the cheat is turned off |
| Heal | Shows health as `90 / 100` and fills it to Anne's maximum (not after death) |
| Super Health | Four times the normal maximum, full; turning it off restores the maximum without dropping current health below it |
| Noclip | Anne walks through walls and sideways obstacles on her own feet; gravity, floors, ceilings and falls still work. Another level or the main menu turns it off |
| Stealth Mode | Creatures keep thinking, walking and animating but never notice Anne. Creatures that already knew her forget her as their memory expires; gunshots still make noise |
| Teleport | Type X, Y, Z or step the values with Left/Right (Shift 10, Ctrl 0.1; gamepad D-pad 1, LB/RB 10) |
| Level Select | The scene files of `data`, numbered in the game's order (`1 - Beach`, `2 - Jungle Road`, `3 - Industrial Jungle`, ...), loaded through the game's own level change |
| Fly / Free Camera | A free camera that moves while Anne stays where she is |

**Fly / Free Camera** uses WASD or the left stick to move, the mouse or right stick to look, Space/A to rise, Ctrl/B to descend and Shift/LB to move faster.

**Saved versus session:** the launcher's Cheats page sets what every session starts with: Movement Speed, Jump Height, Invulnerability, Infinite Ammo, Freeze Dinosaurs and Super Health. F10 shows those values and changes them for the running session only. The game never writes the settings file. Heal, Teleport, Level Select, Fly / Free Camera, Noclip and Stealth Mode are F10-only and start Off in every run. Free Camera and Noclip return to normal in another level or the main menu. While a game menu is open, or while the game saves or loads, the game has its own values, so a save never keeps a cheat.

### Saves, pause and compatibility

- Normal save and load, the retail `SAVEGAME.*` slots and save pictures are supported.
- Pause remains the game's own Esc menu.
- Quick Save is a normal retail save; the port does not change the save format.
- No mods are supported in this release.

## Controls and Input

### Keyboard and mouse

The game's own bindings are the defaults:

| Action | Default |
|---|---|
| Run / walk / step / back | W / S / A / D |
| Walk backwards | X |
| Look | Mouse movement |
| Jump | Q |
| Crouch (hold) | Z |
| Throw | F |
| Stow / retrieve | E |
| Replay voice-over | R |
| Fire / use | Space |
| Reach with the hand | Mouse left |
| Move the arm | Mouse left + movement |
| Grab | Mouse left + mouse right |
| Drop | Mouse right |
| Rotate arm | Ctrl + mouse left + movement |
| Rotate wrist | Shift + mouse left + movement |
| Pause | Esc |

- **Remapping**: keyboard and mouse bindings are changed in the game's own **Options > Controls** screen. The launcher lists the keys the game loads and reflects remapped keys in the tutorial prompts, but it does not replace the game's remapping screen.
- **Mouse sensitivity**: 0.25 to 4.00, default 1.00, set in the launcher's Controls page. It scales mouse look and hand movement on both axes, as Trespasser CE's `MouseSensX`/`MouseSensY`.
- **Invert mouse** remains the game's own option and is independent of sensitivity.
- **F2** or **View** shows a controls card in a level. It lists the current device's bindings and hotkeys, translated, and switches immediately when you change device.

### Gamepad

- SDL3 gamepads work out of the box: Xbox 360/One/Series, PlayStation DualShock/DualSense, Switch Pro and Joy-Con pairs, Steam Deck and generic controllers.
- Keyboard, mouse and gamepad work at the same time. The last device used decides the prompts and glyph style.
- **Automatic glyphs** show Xbox letters, PlayStation symbols, Steam Deck labels, Nintendo positions or generic buttons. A fixed style can be chosen.
- **Analog movement**: the left stick walks slowly when tilted partly and runs at full tilt. The right stick looks and moves the hand, converted to the game's own mouse motion.
- **Gameplay**: Jump, crouch (hold B or toggle L3), use/fire, raise/move the hand, grab/drop, throw, rotate wrist/arm, straighten the hand, stow/retrieve, Show Objective, pause and the controls card.
- **Menus**: D-pad or left stick moves between controls, A selects, B goes back, the right stick moves the pointer, sliders and lists work with the D-pad.
- **Hotplug**: controllers can be connected, disconnected or swapped at any time; the last one used is active.
- **Settings**: glyph style, look sensitivity X/Y, invert Y, stick deadzone and trigger deadzone. Every action except the pause menu and the controls card can be rebound in the launcher; choosing a button already used by another action swaps the two.
- **Minimap**: L3 + R3 together toggles it. L3 alone is the crouch toggle.
- **No vibration** in this release. The rumble adapter exists, but no game event drives it.

### Steam Deck

- Steam Deck runs the same native Linux x86_64 AppImage. There is no separate Steam Deck build, no Steamworks or Steam Input SDK dependency and no Wine or Proton layer.
- SDL3 Gamepad provides Steam Deck glyphs and controller navigation. Trackpad and touchscreen input go through SDL3's mouse path.
- The launcher's built-in file browser works without `zenity` or `kdialog`, which is useful in Gaming Mode.
- **Steam Deck support is experimental and has not been validated on Steam Deck hardware in this release.** The 1280x800 layout, controller feel, Wayland and audio devices are prepared and headless-tested, but not confirmed on a physical device.

## Launcher and Installation

The launcher opens before the game and runs in the same process. It uses the TRG Launcher interface (the one from the Dragon's Crown Pro PC and 13 Sentinels ports) over SDL3 and Dear ImGui, with this port's pages and names.

### Pages

| Page | What it does |
|---|---|
| Play | Game status, version selection, Import/Reinstall, Remove version, Show this launcher |
| Display | Display mode, window size, monitor, aspect ratio, presentation |
| Graphics | Renderer, graphics card, render resolution, field of view, anti-aliasing, ambient occlusion, water refraction, enhanced sky, maximum object detail, texture filtering, VSync |
| Gameplay | Language, Enable Minimap, Video subtitles, Skip Intro, Dual stow, Active AI, Official Patch 1.1, FPS in title, Frame counter, Render resolution in title |
| Controls | Gamepad status and bindings, glyphs, look sensitivity, invert Y, deadzones; mouse sensitivity and invert; all keyboard actions and hotkeys |
| Cheats | Movement Speed, Jump Height, Invulnerability, Infinite Ammo, Freeze Dinosaurs, Super Health |
| Bug Report | Description, one privacy-filtered log, preview, send, My Reports |
| About | Credits, translation credits, folders, settings file, reset, port version, game, renderer, platform, libraries, repository link |

There is no online or multiplayer page.

### Importing the game

1. Put your legally obtained Trespasser `.iso` disc image anywhere readable. The image is only read; it is never modified.
2. Open the launcher's **Play** page and choose **Import game...**.
3. The launcher identifies the release from the disc's files by size and SHA-256, never by its file name or volume label.
4. It copies the required files into `game\versions\<id>` (Windows) or `game/versions/<id>` (Linux), verifies each file, and stages the installation so a failure or Cancel leaves the previous installation untouched.
5. When Play says the game is ready, press **PLAY**.

The supported releases are listed in [Supported Game Versions](#supported-game-versions). The installers, DirectX setup, EA registration, help files, original executable and disc image are never copied. The original executable is an import input only and is not needed to play.

**Reinstall game...** repairs a selected version whose files are no longer valid. **Remove version** deletes only that version's folder; saves, settings, other versions and disc images are untouched.

### Portable data

- Windows: `game\`, `user_data\` and `logs\` sit next to `trespasser_recomp.exe` by default. `user_data\launcher\trespasser.ini` holds the settings; `user_data\saves\` holds the game's registry store and saves.
- Linux: `game/`, `user_data/` and `logs/` sit next to the AppImage file, never inside its read-only mount.
- The launcher is portable: moving the folder moves the settings and saves with it.
- `--user_data_root`, `TRESPASSER_USER_DATA_ROOT`, `--game` and `TRESPASSER_GAME_DIR` can override the folders for advanced use.

### Updates

- The launcher checks the project's GitHub Releases for a newer port when it opens.
- It downloads the Windows ZIP or the Linux AppImage, verifies the published SHA-256, unpacks and validates the package, and refuses unsafe or protected paths.
- On Windows, **Restart to update** closes the launcher and starts `trg-updater.exe`, which waits for the program to exit, replaces the files with a rollback journal and starts the new launcher. A running game blocks the install.
- On Linux, the update replaces the AppImage file itself, verifies it again and restarts it with `--launcher`. Outside an AppImage automatic installation is unavailable.
- The player's game files, saves, settings, logs and caches are never part of a package and are never touched.

The first public release has no older version to update from, but the updater is already in place for future releases.

### Bug Report

The **Bug Report** page can send a description and one privacy-filtered game log to a small service operated for this project. Nothing is sent until you press **Send report** and confirm. The preview shows exactly what will be sent, and **Save report** writes the same filtered report to `logs/reports/` for a GitHub issue. Saves, game files, screenshots, settings, crash dumps and other logs are not sent. Automatic filtering cannot promise to catch every private value, which is why the preview exists.

### Installation steps

**Windows:**

1. Download `JurassicParkTrespasserRecompiled-v0.1.0-windows-x86.zip`.
2. Optionally verify its SHA-256 against the value in [SHA-256 Checksums](#sha-256-checksums).
3. Extract it into a writable folder.
4. Run `trespasser_recomp.exe`. Keep `trg-updater.exe` beside it for future updates.
5. Import your disc image from the Play page, then press PLAY.

**Linux:**

1. Download `JurassicParkTrespasserRecompiled-v0.1.0-x86_64.AppImage`.
2. Optionally verify its SHA-256.
3. Make it executable: `chmod +x JurassicParkTrespasserRecompiled-v0.1.0-x86_64.AppImage`.
4. Run it. If FUSE is unavailable, the AppImage extracts itself and runs from the temporary copy; `--appimage-extract-and-run` or `APPIMAGE_EXTRACT_AND_RUN=1` forces that mode.
5. Import your disc image from the Play page, then press PLAY.

The Windows executable is not code-signed, so SmartScreen may warn on first launch. The Linux AppImage is not signed by a distribution; verify the SHA-256 if you downloaded it from a mirror or another machine.

## Languages, Subtitles and Restored Dialogue

### Languages

Seven languages are built into the port: English, Brazilian Portuguese, French, German, Spanish, Russian and Italian. The launcher's **Gameplay > Language** setting chooses one.

| Language | Game text, menus and subtitles | Voices and videos | Launcher |
|---|---|---|---|
| English | English | English | English |
| Brazilian Portuguese | Brazilian Portuguese | English | Brazilian Portuguese |
| French | French | English | English |
| German | German | English | English |
| Spanish | Spanish | English | English |
| Russian | Russian | English | English |
| Italian | Italian | English | English |

- Every language has the game's text, the words on the menu pictures, subtitles, the tutorial and the port's own panels.
- Voices, effects, music and videos stay the English originals. Localised audio and videos are not distributed.
- Brazilian Portuguese also translates the launcher; the other languages keep the launcher in English.
- The port completes the 1998 localisations where they stop: missing subtitles, missing German and Russian strings, and the Italian game text were added for the port.
- Official release text is used where it exists; the port's own translations are marked as PC port completion in the credits.

### Video subtitles

**Gameplay > Video subtitles** (On by default) shows the spoken lines of the videos in the chosen language:

| Video | Dialogue | Cues |
|---|---|---|
| `tpassintro.smk` (opening) | Hammond's narration | 16 |
| `newgame.smk` (after New Game) | the answering machine and Jill's message | 9 |
| `win.smk` (ending) | the answering machine and Jill's last message | 7 |
| `credits.smk` | music only | none |

- Subtitles are drawn over the picture at the window's resolution, white with a dark outline, centred near the bottom of the picture.
- Each cue follows the frame the game's Smacker player shows, so subtitles stop with the video and are never left on the next screen.
- Only retail video files get subtitles: each timeline is tied to the file's size and the SHA-256 of its first 64 KiB. A replaced or modded video plays without them, and the log says so.
- English, Brazilian Portuguese, French, German, Italian, Russian and Spanish are supported.
- The video files, their pixels and their audio are never changed.

### Restored dialogue and corrected captions

The port includes the *English Stream TPA Fix (Subtitle Timing Fix)* by KeyofBlueS, preserved by TresCom, for the English game and for the same temporal events in every language:

- 328 of the retail 402 voice captions are corrected: 309 in timing only, 19 also in line breaks or text.
- Three captions the retail file lacked for audio it has were added.
- Five restored English dialogue records are compiled into the executable: four voice lines the retail levels call but the retail files lack (`VA24` and `VA34` in JR, `VA208` in AS2, the tour guide in BE) and the long version of the Ray computer voice. They total 4,159,690 bytes of PCM.
- The player's own `Stream.tpa` is only read. The restored records are presented as part of the file in memory. The retail audio is never replaced, except the known short `VCRAYCOMPUTER` record, and only when its bytes are the known retail ones.
- The restored lines stay English; their subtitles are translated into every supported language.
- No external translation or audio package is needed. The modified `Stream.tpa` is not distributed and nothing is copied at start-up.

## Bug Fixes and Compatibility

- **Official 1.1 fixes** reimplemented natively: terrain tolerances, partition handling, jump consistency, hand and pickup behaviour, compound-object interpenetration, box-box interaction and selected floating-object placements. See [Gameplay Improvements and Cheats](#gameplay-improvements-and-cheats).
- **Footstep and collision sounds** no longer go silent after loading a save or restarting a level.
- **Widescreen camera culling** was corrected so objects do not disappear at the sides of a Hor+ view.
- **Window presentation** was stabilised: a pending screen change takes effect with the first frame of the new generation, Alt+Tab during a level load no longer reads uncommitted paging memory, and borderless/windowed transitions keep the picture correct.
- **Direct3D 11 water rendering and surface synchronisation** were fixed and optimised.
- **Menu selection audio** was restored, with a lower menu sound-effect volume.
- **Texture conversion and mipmap generation** were accelerated, and perspective/bump polygons can be drawn on the GPU.
- **Save previews** are generated from the frozen gameplay frame and deleted with the save.
- **Multi-version support** was added: English 1.0.116, Russian Fargus 1.0.116, Spanish 1.1.117 and Brazilian Portuguese 1.1.117 each keep their own files, saves, menu art and language.
- **Legacy compatibility checks**: the 1998 100 MB disk-space check is bypassed and the legacy Safe Mode prompt is suppressed by default; developer flags can restore them for comparison.
- **Clean shutdown**: the window's X, Alt+F4 and termination signals lead the game out through its own quit path. A crash leaves a guest/host report in the log, and crash dumps are not included in the release.
- **Localisation fixes**: French `è`/`ê` conversion errors, the Spanish OK button and the Brazilian Credits button are corrected; Russian text uses code page 1251 and the Fargus font is reproduced.

The game's own console (Ctrl+F11) and its normal save/load behaviour remain available. The port does not change the save format.

## Native Windows Port

- The host is **one 32-bit Windows program**, `trespasser_recomp.exe`, built for x86 and running on 64-bit Windows 10/11.
- The retail `.text` is lifted to C once, ahead of time: 17,767 functions in the reference 1.0.116 lift, with 0 lift errors, 0 unimplemented instructions and 0 unresolvable constant targets.
- All 297 imports resolve against the system's 32-bit DLLs. The host maps the guest image at `0x00400000`, forwards its imports to real Windows DLLs and runs the lifted C in place of `.text`.
- **SelfMod**, the self-patching rasteriser section, runs as the original machine code at its original address.
- Three lifts are linked into the one program: English/Russian 1.0.116, Spanish 1.1.117 and Brazilian Portuguese 1.1.117. The launcher selects the lift for the installed version; only the active lift executes.
- The **updater** is a separate 64-bit static CLANG64 build, `trg-updater.exe`. It replaces files with a rollback journal and restarts the launcher.
- **Requirements**: Windows 10 or 11, 64-bit; a Direct3D 11-capable GPU for the D3D11 renderer, or a Vulkan 1.1+ driver for Vulkan; the Software renderer needs no GPU acceleration.
- The Windows executable is not digitally signed.
- **D3D11 is the primary Windows backend.** Vulkan is selectable and has broad parity coverage, but full manual runtime parity has not been proven; use D3D11 unless you are testing Vulkan.

## Native Linux and Steam Deck

- The Linux build is a native **x86_64 ELF** program, not Wine, Proton, Winelib or a PE loader. The guest stays a logical 32-bit Win32 program while the host is 64-bit.
- It runs the same lifted game, the same launcher sources and the same settings file as Windows, with a portable Win32-compatible layer for files, registry, synchronisation, windowing, DirectDraw, audio and video.
- **Requirements**: 64-bit x86 Linux with glibc 2.31 or newer; an X11 or Wayland desktop; a Vulkan 1.1+ driver for Vulkan or no GPU driver for Software; FUSE is optional because the AppImage can extract and run without it.
- **Vulkan** is the default GPU backend. **Software** is the functional fallback for menus, gameplay, video, subtitles, pause and hotkeys.
- **FFmpeg** decodes the Smacker videos; the AppImage carries its own static FFmpeg with only the Smacker demuxer and decoders, under the LGPL.
- **OpenSSL** is linked statically for the Bug Report page, so the AppImage is self-contained. The Vulkan loader, driver, display server and audio server come from the system.
- **Application identity**: `io.github.saulob.jurassic-park-trespasser` is the Wayland `app_id`, the X11 `WM_CLASS` and the `.desktop` name. The window icon is embedded.
- **Portable paths**: `game/`, `user_data/` and `logs/` live beside the original AppImage file, never inside its read-only mount and never in `~/.config` or `~/.local/share`.
- **AppImage**: one type-2 AppImage, with an upstream runtime pinned by SHA-256 and a patch that extracts and runs when FUSE fails. It replaces itself during updates and restarts the original `$APPIMAGE` path.
- **Steam Deck**: the same AppImage, no separate build, no Steamworks or Steam Input dependency. It is **experimental and not hardware-validated** in this release. The 1280x800 layout, Gamescope swapchain handling, controller navigation, trackpad/touchscreen mouse path and portable paths are prepared and headless-tested.

**Linux graphics limits in v0.1.0:** the Windows mode-table override that provides native widescreen gameplay is not implemented on Linux. The guest stays at its 4:3 mode and the picture is presented scaled to the window. HD menus and loading screens work; HD video presentation and native 21:9/32:9 Hor+ gameplay do not. See [Known Issues and Limitations](#known-issues-and-limitations).

## Supported Game Versions

| Release | Version | Windows | Linux |
|---|---|---|---|
| English retail (UK `Trespass_UK_116`, US `Trespasser_116d`) | 1.0.116 | Yes | Yes |
| Russian Fargus (`TRESPASSER_116D`) | 1.0.116 | Yes | Yes |
| Spanish (`TRESPASSER_S`) | 1.1.117 | Yes | Import only; not playable yet |
| Brazilian Portuguese | 1.1.117 | Yes | Import only; not playable yet |

- The disc is recognised by file sizes and SHA-256, never by its file name or volume label.
- The Spanish and Brazilian 1.1.117 releases have their own lifted builds on Windows. The Linux executable carries the English/Russian 1.0.116 lift only; the launcher lists the other two as not playable yet.
- The official v1.1 patch is not needed. Its selected fixes are reimplemented by the port.
- No Japanese release is supported or present in the analysed versions.
- No demo, mod or fan-made total conversion is supported in this release.
- The original executables are import inputs only and are never copied into the installation. No game file is distributed with the port.

## Known Issues and Limitations

- **120 FPS / uncapped frame rate**: not implemented or validated. VSync Off removes the present wait, but there is no frame cap/uncap setting and no guarantee that physics, animation, input or audio behave correctly above 60 FPS. The README lists uncapped frame rate as not yet.
- **Dedicated ultrawide HUD**: not implemented. Menus stay 4:3, gameplay text keeps the game's screen anchors, and there is no ultrawide safe-area layout. 21:9 and 32:9 rendering itself is real Hor+, not stretched.
- **HD textures**: not included. "HD" refers to menus, loading screens, save previews and, on Windows, video presentation. The 3D world uses the original textures with mipmaps, filtering and a higher resolution cap.
- **Linux native widescreen**: the Windows mode-table override is not on Linux. Gameplay stays 4:3 and is presented scaled; 21:9/32:9 Hor+ and native widescreen gameplay are Windows-only in v0.1.0.
- **Linux HD video**: the Windows HD video presentation pipeline is not part of the Linux build. Linux plays the original videos with subtitles.
- **Linux Spanish and Brazilian releases**: they import but are not playable yet because their lifted builds are not in the Linux executable.
- **Steam Deck**: experimental and not validated on hardware. The 1280x800 layout, controller feel, Wayland and audio devices are prepared and headless-tested, not confirmed on a physical Deck.
- **Linux in general**: this is an experimental first release. The Linux gate is headless and was first exercised under WSLg/X11; a broad matrix of real desktops, GPUs, audio devices and Wayland compositors has not been validated.
- **Windows Vulkan**: selectable but not the primary backend. D3D11 remains the recommended path until Vulkan proves full manual runtime parity.
- **No HDR** in this release.
- **No vibration**: the gamepad rumble adapter exists, but no game event drives it.
- **No mods** support and no mod loader.
- **No x64 game host**: the playable host is 32-bit. The 64-bit host is groundwork only and cannot run the game in this release.
- **No custom waypoints**: the minimap and compass show the current objective; you cannot place your own waypoint.
- **Keyboard rebinding** is done in the game's own Controls screen, not the launcher. The launcher lists and reflects the bindings; gamepad bindings are changed in the launcher.
- **Controls screen background**: the rusted panel is the original 318x198 picture scaled up and stays soft until an HD redraw exists. Its text, fields and frames are drawn at screen resolution.
- **Save previews are platform-specific**: Windows writes JPEG previews, Linux writes PNG previews, and neither reads the other's format in this release.
- **Bug Report sends logs only**: no crash dumps, saves, screenshots or game files. Automatic privacy filtering cannot guarantee removal of every private value; use the preview.
- **4K 32:9 budget**: the 7680x2160 box is above the internal gameplay-buffer budget. The game's own screen is reduced to the largest 32:9 mode within budget while the final image is rasterised at the selected box.
- **Unsigned Windows executable**: SmartScreen may warn on first launch. Verify the SHA-256 and the VirusTotal links below.
- **Experimental Linux AppImage**: FUSE may be unavailable; the AppImage then extracts itself. Wayland/X11, audio and controller behaviour may vary by distribution.

## Downloads and Installation

These links use the final v0.1.0 asset paths and resolve once the release is published. Before publication they return 404.

### Windows 10 / 11 x86

- [JurassicParkTrespasserRecompiled-v0.1.0-windows-x86.zip](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.0/JurassicParkTrespasserRecompiled-v0.1.0-windows-x86.zip) — 29,359,834 bytes
- [JurassicParkTrespasserRecompiled-v0.1.0-windows-x86.zip.sha256](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.0/JurassicParkTrespasserRecompiled-v0.1.0-windows-x86.zip.sha256)

Extract the ZIP into a writable folder and run `trespasser_recomp.exe`. Keep `trg-updater.exe` beside it.

### Linux x86_64 / Steam Deck

- [JurassicParkTrespasserRecompiled-v0.1.0-x86_64.AppImage](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.0/JurassicParkTrespasserRecompiled-v0.1.0-x86_64.AppImage) — 24,418,624 bytes
- [JurassicParkTrespasserRecompiled-v0.1.0-x86_64.AppImage.sha256](https://github.com/saulob/jurassic-park-trespasser-recomp/releases/download/v0.1.0/JurassicParkTrespasserRecompiled-v0.1.0-x86_64.AppImage.sha256)

Make the AppImage executable and run it. The same file is intended for desktop Linux and Steam Deck; Steam Deck remains experimental in this release.

**Not published as release assets:** debug symbols, development builds, staging folders, game data, saves, logs, crash dumps and mods. The Windows ZIP contains only the program, the updater, the Visual C++ runtime DLLs, the launcher assets, the README and the licence notices. The AppImage is one file.

## Security and VirusTotal

The four v0.1.0 artifacts were checked with VirusTotal before publication. The results supplied for this release reported **no detections** for any of the four files.

VirusTotal is an additional verification signal, not an absolute guarantee of safety. A clean scan does not prove that a program is free of every possible issue, and an engine can flag a file that another engine considers clean. Verify the SHA-256 values and download only from this repository's Releases page.

| File | SHA-256 | VirusTotal |
|---|---|---|
| `JurassicParkTrespasserRecompiled-v0.1.0-windows-x86.zip` | `4b677fb36519f63221ff25dffd5b5a50a250672db56658b07865810649bc1a36` | [VirusTotal](https://www.virustotal.com/gui/file/4b677fb36519f63221ff25dffd5b5a50a250672db56658b07865810649bc1a36?nocache=1) |
| `trespasser_recomp.exe` (inside the ZIP) | `52b4dc10c9da024b1c10ef152615f1235d21301f9bcacf0d1d6f742f91c0d7fd` | [VirusTotal](https://www.virustotal.com/gui/file/52b4dc10c9da024b1c10ef152615f1235d21301f9bcacf0d1d6f742f91c0d7fd) |
| `trg-updater.exe` (inside the ZIP) | `ba019e55a195d234c8f6b02e6f2bc2dc04aeb0df27ac3aa3570c29cb514c4be6` | [VirusTotal](https://www.virustotal.com/gui/file/ba019e55a195d234c8f6b02e6f2bc2dc04aeb0df27ac3aa3570c29cb514c4be6) |
| `JurassicParkTrespasserRecompiled-v0.1.0-x86_64.AppImage` | `eb4e8d6308fa41407b503668e59536c387da654baf0ffa87bc88b2e4447856d9` | [VirusTotal](https://www.virustotal.com/gui/file/eb4e8d6308fa41407b503668e59536c387da654baf0ffa87bc88b2e4447856d9) |

The Windows executable and the updater are not code-signed. SmartScreen may warn on first launch. The ZIP's own VirusTotal result does not cover the programs inside it; scan the `.exe` entries separately if your security policy requires it.

Before publication, the Windows ZIP was checked with the project's release verifier and reported clean. The four SHA-256 values above match the approved artifacts, and the AppImage is a type-2 image with its approved hash.

Final package inspection found no game data, saves, logs, PDB/debug files, development symbol packages or local development paths in the published Windows ZIP. Debug symbols for both platforms are kept outside the release and are not uploaded.

## SHA-256 Checksums

| File | Size | SHA-256 |
|---|---|---|
| `JurassicParkTrespasserRecompiled-v0.1.0-windows-x86.zip` | 29,359,834 bytes | `4b677fb36519f63221ff25dffd5b5a50a250672db56658b07865810649bc1a36` |
| `JurassicParkTrespasserRecompiled-v0.1.0-x86_64.AppImage` | 24,418,624 bytes | `eb4e8d6308fa41407b503668e59536c387da654baf0ffa87bc88b2e4447856d9` |
| `trespasser_recomp.exe` (inside the ZIP) | 89,398,784 bytes | `52b4dc10c9da024b1c10ef152615f1235d21301f9bcacf0d1d6f742f91c0d7fd` |
| `trg-updater.exe` (inside the ZIP) | 1,291,264 bytes | `ba019e55a195d234c8f6b02e6f2bc2dc04aeb0df27ac3aa3570c29cb514c4be6` |

The matching `.sha256` files accompany the ZIP and the AppImage. Each file contains one line in the form `<sha256>  <file name>`. The ZIP's approved hash is the current v0.1.0 hash; the EXE and updater inside it match the separately approved hashes above.

## Credits and Acknowledgements

- **pcrecomp** by sp00nznet, the static recompilation toolkit used to lift the retail `.text` to C: <https://github.com/sp00nznet/pcrecomp>
- **sp00nznet/trespasser**, the reconnaissance that preceded this port.
- **Trespasser CE** by Lee Arbuco, studied for the CE-inspired gameplay and graphics options. Trespasser CE is not included and is not required.
- **CE Patcher Large Version** by yochenhsieh, hosted by TresCom, used as a reference for CE's configuration options.
- **Trespasser Mod Manager** by MikeTheRaptor, used as a reference for CE's configuration options.
- **TresCom** (<https://www.trescom.org/>) for preserving and hosting the official translations, the subtitle fix, the voiceover reference and the community's documentation.
- **KeyofBlueS** for the English Stream TPA subtitle timing fix and the Italian subtitle timings used as a reference; the modified `Stream.tpa` is not distributed.
- **tatu** for the *Trespasser Voiceover List 3.0*, used as a reference for Jill's lines.
- **Nick3069** for the GameStrings conversions of the official translations.
- **cynodesmus** for the Russian game text and subtitles.
- **TRG Launcher** by TekRantGaming, the launcher base shared with the Dragon's Crown Pro PC and 13 Sentinels ports.
- **SDL3**, **Dear ImGui**, **Intel ASSAO**, **nanosvg**, **miniz**, **FFmpeg**, **OpenSSL** and **Vulkan-Headers**, whose licences are included with the release.
- **DreamWorks Interactive** for the original *Jurassic Park: Trespasser* (1998). This is an unofficial fan preservation project and is not affiliated with or endorsed by DreamWorks, Universal or EA.
- Everyone who tested the port, reported problems and sent logs.

The full legal notes, the third-party licence texts and the source-preservation notes are in `docs/LEGAL.md`, `docs/REFERENCES.md` and the `licenses` folder of the Windows package.
