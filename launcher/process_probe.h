// The running program's own file, and whether another process runs the same file (an update replaces it).
#pragma once

#include <filesystem>

namespace jpt {

std::filesystem::path CurrentExecutablePath();
bool OtherProcessRunning(const std::filesystem::path& executable);

} // namespace jpt
