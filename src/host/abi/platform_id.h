#ifndef TRESPASSER_PLATFORM_ID_H
#define TRESPASSER_PLATFORM_ID_H

/* The platform a build targets, for the policies that differ between Windows and Linux (audio driver, Smacker binding). */
typedef enum { JPT_PLATFORM_WINDOWS, JPT_PLATFORM_LINUX, JPT_PLATFORM_OTHER } jpt_platform_t;

static inline jpt_platform_t jpt_current_platform(void) {
#if defined(_WIN32)
    return JPT_PLATFORM_WINDOWS;
#elif defined(__linux__)
    return JPT_PLATFORM_LINUX;
#else
    return JPT_PLATFORM_OTHER;
#endif
}

#endif
