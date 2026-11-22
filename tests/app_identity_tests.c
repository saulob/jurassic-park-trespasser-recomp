/* The application identifier SDL hands to the desktop, and the embedded window icon, against SDL's dummy video driver
 * (headless: no window is shown). */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "jpt_app_identity.h"
#include "jpt_port.h"

static int g_checks, g_failures;

#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                                \
        g_checks++;                                                                                                     \
        if (!(cond)) {                                                                                                  \
            g_failures++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                        \
            printf("\n");                                                                                               \
        }                                                                                                               \
    } while (0)

static void test_identifier(void) {
    jpt_app_identity_apply("9.9.9");
    CHECK(!strcmp(SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING), JPT_APPLICATION_ID),
          "SDL's identifier is the application ID");
    CHECK(!strcmp(SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING), JPT_APPLICATION_NAME), "and its name");
    CHECK(!strcmp(SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_VERSION_STRING), "9.9.9"), "and the version");
    CHECK(!strcmp(SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_TYPE_STRING), "game"), "of a game");
}

static void test_icon(void) {
    SDL_Surface* icon = jpt_app_icon_load();
    SDL_Window* window;
    CHECK(icon != NULL, "the embedded icon decodes (%s)", SDL_GetError());
    if (icon) {
        CHECK(icon->w == 256 && icon->h == 256, "256x256 (%dx%d)", icon->w, icon->h);
        CHECK(SDL_ISPIXELFORMAT_ALPHA(icon->format), "with an alpha channel");
        SDL_DestroySurface(icon);
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO), "SDL video on the dummy driver (%s)", SDL_GetError());
    window = SDL_CreateWindow("icon test", 64, 64, SDL_WINDOW_HIDDEN);
    CHECK(window != NULL, "a hidden window (%s)", SDL_GetError());
    CHECK(jpt_app_icon_apply(window), "SDL accepts the icon for a window (%s)", SDL_GetError());
    CHECK(!jpt_app_icon_apply(NULL), "no window, no icon");
    SDL_DestroyWindow(window);
    SDL_Quit();
}

int main(void) {
    test_identifier();
    test_icon();
    printf("app_identity_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
