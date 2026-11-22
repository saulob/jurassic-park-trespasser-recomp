#include "file_picker.h"

#ifndef _WIN32
#include <unistd.h>

#include <cstdlib>
#include <sstream>
#include <string>
#endif

namespace jpt {

bool FilePickerAvailable() {
#ifdef _WIN32
    return true;
#else
    const char* path = std::getenv("PATH");
    std::stringstream folders(path ? path : "");
    std::string folder;
    while (std::getline(folders, folder, ':')) {
        for (const char* tool : {"zenity", "kdialog"}) {
            const std::string candidate = (folder.empty() ? std::string(".") : folder) + "/" + tool;
            if (access(candidate.c_str(), X_OK) == 0) return true;
        }
    }
    return false;
#endif
}

}  // namespace jpt
