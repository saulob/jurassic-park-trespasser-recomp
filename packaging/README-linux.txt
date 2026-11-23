Jurassic Park: Trespasser Recompiled - Linux
Version @VERSION@

Jurassic Park: Trespasser (1998), running natively on Linux x86-64. The game's own code runs on the PC through a
replacement for the Windows system libraries, with a Vulkan renderer and a software fallback. This is a native build,
not Wine or Proton. The same AppImage is intended for desktop Linux and the Steam Deck.

Source code and issue tracker:
https://github.com/saulob/jurassic-park-trespasser-recomp


THE GAME ITSELF IS NOT INCLUDED
-------------------------------
No game file is included. You need your own legally obtained Trespasser disc image (.iso): English 1.0.116 (UK or US
disc), Russian (Fargus), Spanish or Brazilian Portuguese. The version is recognised from the disc's files, never from
its file name. The image is only read; it can be kept wherever you like.


REQUIREMENTS
------------
- 64-bit Linux (x86-64) with glibc 2.31 or later
- An X11 or Wayland desktop
- For the Vulkan renderer: a Vulkan 1.1+ driver (Mesa RADV or ANV, NVIDIA). The Software renderer is the functional
  fallback and needs no GPU driver
- A controller is optional; the keyboard and mouse work too
- FUSE is optional. The AppImage normally mounts itself with FUSE; if that is not possible it unpacks itself into a
  temporary folder and runs from there automatically. Setting APPIMAGE_EXTRACT_AND_RUN=1 (or starting with
  --appimage-extract-and-run) forces that mode
- Nothing else: the AppImage carries SDL3, FFmpeg and OpenSSL. The Vulkan loader and driver, the display server and
  the audio server come from your system


INSTALL
-------
1. Put the AppImage in a folder you can write to (it keeps its game files, saves and logs beside itself) and make it
   executable:

     chmod +x JurassicParkTrespasserRecompiled-v@VERSION@-x86_64.AppImage

2. Run it (double-click, or from a terminal). The launcher opens.
3. On the Play page, click "Import game..." and select your .iso disc image. The launcher uses the desktop's chooser
   (zenity or kdialog) when there is one, and its own controller-friendly file browser otherwise (mouse, keyboard or
   controller).
4. Wait while the launcher checks the disc and copies the game files into the "game" folder next to the AppImage. The
   .iso file is only read and can be kept anywhere.
5. When the Play page says the game is ready, press PLAY.

To repair an installation, use "Reinstall game..." on the Play page. Your saves and settings are kept.


UPDATES
-------
The launcher checks the project's GitHub releases once per start. When a newer AppImage exists, an update banner
appears in the header; nothing is installed until you choose UPDATE NOW and then RESTART TO UPDATE. The update
replaces the AppImage you started from, verifies its SHA-256, keeps a backup for rollback and restarts it. Game files,
user data, saves, settings, logs and shader caches in the folders beside the AppImage are kept.


STEAM DECK
----------
Desktop Mode: copy the AppImage to a folder such as ~/Games or an SD card, make it executable (right-click >
Properties > Permissions) and run it once to import the game and set the options with the launcher.

Gaming Mode: in Desktop Mode open Steam, "Add a Game" > "Add a Non-Steam Game...", then Browse and choose the
AppImage (select "All Files" in the chooser). The launcher opens first; use --launcher in the shortcut's Launch
Options to open it again later. Nothing hardcodes /home/deck, and the AppImage works from an SD card.

The same AppImage serves desktop Linux, Steam Deck Desktop Mode and Steam Deck Gaming Mode. There is no separate
Steam Deck build and no Wine or Proton.

Steam Deck support is prepared but has not been validated on real hardware. Linux support itself is experimental: it
is meant to offer everything the Windows version does, but the maintainer has no real Linux PC or Steam Deck. If
something does not work, please attach the newest log from the "logs" folder to an issue, with your distribution, GPU
and driver.


PLAYING
-------
- F1 opens the Options menu; F11 switches between Windowed and Borderless Fullscreen.
- F9 closes the game; the Controls page can rebind it. Holding L1 + R1 + Options on one controller for 2 seconds
  closes it too, and the Controller Exit Shortcut setting turns that off.
- Controllers are read through SDL's gamepad support: Xbox controllers and the Steam Deck's own controls.
- To open the launcher again after turning it off, start the AppImage with --launcher (or use the "Show launcher"
  action of its desktop entry).
- The launcher's Bug Report page sends a report, with the log you choose, only when you press Send report. My Reports
  keeps the reports sent from this computer.


WHERE THINGS ARE
----------------
- Game files:     the "game" folder next to the AppImage file
- Logs:           the "logs" folder next to the AppImage file; if that folder cannot be written, "user_data/logs"
- Settings (trespasser.ini), saves, caches and the Vulkan pipeline cache:
                  the "user_data" folder next to the AppImage file

These folders are created when they are first needed and are not part of the download. They are never written inside
the AppImage's read-only mount. The program stops with a message if the folder beside the AppImage cannot be written;
it never chooses ~/.config or ~/.local/share on its own. --game, --log-dir and --user_data_root choose another place
explicitly.

When updating the port, keep "user_data" (and "game", to skip importing the disc again).

If the game does not start or crashes, please send a report from the launcher's Bug Report page (Open preview shows
exactly what goes out), or attach the newest log from the "logs" folder to a report at
https://github.com/saulob/jurassic-park-trespasser-recomp/issues
The Linux build writes the text log only; it does not make crash dump files.


LICENSE
-------
The port's own source code has no licence file yet; that choice belongs to the repository owner. The legal notes and
the third-party notices are in usr/share/licenses/JurassicParkTrespasserRecompiled/ inside the AppImage (run it with
--appimage-extract to see the files). Third-party components keep their own licences, including SDL3 (zlib), Dear
ImGui (MIT), miniz (MIT), pcrecomp (MIT), Vulkan-Headers (Apache-2.0), FFmpeg (LGPL-2.1-or-later) and OpenSSL
(Apache-2.0).

The game and its assets are not included and are not covered by this project's licence. This port is not affiliated
with or endorsed by DreamWorks Interactive, Electronic Arts or any other rights holder of Jurassic Park: Trespasser.
