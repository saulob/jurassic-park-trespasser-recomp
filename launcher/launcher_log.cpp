#include "launcher_log.h"

#include <cstdarg>
#include <cstdio>

namespace jpt {
namespace {

jpt_log_fn g_log = nullptr;

} // namespace

void SetLauncherLog(jpt_log_fn log) { g_log = log; }

void Log(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (g_log) g_log(line);
}

} // namespace jpt
