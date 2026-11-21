#include "jpt_video_subtitles.h"

#include <string.h>

#define CUES(a) a, (int)(sizeof a / sizeof a[0])

#define K_INTRO_0 "Video::My name is John Parker Hammond. I was born on March 14, 1928."
#define K_INTRO_1 "Video::What follows is a record of certain events"
#define K_INTRO_2 "Video::in which I took part, between the years 1980 and 1997,"
#define K_INTRO_3 "Video::on an island I will call Site B."
#define K_INTRO_4 "Video::Site B was not to be a theme park, but a research station."
#define K_INTRO_5 "Video::This was where we did the real work."
#define K_INTRO_6 "Video::By 1989, International Genetic Technologies had succeeded in their design,"
#define K_INTRO_7 "Video::to genetically recreate the dinosaurs."
#define K_INTRO_8 "Video::It was an unprecedented accomplishment, the pinnacle of 20th century science,"
#define K_INTRO_9 "Video::a work to rank with the achievements of Galileo, or Einstein."
#define K_INTRO_10 "Video::But it was not all so easy or so simple as it appeared."
#define K_INTRO_11 "Video::One seldom hears the true history of such events,"
#define K_INTRO_12 "Video::what happened at the place where the world changed."
#define K_INTRO_13 "Video::How it began..."
#define K_INTRO_14 "Video::...what were the reasons..."
#define K_INTRO_15 "Video::...what was the cost."

#define K_NEW_GAME_0 "Video::The person you're trying to reach is unavailable."
#define K_NEW_GAME_1 "Video::Please leave a message after the tone."
#define K_NEW_GAME_2 "Video::Anne? Anne, are you there?"
#define K_NEW_GAME_3 "Video::It's Jill, pick up the phone!"
#define K_NEW_GAME_4 "Video::God, don't tell me your mom was serious? You're in Costa Rica?"
#define K_NEW_GAME_5 "Video::Visiting the natives, huh? Sun, sand and adventure."
#define K_NEW_GAME_6 "Video::My little Indiana Anne, world traveler extraordinaire."
#define K_NEW_GAME_7 "Video::Well, give me a call when you get back, okay?"
#define K_NEW_GAME_8 "Video::By the way, I thought you hated flying?"

#define K_WIN_0 "Video::The person you're trying to reach is unavailable."
#define K_WIN_1 "Video::Please leave a message after the tone."
#define K_WIN_2 "Video::Anne! It's Jill. This is like the eighth message I'm leaving you."
#define K_WIN_3 "Video::I'm sensing some sort of pattern here..."
#define K_WIN_4 "Video::Come on, Anne. Quit being antisocial and pick up the phone sometime."
#define K_WIN_5 "Video::Oh, and by the way, don't even bother calling me"
#define K_WIN_6 "Video::unless you have a damn good excuse."

#define K_INTRO_0 "Video::My name is John Parker Hammond. I was born on March 14, 1928."
#define K_INTRO_1 "Video::What follows is a record of certain events"
#define K_INTRO_2 "Video::in which I took part, between the years 1980 and 1997,"
#define K_INTRO_3 "Video::on an island I will call Site B."
#define K_INTRO_4 "Video::Site B was not to be a theme park, but a research station."
#define K_INTRO_5 "Video::This was where we did the real work."
#define K_INTRO_6 "Video::By 1989, International Genetic Technologies had succeeded in their design,"
#define K_INTRO_7 "Video::to genetically recreate the dinosaurs."
#define K_INTRO_8 "Video::It was an unprecedented accomplishment, the pinnacle of 20th century science,"
#define K_INTRO_9 "Video::a work to rank with the achievements of Galileo, or Einstein."
#define K_INTRO_10 "Video::But it was not all so easy or so simple as it appeared."
#define K_INTRO_11 "Video::One seldom hears the true history of such events,"
#define K_INTRO_12 "Video::what happened at the place where the world changed."
#define K_INTRO_13 "Video::How it began..."
#define K_INTRO_14 "Video::...what were the reasons..."
#define K_INTRO_15 "Video::...what was the cost."

#define K_NEW_GAME_0 "Video::The person you're trying to reach is unavailable."
#define K_NEW_GAME_1 "Video::Please leave a message after the tone."
#define K_NEW_GAME_2 "Video::Anne? Anne, are you there?"
#define K_NEW_GAME_3 "Video::It's Jill, pick up the phone!"
#define K_NEW_GAME_4 "Video::God, don't tell me your mom was serious? You're in Costa Rica?"
#define K_NEW_GAME_5 "Video::Visiting the natives, huh? Sun, sand and adventure."
#define K_NEW_GAME_6 "Video::My little Indiana Anne, world traveler extraordinaire."
#define K_NEW_GAME_7 "Video::Well, give me a call when you get back, okay?"
#define K_NEW_GAME_8 "Video::By the way, I thought you hated flying?"

#define K_WIN_0 "Video::The person you're trying to reach is unavailable."
#define K_WIN_1 "Video::Please leave a message after the tone."
#define K_WIN_2 "Video::Anne! It's Jill. This is like the eighth message I'm leaving you."
#define K_WIN_3 "Video::I'm sensing some sort of pattern here..."
#define K_WIN_4 "Video::Come on, Anne. Quit being antisocial and pick up the phone sometime."
#define K_WIN_5 "Video::Oh, and by the way, don't even bother calling me"
#define K_WIN_6 "Video::unless you have a damn good excuse."

static const jvs_cue_t k_intro[] = {
    { 0, 82, K_INTRO_0 },
    { 89, 124, K_INTRO_1 },
    { 125, 194, K_INTRO_2 },
    { 197, 245, K_INTRO_3 },
    { 251, 314, K_INTRO_4 },
    { 315, 364, K_INTRO_5 },
    { 447, 535, K_INTRO_6 },
    { 536, 587, K_INTRO_7 },
    { 621, 707, K_INTRO_8 },
    { 708, 775, K_INTRO_9 },
    { 848, 910, K_INTRO_10 },
    { 919, 973, K_INTRO_11 },
    { 974, 1030, K_INTRO_12 },
    { 1035, 1057, K_INTRO_13 },
    { 1061, 1090, K_INTRO_14 },
    { 1095, 1139, K_INTRO_15 },
};

static const jvs_cue_t k_intro_ru[] = {
    { 0, 94, K_INTRO_0 },
    { 95, 128, K_INTRO_1 },
    { 153, 215, K_INTRO_2 },
    { 216, 261, K_INTRO_3 },
    { 262, 319, K_INTRO_4 },
    { 320, 360, K_INTRO_5 },
    { 442, 530, K_INTRO_6 },
    { 531, 596, K_INTRO_7 },
    { 605, 691, K_INTRO_8 },
    { 692, 788, K_INTRO_9 },
    { 833, 936, K_INTRO_10 },
    { 942, 998, K_INTRO_11 },
    { 999, 1032, K_INTRO_12 },
    { 1066, 1090, K_INTRO_13 },
    { 1101, 1123, K_INTRO_14 },
    { 1132, 1146, K_INTRO_15 },
};

static const jvs_cue_t k_intro_es[] = {
    { 0, 80, K_INTRO_0 },
    { 81, 127, K_INTRO_1 },
    { 128, 196, K_INTRO_2 },
    { 197, 235, K_INTRO_3 },
    { 236, 312, K_INTRO_4 },
    { 313, 360, K_INTRO_5 },
    { 449, 541, K_INTRO_6 },
    { 542, 585, K_INTRO_7 },
    { 628, 712, K_INTRO_8 },
    { 713, 767, K_INTRO_9 },
    { 852, 918, K_INTRO_10 },
    { 919, 967, K_INTRO_11 },
    { 968, 1019, K_INTRO_12 },
    { 1020, 1042, K_INTRO_13 },
    { 1043, 1070, K_INTRO_14 },
    { 1075, 1102, K_INTRO_15 },
};

static const jvs_cue_t k_intro_ptbr[] = {
    { 0, 93, K_INTRO_0 },
    { 94, 135, K_INTRO_1 },
    { 136, 214, K_INTRO_2 },
    { 215, 256, K_INTRO_3 },
    { 257, 329, K_INTRO_4 },
    { 330, 364, K_INTRO_5 },
    { 449, 551, K_INTRO_6 },
    { 552, 593, K_INTRO_7 },
    { 627, 709, K_INTRO_8 },
    { 710, 773, K_INTRO_9 },
    { 852, 901, K_INTRO_10 },
    { 905, 962, K_INTRO_11 },
    { 963, 1008, K_INTRO_12 },
    { 1009, 1031, K_INTRO_13 },
    { 1032, 1069, K_INTRO_14 },
    { 1070, 1119, K_INTRO_15 },
};

static const jvs_cue_t k_new_game[] = {
    { 368, 399, K_NEW_GAME_0 },
    { 400, 433, K_NEW_GAME_1 },
    { 435, 460, K_NEW_GAME_2 },
    { 461, 493, K_NEW_GAME_3 },
    { 500, 539, K_NEW_GAME_4 },
    { 545, 586, K_NEW_GAME_5 },
    { 591, 634, K_NEW_GAME_6 },
    { 639, 671, K_NEW_GAME_7 },
    { 678, 730, K_NEW_GAME_8 },
};

static const jvs_cue_t k_new_game_ru[] = {
    { 372, 404, K_NEW_GAME_0 },
    { 405, 435, K_NEW_GAME_1 },
    { 436, 457, K_NEW_GAME_2 },
    { 458, 478, K_NEW_GAME_3 },
    { 487, 523, K_NEW_GAME_4 },
    { 526, 561, K_NEW_GAME_5 },
    { 580, 632, K_NEW_GAME_6 },
    { 646, 673, K_NEW_GAME_7 },
    { 678, 719, K_NEW_GAME_8 },
};

static const jvs_cue_t k_new_game_es[] = {
    { 359, 401, K_NEW_GAME_0 },
    { 402, 436, K_NEW_GAME_1 },
    { 440, 467, K_NEW_GAME_2 },
    { 468, 501, K_NEW_GAME_3 },
    { 502, 555, K_NEW_GAME_4 },
    { 556, 608, K_NEW_GAME_5 },
    { 609, 657, K_NEW_GAME_6 },
    { 658, 689, K_NEW_GAME_7 },
    { 690, 723, K_NEW_GAME_8 },
};

static const jvs_cue_t k_new_game_ptbr[] = {
    { 357, 413, K_NEW_GAME_0 },
    { 414, 433, K_NEW_GAME_1 },
    { 442, 471, K_NEW_GAME_2 },
    { 472, 497, K_NEW_GAME_3 },
    { 498, 552, K_NEW_GAME_4 },
    { 553, 603, K_NEW_GAME_5 },
    { 604, 659, K_NEW_GAME_6 },
    { 660, 692, K_NEW_GAME_7 },
    { 693, 722, K_NEW_GAME_8 },
};

static const jvs_cue_t k_win[] = {
    { 278, 314, K_WIN_0 },
    { 315, 344, K_WIN_1 },
    { 354, 406, K_WIN_2 },
    { 413, 439, K_WIN_3 },
    { 443, 487, K_WIN_4 },
    { 491, 524, K_WIN_5 },
    { 525, 550, K_WIN_6 },
};

static const jvs_cue_t k_win_ru[] = {
    { 272, 315, K_WIN_0 },
    { 316, 337, K_WIN_1 },
    { 338, 391, K_WIN_2 },
    { 392, 433, K_WIN_3 },
    { 438, 488, K_WIN_4 },
    { 492, 526, K_WIN_5 },
    { 527, 555, K_WIN_6 },
};

static const jvs_cue_t k_win_es[] = {
    { 273, 315, K_WIN_0 },
    { 316, 351, K_WIN_1 },
    { 356, 408, K_WIN_2 },
    { 409, 432, K_WIN_3 },
    { 433, 475, K_WIN_4 },
    { 476, 512, K_WIN_5 },
    { 513, 554, K_WIN_6 },
};

static const jvs_cue_t k_win_ptbr[] = {
    { 272, 317, K_WIN_0 },
    { 318, 346, K_WIN_1 },
    { 357, 409, K_WIN_2 },
    { 410, 431, K_WIN_3 },
    { 435, 479, K_WIN_4 },
    { 480, 515, K_WIN_5 },
    { 516, 553, K_WIN_6 },
};

static const jvs_fingerprint_t k_intro_files[] = {
    { "en-116", 29473692u, "8df7c0d6c0e809cf17c644b81c4e1ba40fde487227d35dfccb1e22323c1cc915" },
};
static const jvs_fingerprint_t k_intro_ru_files[] = {
    { "ru-116", 27176048u, "e04b66166d33e113f71b2a71c25518a7e6b929c4a133bac14b82066e7df1d616" },
};
static const jvs_fingerprint_t k_intro_es_files[] = {
    { "es-117", 29403480u, "ca8d85c9aa12982193a0fecc76df64381e2124d383be074bb46f8760b0a80c2d" },
};
static const jvs_fingerprint_t k_intro_ptbr_files[] = {
    { "ptbr-117", 29453916u, "1a013e0994f1e6f5b9d0ac755ce06329fb0d0b558d112ee0622823b607f575d7" },
};

static const jvs_fingerprint_t k_new_game_files[] = {
    { "en-116", 15502788u, "98a379a48847e603c54ea84ee95c872eafdadbbd49e9a4a8e7d75e6a218b7770" },
};
static const jvs_fingerprint_t k_new_game_ru_files[] = {
    { "ru-116", 13974292u, "d45e2314d561c18851bcd9e2c31df3a3c1ff4ec93fc86b57b7623c6cc6b32798" },
};
static const jvs_fingerprint_t k_new_game_es_files[] = {
    { "es-117", 15510172u, "447bc9f34d11a785a850f7ebb5b4afe06f223decda2f6e6344114210a6dde92a" },
};
static const jvs_fingerprint_t k_new_game_ptbr_files[] = {
    { "ptbr-117", 15496048u, "3c13a9c2e9495a55dbca840bd149cf051933417356f04093d282d261234787a0" },
};

static const jvs_fingerprint_t k_win_files[] = {
    { "en-116", 13227104u, "fbe95bb588dc4b639e4b7a3177b25623482dcbb1d6661db5780bf5346bc01ab9" },
};
static const jvs_fingerprint_t k_win_ru_files[] = {
    { "ru-116", 12286452u, "c99528607bf25e2e2812c71ba2a857475a874003256c78c0a38a9acaf2eb18a8" },
};
static const jvs_fingerprint_t k_win_es_files[] = {
    { "es-117", 13247472u, "6a48b4c62a5a08fa6012d67e61effab53e941b8fbe3c03f04073744f4dc70449" },
};
static const jvs_fingerprint_t k_win_ptbr_files[] = {
    { "ptbr-117", 13274428u, "23075ee5cc85eaa46858aaa09cbee6bce2acac2db13d5ac73b63803832605e0e" },
};

const jvs_timeline_t jvs_timelines[] = {
    { "tpassintro.smk", 1585u, CUES(k_intro), CUES(k_intro_files) },
    { "tpassintro.smk", 1585u, CUES(k_intro_ru), CUES(k_intro_ru_files) },
    { "tpassintro.smk", 1585u, CUES(k_intro_es), CUES(k_intro_es_files) },
    { "tpassintro.smk", 1585u, CUES(k_intro_ptbr), CUES(k_intro_ptbr_files) },
    { "newgame.smk", 1085u, CUES(k_new_game), CUES(k_new_game_files) },
    { "newgame.smk", 1085u, CUES(k_new_game_ru), CUES(k_new_game_ru_files) },
    { "newgame.smk", 1085u, CUES(k_new_game_es), CUES(k_new_game_es_files) },
    { "newgame.smk", 1085u, CUES(k_new_game_ptbr), CUES(k_new_game_ptbr_files) },
    { "win.smk", 606u, CUES(k_win), CUES(k_win_files) },
    { "win.smk", 606u, CUES(k_win_ru), CUES(k_win_ru_files) },
    { "win.smk", 606u, CUES(k_win_es), CUES(k_win_es_files) },
    { "win.smk", 606u, CUES(k_win_ptbr), CUES(k_win_ptbr_files) },
};
const int jvs_timeline_count = (int)(sizeof jvs_timelines / sizeof jvs_timelines[0]);

static int same_name(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

const jvs_timeline_t* jvs_timeline_named(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < jvs_timeline_count; i++)
        if (same_name(name, jvs_timelines[i].name)) return &jvs_timelines[i];
    return NULL;
}

int jvs_fingerprint_matches(const jvs_timeline_t* t, uint64_t size, const char* sha256_hex) {
    if (!t || !sha256_hex) return 0;
    for (int i = 0; i < t->fingerprint_count; i++)
        if (size == t->fingerprints[i].size && same_name(sha256_hex, t->fingerprints[i].sha256)) return 1;
    return 0;
}

const jvs_timeline_t* jvs_timeline_find(const jvs_timeline_t* table, int count, const char* name, uint64_t size,
                                        const char* sha256_hex) {
    if (!table) {
        table = jvs_timelines;
        count = jvs_timeline_count;
    }
    if (!name) return NULL;
    for (int i = 0; i < count; i++)
        if (same_name(name, table[i].name) && jvs_fingerprint_matches(&table[i], size, sha256_hex)) return &table[i];
    return NULL;
}

jvs_choice_t jvs_select(int enabled, const char* name, uint64_t size, const char* sha256_hex, const jvs_timeline_t** out) {
    *out = NULL;
    if (!enabled) return JVS_DISABLED;
    if (!jvs_timeline_named(name)) return JVS_NO_DIALOGUE;
    *out = jvs_timeline_find(NULL, 0, name, size, sha256_hex);
    return *out ? JVS_SELECTED : JVS_NOT_RETAIL;
}

int jvs_cue_at(const jvs_timeline_t* t, int32_t frame) {
    if (!t || frame < 0) return -1;
    for (int i = 0; i < t->cue_count; i++) {
        if (frame < t->cues[i].first) return -1;
        if (frame <= t->cues[i].last) return i;
    }
    return -1;
}

double jvs_frame_ms(int32_t frame, double ms_per_frame) { return (double)frame * ms_per_frame; }

void jvs_session_begin(jvs_session_t* s, const jvs_timeline_t* t) {
    memset(s, 0, sizeof *s);
    s->timeline = t;
    s->playing = t != NULL;
    s->last_frame = -1;
}

int jvs_session_cue(jvs_session_t* s, int32_t frame) {
    if (!s->playing || s->ended) return -1;
    if (frame < s->last_frame) {
        s->ended = 1;
        return -1;
    }
    s->last_frame = frame;
    return jvs_cue_at(s->timeline, frame);
}

void jvs_session_end(jvs_session_t* s) {
    s->playing = 0;
    s->timeline = NULL;
}

int jvs_font_px(const jvs_rect_t* picture) {
    int px = (picture->h * 52 + 500) / 1000;
    return px < 12 ? 12 : px;
}

int jvs_outline_px(int font_px) {
    int r = (font_px + 8) / 16;
    return r < 1 ? 1 : r;
}

static int safe_margin(int size) {
    int m = size * 4 / 100;
    return m < 2 ? 2 : m;
}

int jvs_max_line_width(const jvs_rect_t* picture, int target_w) {
    int w = picture->w * 88 / 100, safe = target_w - 2 * safe_margin(target_w);
    return w < safe ? w : safe;
}

jvs_rect_t jvs_place(int target_w, int target_h, const jvs_rect_t* picture, int block_w, int block_h) {
    jvs_rect_t r;
    int mx = safe_margin(target_w), my = safe_margin(target_h), bottom;
    r.w = block_w;
    r.h = block_h;
    r.x = picture->x + (picture->w - block_w) / 2;
    bottom = picture->y + picture->h - picture->h * 5 / 100;
    if (bottom > target_h - my) bottom = target_h - my;
    r.y = bottom - block_h;
    if (r.x + r.w > target_w - mx) r.x = target_w - mx - r.w;
    if (r.x < mx) r.x = mx;
    if (r.y < my) r.y = my;
    return r;
}

static int measure(const char* text, int start, int end, jvs_width_fn width, void* ctx) {
    while (start < end && text[start] == ' ') start++;
    while (end > start && text[end - 1] == ' ') end--;
    return end > start ? width(ctx, text + start, end - start) : 0;
}

static jvs_line_t line_of(const char* text, int start, int end, jvs_width_fn width, void* ctx) {
    jvs_line_t l;
    while (start < end && text[start] == ' ') start++;
    while (end > start && text[end - 1] == ' ') end--;
    l.start = start;
    l.length = end - start;
    l.width = l.length ? width(ctx, text + start, l.length) : 0;
    return l;
}

int jvs_wrap(const char* text, int max_width, jvs_width_fn width, void* ctx, jvs_line_t* lines) {
    int n = (int)strlen(text), best = -1, best_w = 0, count = 0, start = 0;
    if (!n) return 0;
    if (measure(text, 0, n, width, ctx) <= max_width) {
        lines[0] = line_of(text, 0, n, width, ctx);
        return 1;
    }
    for (int i = 1; i < n; i++) {
        int w1, w2, worst;
        if (text[i] != ' ') continue;
        w1 = measure(text, 0, i, width, ctx);
        w2 = measure(text, i, n, width, ctx);
        worst = w1 > w2 ? w1 : w2;
        if (worst <= max_width && (best < 0 || worst < best_w)) {
            best = i;
            best_w = worst;
        }
    }
    if (best >= 0) {
        lines[0] = line_of(text, 0, best, width, ctx);
        lines[1] = line_of(text, best, n, width, ctx);
        return 2;
    }
    while (start < n && count < JVS_MAX_LINES) {
        int end = n, last_space = -1;
        if (count < JVS_MAX_LINES - 1 && measure(text, start, n, width, ctx) > max_width) {
            for (int i = start + 1; i < n; i++)
                if (text[i] == ' ') {
                    if (measure(text, start, i, width, ctx) > max_width) break;
                    last_space = i;
                }
            if (last_space > start) end = last_space;
            else {
                for (end = start + 1; end < n && text[end] != ' '; end++) {
                }
            }
        }
        lines[count++] = line_of(text, start, end, width, ctx);
        start = end;
        while (start < n && text[start] == ' ') start++;
    }
    return count;
}

static uint32_t over(uint32_t d, uint32_t s) {
    uint32_t a = s >> 24, inv = 255 - a, out = 0;
    if (!a) return d;
    if (a == 255) return s;
    for (int shift = 0; shift < 32; shift += 8) {
        uint32_t c = ((s >> shift) & 255) + (((d >> shift) & 255) * inv + 127) / 255;
        out |= (c > 255 ? 255 : c) << shift;
    }
    return out;
}

/* Chamfer distance (3-4) from the glyphs' cores: the outline is a ring of r pixels with a soft edge. */
void jvs_outline(const uint32_t* glyphs, int w, int h, int r, uint16_t* scratch, uint32_t* out) {
    uint16_t* d = scratch;
    const uint16_t unreached = 0x7FFF;
    for (int i = 0; i < w * h; i++) d[i] = (glyphs[i] & 0xFF) >= 128 ? 0 : unreached;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint16_t* p = d + y * w + x;
            if (x > 0 && p[-1] + 3 < *p) *p = (uint16_t)(p[-1] + 3);
            if (y > 0) {
                if (p[-w] + 3 < *p) *p = (uint16_t)(p[-w] + 3);
                if (x > 0 && p[-w - 1] + 4 < *p) *p = (uint16_t)(p[-w - 1] + 4);
                if (x + 1 < w && p[-w + 1] + 4 < *p) *p = (uint16_t)(p[-w + 1] + 4);
            }
        }
    for (int y = h - 1; y >= 0; y--)
        for (int x = w - 1; x >= 0; x--) {
            uint16_t* p = d + y * w + x;
            if (x + 1 < w && p[1] + 3 < *p) *p = (uint16_t)(p[1] + 3);
            if (y + 1 < h) {
                if (p[w] + 3 < *p) *p = (uint16_t)(p[w] + 3);
                if (x + 1 < w && p[w + 1] + 4 < *p) *p = (uint16_t)(p[w + 1] + 4);
                if (x > 0 && p[w - 1] + 4 < *p) *p = (uint16_t)(p[w - 1] + 4);
            }
        }
    for (int i = 0; i < w * h; i++) {
        int fill = (int)(glyphs[i] & 0xFF), ring = (r * 3 + 3 - d[i]) * 255 / 3, alpha, col;
        ring = ring < 0 ? 0 : ring > 255 ? 255 : ring;
        ring = ring * JVS_OUTLINE_OPACITY / 255;
        alpha = fill + ring * (255 - fill) / 255;
        col = fill;
        out[i] = (uint32_t)alpha << 24 | (uint32_t)col << 16 | (uint32_t)col << 8 | (uint32_t)col;
    }
}

void jvs_blend(uint32_t* target, long target_pitch_pixels, int target_w, int target_h, const uint32_t* image, int image_w,
               int image_h, int x, int y) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + image_w > target_w ? target_w : x + image_w, y1 = y + image_h > target_h ? target_h : y + image_h;
    if (!target || !image) return;
    for (int ty = y0; ty < y1; ty++) {
        uint32_t* d = target + (size_t)ty * (size_t)target_pitch_pixels;
        const uint32_t* s = image + (size_t)(ty - y) * (size_t)image_w - x;
        for (int tx = x0; tx < x1; tx++) d[tx] = over(d[tx], s[tx]);
    }
}
