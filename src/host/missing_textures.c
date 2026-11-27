#include "missing_textures.h"

#include <stdio.h>
#include <string.h>

#include "guest_abi.h"
#include "jpt_compat.h"
#include "jpt_custom_level.h"

#define REPORTS_MAX 128
/* CreateFileA's argument slot where the frames of the loader and its caller leave the text the caller logged. */
#define CALLER_TEXT_ARG 28

static char s_asked[JPT_CUSTOM_LEVEL_PATH_MAX];
static uint32_t s_seen[REPORTS_MAX];
static unsigned s_seen_count;

static const char* file_name(const char* path) {
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (back > slash) slash = back;
    return slash ? slash + 1 : path;
}

static int first_time(const char* name) {
    uint32_t h = 2166136261u;
    for (const char* c = name; *c; c++) h = (h ^ (uint8_t)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c)) * 16777619u;
    for (unsigned i = 0; i < s_seen_count; i++)
        if (s_seen[i] == h) return 0;
    if (s_seen_count == REPORTS_MAX) return 0;
    s_seen[s_seen_count++] = h;
    return 1;
}

static void report(const char* asked) {
    static const char* const k_results[] = { "not there", "there, but not an 8 bit uncompressed BMP", "found" };
    jpt_texture_try_t t;
    char other[JPT_CUSTOM_LEVEL_PATH_MAX];
    int unusable = 0;
    host_log("custom-level", "texture not found: %s, asked for by %s of %s", asked, jpt_custom_level_current_scene(), jpt_custom_level_dir());
    for (int i = 0; jpt_custom_level_texture_try(asked, i, &t); i++) {
        host_log("custom-level", "  %s %s: %s%s%s", t.in_game ? "game   " : "package", t.relative, k_results[t.result], t.file[0] ? ", " : "", t.file);
        unusable |= t.result == JPT_TEXTURE_UNUSABLE;
    }
    host_log("custom-level", "  %s: the level's texture set does not have it, and %s; Trespasser CE's grey texture stands for it", file_name(asked),
             unusable ? "no file of that name is one the original loader takes" : "no root has a file of that name");
    if (jpt_custom_level_texture_other(asked, other, sizeof other)) host_log("custom-level", "  the package has %s, which none of these places is or the original loader cannot read", other);
    if (s_seen_count == REPORTS_MAX) host_log("custom-level", "more textures were not found: only the first %d are reported", REPORTS_MAX);
}

static int asked_as_height_map(void) {
    char text[16];
    uint32_t va = 0;
    return guest_read32(g_esp + 4 + 4 * CALLER_TEXT_ARG, &va) && va && guest_read_string(va, text, sizeof text) && !strncmp(text, "BUMP_LOAD", 9);
}

static void session_ended(void) {
    s_asked[0] = 0;
    s_seen_count = 0;
}

/* The loader asks for the whole path, then for each of its tails; the bare file name is the last attempt. */
int missing_texture_attempt(const char* path, uint8_t** picture, size_t* size) {
    *picture = NULL;
    if (!jpt_custom_level_is_texture_request(path)) return 0;
    jpt_custom_level_on_end(session_ended);
    if (file_name(path) != path) {
        if (_stricmp(file_name(path), file_name(s_asked))) snprintf(s_asked, sizeof s_asked, "%s", path);
        return 1;
    }
    if (_stricmp(file_name(s_asked), path)) snprintf(s_asked, sizeof s_asked, "%s", path);
    *picture = jpt_custom_level_missing_texture(asked_as_height_map(), size);
    if (first_time(s_asked)) report(s_asked);
    s_asked[0] = 0;
    return 1;
}
