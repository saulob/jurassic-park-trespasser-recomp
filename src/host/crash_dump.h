#ifndef JPT_CRASH_DUMP_H
#define JPT_CRASH_DUMP_H

#include <stddef.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

int jpt_crash_dump_path(char *out, size_t size, const char *log_path, int attempt);
int jpt_crash_dump_install(void);
int jpt_crash_dump_write(EXCEPTION_POINTERS *info, DWORD thread);

#ifdef __cplusplus
}
#endif
#endif
