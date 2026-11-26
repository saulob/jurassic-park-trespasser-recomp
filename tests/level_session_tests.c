/* The [session], [level] and [restart] lines and the decisions behind them (jpt_level_session.c), headless. */
#include <stdio.h>
#include <string.h>

#include "jpt_level_session.h"

static unsigned long g_checks, g_failures;
#define CHECK(c)                                          \
    do {                                                  \
        g_checks++;                                       \
        if (!(c)) {                                       \
            g_failures++;                                 \
            printf("FAILED line %d: %s\n", __LINE__, #c); \
        }                                                 \
    } while (0)

static char g_lines[128][256];
static int g_count;

static void capture(const char* tag, const char* text) {
    if (g_count < 128) snprintf(g_lines[g_count++], sizeof g_lines[0], "[%s] %s", tag, text);
}

static int has(const char* text) {
    for (int i = 0; i < g_count; i++)
        if (!strcmp(g_lines[i], text)) return 1;
    return 0;
}

static int index_of(const char* text) {
    for (int i = 0; i < g_count; i++)
        if (!strcmp(g_lines[i], text)) return i;
    return -1;
}

static void custom_session(void) {
    jpt_session_info_t info;
    memset(&info, 0, sizeof info);
    info.kind = JPT_SESSION_CUSTOM;
    info.build = "en-116";
    info.renderer = "d3d11";
    info.language = "en";
    info.package = "Some_Package";
    info.layout = "loose";
    info.start_level = "Data/alpha.scn";
    info.capabilities = "texhw,sound_added";
    g_count = 0;
    jpt_session_begin(&info);
}

static void campaign_session(const char* renderer) {
    jpt_session_info_t info;
    memset(&info, 0, sizeof info);
    info.kind = JPT_SESSION_CAMPAIGN;
    info.build = "en-116";
    info.renderer = renderer;
    g_count = 0;
    jpt_session_begin(&info);
}

/* What the hooks do around one load of the game's level loader. */
static void load(const char* scene, jpt_level_source_t source, int save, int main_menu, int ok) {
    jpt_session_level_begin(jpt_session_reason(save, main_menu), scene, source);
    if (jpt_session_restart_purges()) jpt_session_world_purged();
    jpt_session_level_end(ok, ok ? 0 : -1);
}

static void restart(const char* scene, jpt_level_source_t source, int ok) {
    if (jpt_session_restart_begin(scene, source) == JPT_RESTART_FULL) load(scene, source, 0, 0, ok);
    else {
        jpt_session_level_begin(JPT_REASON_RESTART, scene, source);
        jpt_session_level_end(ok, ok ? 0 : -1);
    }
    jpt_session_restart_end(ok);
}

static void test_custom_session(void) {
    int lines;
    jpt_session_set_log(capture);
    custom_session();
    CHECK(has("[session] type=custom") && has("[session] package=Some_Package layout=loose") && has("[session] start_level=alpha.scn"));
    CHECK(has("[session] game_build=en-116") && has("[session] renderer=d3d11") && has("[session] language=en"));
    CHECK(has("[session] saves=custom package namespace") && has("[session] capabilities=texhw,sound_added"));
    CHECK(jpt_session_kind() == JPT_SESSION_CUSTOM && jpt_session_restart_mode() == JPT_RESTART_FULL);
    CHECK(!jpt_session_world_generation() && !jpt_session_levels());

    g_count = 0;
    load("C:\\Game\\data\\alpha.scn", JPT_SOURCE_PACKAGE, 0, 1, 1);
    CHECK(has("[level] begin reason=initial scene=alpha.scn source=package session=custom"));
    CHECK(has("[level] ready scene=alpha.scn source=package world_generation=1") && g_count == 2);

    /* Restart: full, the world one generation newer, the level the same. */
    g_count = 0;
    restart("alpha.scn", JPT_SOURCE_PACKAGE, 1);
    CHECK(has("[restart] requested scene=alpha.scn session=custom mode=full source=package"));
    CHECK(has("[level] begin reason=restart scene=alpha.scn source=package session=custom"));
    CHECK(has("[level] ready scene=alpha.scn source=package world_generation=2"));
    CHECK(has("[restart] result=ready old_world_generation=1 new_world_generation=2") && g_count == 4);
    CHECK(index_of("[restart] requested scene=alpha.scn session=custom mode=full source=package") <
          index_of("[restart] result=ready old_world_generation=1 new_world_generation=2"));
    CHECK(!jpt_session_restarting() && !jpt_session_restart_purges());

    /* Twice. */
    g_count = 0;
    restart("alpha.scn", JPT_SOURCE_PACKAGE, 1);
    CHECK(has("[restart] result=ready old_world_generation=2 new_world_generation=3"));

    /* custom -> custom, then Restart restarts beta, not alpha. */
    g_count = 0;
    load("C:\\Game\\data\\beta.scn", JPT_SOURCE_PACKAGE, 0, 0, 1);
    CHECK(has("[level] begin reason=transition scene=beta.scn source=package session=custom"));
    restart(jpt_session_current_scene(), jpt_session_current_source(), 1);
    CHECK(has("[restart] requested scene=beta.scn session=custom mode=full source=package"));

    /* custom -> retail stays a custom session, and its Restart is full too. */
    g_count = 0;
    load("C:\\Game\\data\\sum.scn", JPT_SOURCE_RETAIL, 0, 0, 1);
    CHECK(has("[level] begin reason=transition scene=sum.scn source=retail session=custom"));
    restart(jpt_session_current_scene(), jpt_session_current_source(), 1);
    CHECK(has("[restart] requested scene=sum.scn session=custom mode=full source=retail") && jpt_session_kind() == JPT_SESSION_CUSTOM);

    /* A save. */
    g_count = 0;
    load("beta.scn", JPT_SOURCE_PACKAGE, 1, 0, 1);
    CHECK(has("[level] begin reason=load_save scene=beta.scn source=package session=custom"));

    /* A full restart whose world was never purged or whose load failed is not called ready. */
    g_count = 0;
    jpt_session_restart_begin("beta.scn", JPT_SOURCE_PACKAGE);
    jpt_session_level_begin(jpt_session_reason(0, 0), "beta.scn", JPT_SOURCE_PACKAGE);
    jpt_session_level_end(1, 0);
    jpt_session_restart_end(1);
    CHECK(g_count == 4 && strstr(g_lines[3], "result=failed (the world was not rebuilt)"));
    g_count = 0;
    restart("beta.scn", JPT_SOURCE_PACKAGE, 0);
    CHECK(strstr(g_lines[2], "[level] failed scene=beta.scn") && strstr(g_lines[3], "[restart] result=failed"));

    /* Only lifecycle events log: the queries do not. */
    lines = g_count;
    for (int i = 0; i < 1000; i++) {
        (void)jpt_session_kind();
        (void)jpt_session_restart_mode();
        (void)jpt_session_current_scene();
        jpt_session_level_end(1, 0);
        jpt_session_restart_end(1);
    }
    CHECK(g_count == lines);

    g_count = 0;
    jpt_session_end();
    CHECK(g_count == 1 && strstr(g_lines[0], "[session] end type=custom"));
    jpt_session_end();
    CHECK(g_count == 1 && jpt_session_kind() == JPT_SESSION_NONE);
}

static void test_campaign_session(void) {
    jpt_session_set_log(capture);
    campaign_session("vulkan");
    CHECK(has("[session] type=campaign") && has("[session] game_build=en-116") && has("[session] renderer=vulkan") && has("[session] saves=game"));
    CHECK(!has("[session] capabilities=none") && g_count == 4);
    CHECK(jpt_session_restart_mode() == JPT_RESTART_SCENE_RELOAD);

    g_count = 0;
    load("C:\\Game\\data\\be.scn", JPT_SOURCE_RETAIL, 0, 1, 1);
    CHECK(has("[level] begin reason=new_game scene=be.scn source=retail session=campaign") && has("[level] ready scene=be.scn source=retail world_generation=1"));
    load("C:\\Game\\data\\jr.scn", JPT_SOURCE_RETAIL, 0, 0, 1);
    CHECK(has("[level] begin reason=transition scene=jr.scn source=retail session=campaign"));
    load("C:\\Saves\\SAVEGAME.003", JPT_SOURCE_RETAIL, 1, 0, 1);
    CHECK(has("[level] begin reason=load_save scene=SAVEGAME.003 source=retail session=campaign"));

    /* The campaign's Restart is the game's own: no purge asked for, the world kept. */
    g_count = 0;
    CHECK(jpt_session_restart_begin("jr.scn", JPT_SOURCE_RETAIL) == JPT_RESTART_SCENE_RELOAD && !jpt_session_restart_purges());
    jpt_session_level_begin(JPT_REASON_RESTART, "jr.scn", JPT_SOURCE_RETAIL);
    jpt_session_level_end(1, 0);
    jpt_session_restart_end(1);
    CHECK(has("[restart] requested scene=jr.scn session=campaign mode=scene_reload source=retail"));
    CHECK(has("[level] begin reason=restart scene=jr.scn source=retail session=campaign"));
    CHECK(has("[restart] result=ready old_world_generation=1 new_world_generation=1"));

    /* New Game from the main menu is a new game again, not a transition. */
    g_count = 0;
    load("be.scn", JPT_SOURCE_RETAIL, 0, 1, 1);
    CHECK(has("[level] begin reason=new_game scene=be.scn source=retail session=campaign"));
    jpt_session_end();
}

/* A custom session, then the campaign in the same process: nothing of the first one is kept. */
static void test_campaign_after_custom(void) {
    jpt_session_set_log(capture);
    custom_session();
    load("alpha.scn", JPT_SOURCE_PACKAGE, 0, 1, 1);
    restart("alpha.scn", JPT_SOURCE_PACKAGE, 1);
    jpt_session_end();
    campaign_session("d3d11");
    CHECK(jpt_session_kind() == JPT_SESSION_CAMPAIGN && !jpt_session_levels() && !jpt_session_world_generation() && !jpt_session_current_scene()[0]);
    CHECK(jpt_session_restart_mode() == JPT_RESTART_SCENE_RELOAD && !has("[session] type=custom"));
    g_count = 0;
    load("be.scn", JPT_SOURCE_RETAIL, 0, 1, 1);
    CHECK(has("[level] begin reason=new_game scene=be.scn source=retail session=campaign"));
    jpt_session_end();
}

static void test_names(void) {
    CHECK(!strcmp(jpt_session_scene_name("C:\\a\\b\\c.scn"), "c.scn") && !strcmp(jpt_session_scene_name("Mission/data/keep.scn"), "keep.scn"));
    CHECK(!strcmp(jpt_session_scene_name("plain.scn"), "plain.scn") && !strcmp(jpt_session_scene_name(""), ""));
    CHECK(!strcmp(jpt_session_reason_name(JPT_REASON_NEW_GAME), "new_game") && !strcmp(jpt_session_reason_name(JPT_REASON_INITIAL), "initial"));
    CHECK(!strcmp(jpt_session_reason_name(JPT_REASON_TRANSITION), "transition") && !strcmp(jpt_session_reason_name(JPT_REASON_RESTART), "restart"));
    CHECK(!strcmp(jpt_session_reason_name(JPT_REASON_LOAD_SAVE), "load_save"));
    /* No log sink: nothing breaks. */
    jpt_session_set_log(NULL);
    campaign_session("software");
    load("be.scn", JPT_SOURCE_RETAIL, 0, 1, 1);
    jpt_session_end();
    CHECK(g_count == 0);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_custom_session();
    test_campaign_session();
    test_campaign_after_custom();
    test_names();
    printf("level_session_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
