# Release security checks

Run these on the final artifacts, after the last package run. Repackaging changes the ZIP hash, so a scan only counts
for the exact file that gets published.

A clean ZIP scan does not cover the programs inside it: an engine can flag an executable in a ZIP that scanned clean.
Scan each file below on its own. Uploads stay manual: no script uploads, publishes or tags anything.

| File | Why |
| --- | --- |
| `JurassicParkTrespasserRecompiled-v<version>-windows-x86.zip` | What Windows players download |
| `trespasser_recomp.exe`, from the staged folder of that ZIP | The program Windows players run |
| `trg-updater.exe`, from the same staged folder | The helper the launcher starts to update itself |
| `JurassicParkTrespasserRecompiled-v<version>-x86_64.AppImage` | What Linux players download |

The package scripts print the SHA-256 of everything they write: `tools\package-release.ps1` runs both,
`tools\package.ps1` alone prints the ZIP, the program and the helper. Check that each file you upload has the hash
that was printed. The staged folder is
`dist\JurassicParkTrespasserRecompiled-v<version>-windows-x86`. The AppImage is one file: scan it as published.

The program is the 32-bit recomp host. The helper is the separate CLANG64 x64 build of `tools\build-updater.ps1`, static
and with only Windows system DLLs; its debug file stays outside the ZIP. The DLLs beside the program are the Microsoft
Visual C++ runtime, copied unchanged from the Visual Studio redistributable; scan them separately only if an engine
names one.

## Self-updater

Publish the ZIP with its `.zip.sha256` file and the AppImage with its `.AppImage.sha256` file, together, as assets of one
release tagged `v<version>` that is neither a draft nor a prerelease. Installed copies read
`https://api.github.com/repos/saulob/jurassic-park-trespasser-recomp/releases/latest` and offer a version newer than
their own; a release without the ZIP under that exact name, or without a SHA-256 for it, is not offered or not
installed.

What the updater checks before anything in the installation changes:

- Every request and redirect is HTTPS on the default port to `api.github.com`, `github.com`,
  `objects.githubusercontent.com` or `release-assets.githubusercontent.com`, without user information in the address. The
  package and checksum addresses must also start with this repository's `releases/download/` address.
- Transfers stop on a stall, after a total time limit, or when cancelled, and download to a temporary `.part` file in a
  temporary folder outside the installation. A failure removes that folder; a retry downloads again from the start.
- The ZIP must be exactly the published size and match the SHA-256 GitHub reports for the asset, or else the one in the
  published `.zip.sha256` file. A wrong or missing checksum stops the update.
- The ZIP is extracted to the temporary folder and refused if any entry is unsafe (absolute, `..`, links, reserved
  names) or protected: game data (`trespass.exe`, the `game` and `versions` folders, `.tpa`, `.smk`, `.wtd`, `.iso`,
  `.cue` files), user data (`user_data`, saves, `trespasser.ini`, `recomp-registry.txt`, logs, caches) or archives. It
  must hold `trespasser_recomp.exe` and `trg-updater.exe`.

The launcher then closes and `trg-updater.exe`, copied out of the installation, replaces only the files the package
holds, after backing up each one it replaces. Each new file is verified against its staged copy. If any replacement or
the restart of the launcher fails, the backed-up files are restored and the previous launcher is started again; the
helper's log is in the temporary update folder. Nothing outside the package's file list is changed or removed. The
launcher will not start the install while a game started from the same installation is still running.

The AppImage updates itself instead: the downloaded AppImage must have the published size and SHA-256 and be a type 2
AppImage. It is copied next to the installed AppImage file (`$APPIMAGE`, never its read-only mount or a folder under
`/tmp/.mount_*`), given the installed file's mode plus execute permission, verified again, and renamed over it after the
old file is kept as a backup. The same file path is then started with `--launcher`; if it cannot start, the backup is
renamed back. `game`, `user_data` and `logs` next to the AppImage are never touched.

## Release report

Record all four values before publishing.

| File | SHA-256 | VirusTotal URL |
| --- | --- | --- |
| Windows ZIP | | |
| Linux AppImage | | |
| `trespasser_recomp.exe` | | |
| `trg-updater.exe` | | |

## Before publishing

Do not publish while an engine flags any of these files, unless the detection has been investigated to a conclusion
and that conclusion is written down.

Never work around a detection by packing, obfuscating or encrypting the program, by disabling a security product or
by asking players to add an exclusion.
