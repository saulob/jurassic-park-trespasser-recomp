#include "menu_audio_policy.h"

#include <stdio.h>
#include <string.h>

static int same_char(char a, char b) {
    if (a >= 'A' && a <= 'Z') a = (char)(a + 'a' - 'A');
    if (b >= 'A' && b <= 'Z') b = (char)(b + 'a' - 'A');
    return a == b;
}

static int same_text(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!same_char(a[i], b[i])) return 0;
    return 1;
}

int menu_audio_source(const char* path, const char* installed_dir, const char* game_dir, char* out, size_t cap) {
    static const char name[] = "menu.tpa";
    size_t dir;
    int n;
    if (!path || !installed_dir || !game_dir || !out || !cap) return 0;
    dir = strlen(installed_dir);
    while (dir && (installed_dir[dir - 1] == '\\' || installed_dir[dir - 1] == '/')) dir--;
    if (!dir || strlen(path) != dir + 1 + sizeof name - 1) return 0;
    if (!same_text(path, installed_dir, dir) || path[dir] != '\\' || !same_text(path + dir + 1, name, sizeof name - 1)) return 0;
    n = snprintf(out, cap, "%s\\data\\Menu.tpa", game_dir);
    return n > 0 && (size_t)n < cap;
}
