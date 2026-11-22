# Saves and settings: the portable `user_data` folder

Everything the port writes for the player lives in one folder, `user_data`, next to the program. There is no setting
for it and no second mode.

```
JurassicParkTrespasser\
    trespasser_recomp.exe
    assets\
    game\                       the installed game, read only at run time
    logs\
    user_data\
        launcher\               trespasser.ini: the launcher's and the port's settings
        versions\<id>\saves\    the same for each other game version (Russian, Spanish, Brazilian Portuguese)
        saves\                  the English 1.0.116 game's own files (its "Installed Directory"):
                                  SAVEGAME.NNN, previews\ (HD save pictures),
                                  recomp-registry.txt (the game's own options),
                                  level swap files (*-130.swp, temp.scx) and the game's screenshots (TPassNNN.BMP)
        cache\                  caches the port may keep: menu-text\<language>\ holds the game's menu files with
                                  their labels translated, rebuilt at each start
```

Copying the whole program folder to another PC carries the saves and settings with it. `user_data` is created at the
first start and is never part of a release download.

The game builds its save, swap file and screenshot paths from its registry value `Installed Directory`; the port
answers it with `user_data\saves`, and `Data Drive` with `game`, so nothing is ever written into the installed game.
The menus also open `<Installed Directory>menu.tpa` (their selection sound, music and birds); when it is not there the
`CreateFileA` shim serves `game\data\Menu.tpa` instead (`src/host/menu_audio_policy.c`).
`trespasser.ini` moved out of the old flat folder into `user_data\launcher`; nothing reads it from beside the program,
so moving it breaks nothing. A `trespasser.ini` that still has a `high_resolution_textures` line (from a build that
had that option) loads normally: the line is ignored and never written again.

## Where the folder is

| System | `user_data` is in |
| --- | --- |
| Windows | next to the executable: the folder that holds `trespasser_recomp.exe` |
| Linux | next to the executable: the folder that holds `trespasser_recomp` |
| Linux AppImage (future; not built yet) | next to the original AppImage file (the `APPIMAGE` variable), never inside its read-only mount |

This is `PortFolder()` in `launcher/user_data.cpp`, which calls `jpt_port_folder()` (`src/common/jpt_port.c`), the same function the Linux
runtime uses (`src/linux/linux_paths.c`). On Linux `game\`, `logs\` and `user_data\` follow it; the only fallback is the log folder of an
AppImage, which moves to `user_data/logs` when the one beside the AppImage file cannot be written. No other location is ever chosen automatically: not
`%LOCALAPPDATA%`, `%APPDATA%`, Documents, `%TEMP%` or any other system folder. `--user_data_root <folder>` (or
`--user_data_root=<folder>`, or the environment variable `TRESPASSER_USER_DATA_ROOT`, the command line winning) is the
alternative when the program folder cannot be written: that folder is used and `<port folder>\user_data` is not
created. `tools\build.ps1 -Run` and `tools\run.ps1` pass `--user_data_root build\user_data`, so a development run keeps
its saves and settings in `build\user_data` and never in the repository root. `build.ps1 -Clean` keeps
`build\user_data`.

The launcher's About page has a **User data** row: it says which folder is in use and opens it.

## Upgrading from the flat `user` folder

Earlier builds kept `trespasser.ini`, `recomp-registry.txt`, the saves and the rest side by side in one folder, `user`:
`<port folder>\user`, or the repository's `user\` for development builds. The first start of this version **moves**
that folder into the root in use, before anything reads or writes it (`SelectUserData` in `launcher/user_data.cpp`):

1. If the root does not exist, or holds only empty folders, and the old folder holds files, the old folder is renamed to
   `<root>\saves` and `trespasser.ini` is moved on to `<root>\launcher`.
2. If the rename is not possible (another drive, a file in use), the old folder is copied into `user_data.migrating\saves`
   next to the root, the copy is checked (same folders, same files, same sizes), `trespasser.ini` is moved to
   `user_data.migrating\launcher`, and only then is `user_data.migrating` renamed to the root.
3. Only after that is the old folder removed.

If any step fails, the old folder is left exactly as it was, no partial `user_data` is ever active, `user_data.migrating`
is removed, the old folder is used for that run in its old layout, one warning is logged, and the move is tried again
at the next start. If only removing the old folder fails, the new root is used and the log says the old folder remains.
This also happens with `--user_data_root`, so a development `build\user_data` takes over the repository's old `user\`.

Even older layouts kept saves in the game folder itself; `SAVEGAME.*` and `recomp-registry.txt` found there are copied
(not moved) into `user_data\saves` when it has no file of that name.

## Cases

| Situation | Result |
| --- | --- |
| No old data, no `user_data` | `user_data` is created and used |
| Old `user` data, no `user_data` (or an empty one) | the old data moves in, as above |
| `user_data` already holds data and an old `user` folder does too | `user_data` is used. The old folder is neither merged nor deleted, and one warning says both exist |
| `user_data` cannot be created or written (a read-only location such as "Program Files") | an error: the log, a message box and the console say that the port folder is not writable and to start with `--user_data_root <a writable folder>` (or set `TRESPASSER_USER_DATA_ROOT`), or to move the program to a writable folder. The launcher and the game do not start, nothing is written, and no other folder is chosen. An old `user` folder is not touched |
| `--user_data_root` given | that folder is used; an old `user` folder still moves into it once. If that folder cannot be written either, the same error is shown for it |

There is no automatic fallback of any kind, and nothing in the launcher asks Windows for a temporary, profile or
documents folder. The launcher's About page shows the root in use; when the root cannot be written the program stops
before the launcher opens, so the error is shown at start-up instead.

The log has one line for each start, for example `User data: portable path selected: ...`, `User data: migrated legacy
data from ... to ...`, `User data: --user_data_root selected: ...` or a `WARNING: User data: ...` line.

## Packaging

`user_data` must never be in a release. `tools\verify-release.ps1 -Path <staged folder or ZIP>` refuses `user_data`,
`user_data.migrating`, the old `user` folder, `saves`, `cache`, `logs`, `downloads`, `staging`, `mods`,
`trespasser.ini`, `recomp-registry.txt`, any `.7z` (also by content), `*.partial`, saves, swap files, game files and
logs, and files that mention this machine's folders or carry `DEBUG_SAULO`. `tools\test-release-check.ps1` checks that a
fresh release passes and that each of those is refused.

## Tests

`tests\user_data_tests.cpp` covers choosing the root, `--user_data_root` and its environment variable, a port folder
that cannot be written (an explicit failure, nothing created, no other location), `--user_data_root` used in its place,
an explicit root that cannot be written, the real write test, a source scan that the launcher's user data code asks for
no system folder, the move by rename and by verified copy, a copy that does not match, failures at each step and the
retry, both folders holding data, an empty `user_data`, the old folder that cannot be removed, the settings file put in
place, and an old `trespasser.ini` with the removed texture-pack line. It uses scratch folders only.
