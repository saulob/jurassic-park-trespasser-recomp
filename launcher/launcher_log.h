// "[launcher] ..." lines of the run log; the host passes its logger in jpt_launcher_config_t.
#pragma once

#include "jpt_user_data.h"

namespace jpt {

void SetLauncherLog(jpt_log_fn log);
void Log(const char* format, ...);

} // namespace jpt
