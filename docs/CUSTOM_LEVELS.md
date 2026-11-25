# Custom levels

Levels made by the community, imported in the launcher's **Custom Levels** page and played with the installed game
version selected on the **Play** page; there is no second selector. Nothing is added to or replaced in `game\versions`.

## Import

ZIP, 7z or an extracted folder; the format is read from the file's contents. The package keeps its folders and names
in `user_data\custom_levels\<package>`, unpacked and checked in a staging folder first, so a failed or cancelled import
leaves nothing behind.

| Refused | |
|---|---|
| paths | absolute, drive, `..`, reserved names, the same name twice (any case), a file where a folder is, more than 16 folders deep |
| links | symbolic links and reparse points |
| size | archive 1 GB, file 512 MB, package 2 GB, 8192 entries, 7z block 768 MB |
| contents | encrypted or unknown archives, no Trespasser `.scn` |

Level data, texts and pictures are kept; everything else (programs, libraries, scripts, ATX or CE components) is left
out and counted. A package with the same files as an installed one is refused as a duplicate.

## Play

A package is active for one run. The game still asks for `<Data Drive>\data\X` and the package answers first
(`src/common/jpt_custom_level.c`), the original files second; other packages are never seen. The names in a package may
differ from each other (`LostJungle.scn` with `LJ.wtd`), be in any case, and an animation script may lie beside the
scene. Textures of one name are never mixed with the original game's. While a package is active nothing under `data` is
written: the game's own `.asb`, `.gop` and `Map` files are refused.

- Windows: `CreateFileA`, `GetFileAttributesA`, `LZOpenFileA`, `_open`, `fopen` and MSVCIRT's `_sopen` (terrain and
  animation scripts). Level files and paths LZ32 cannot take (128 characters) are read by the host.
- Linux: `jpt_vpath_resolve_read` with an overlay on `C:\data`.

Once the main menu is up the scene is loaded by the same calls as Level Select (Direct Load). The opening video is
skipped for that run. Saves and level swap files go to `<saves>\custom_levels\<package>`; the game's own settings stay
the version's. A scene whose name the game cannot keep in a save (more than 16 characters, or other than letters,
digits, `_` and `-`) is loaded as `LEVELn.SCN`.

ATX and Trespasser CE scripting are not implemented: a level that needs them can be imported, with its README at hand (the Readme button opens the original file in the system's own application),
but is not supported. Lost Jungle's README says Restart Level does not work in it.
