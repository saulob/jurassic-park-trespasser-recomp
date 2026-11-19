#include "audio_policy.h"

#include <math.h>
#include <string.h>

int audio_clamp_volume(int hundredths_db) {
    if (hundredths_db < DS_VOLUME_MIN) return DS_VOLUME_MIN;
    return hundredths_db > DS_VOLUME_MAX ? DS_VOLUME_MAX : hundredths_db;
}

float audio_db_gain(float db) {
    if (db >= 0.0f) return 1.0f;
    return powf(10.0f, db / 20.0f);
}

float audio_volume_gain(int hundredths_db) {
    int v = audio_clamp_volume(hundredths_db);
    return v <= DS_VOLUME_MIN ? 0.0f : audio_db_gain((float)v / 100.0f);
}

int audio_resolve_frequency(uint32_t requested, uint32_t original) {
    if (requested == 0) return (int)original;
    if (requested < DS_FREQUENCY_MIN || requested > DS_FREQUENCY_MAX) return -1;
    return (int)requested;
}

static int same_text(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

int audio_is_dsound_library(const char* name) {
    const char* base = name;
    if (!name) return 0;
    for (const char* p = name; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    return same_text(base, "dsound.dll");
}

int audio_is_dsound_create(const char* name) {
    return name && (uintptr_t)name > 0xFFFF && !strcmp(name, "DirectSoundCreate");
}
