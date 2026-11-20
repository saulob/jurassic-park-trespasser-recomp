#ifndef JPT_COMPAT_H
#define JPT_COMPAT_H

/* The MSVC string functions the shared sources use, under their names on POSIX. */
#ifndef _WIN32
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strdup strdup
#define _snprintf snprintf
#endif

#endif
