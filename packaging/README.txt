Jurassic Park: Trespasser Recompiled
====================================

A PC port of the 1998 game: it runs as native code on modern Windows, with a Direct3D 11 (or Vulkan) renderer.
No game file is included: the launcher installs the game from your own disc image (.iso).

Start trespasser_recomp.exe. The launcher opens first: Play > Import game, then Play.

  user_data\   your settings and saves (portable: they stay next to the program)
  logs\        one log per run (the Bug Report page sends one of them only when you press Send report)
  game\        the installed game versions

The launcher checks for a newer release and updates the port in place; user_data, logs and game are never touched.
trg-updater.exe is the helper that replaces the program files during an update: keep it next to the program.

vcruntime140.dll and msvcp140.dll are the Microsoft Visual C++ runtime, shipped beside the program (app-local).

Project: https://github.com/saulob/jurassic-park-trespasser-recomp
Third-party licences: licenses\THIRD-PARTY-LICENSES.txt. Legal notes: licenses\LEGAL.txt
