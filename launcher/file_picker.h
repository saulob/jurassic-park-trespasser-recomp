// Whether the desktop's file picker can be started, so that the launcher can say so instead of doing nothing.
#pragma once

namespace jpt {

// Always true on Windows. On Linux, zenity or kdialog on the PATH (the picker of TRG Launcher starts one of them).
bool FilePickerAvailable();

}  // namespace jpt
