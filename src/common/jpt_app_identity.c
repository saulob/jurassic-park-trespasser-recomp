#include "jpt_app_identity.h"

#include <SDL3/SDL.h>

#include "jpt_port.h"

extern const unsigned char jpt_app_icon_png[];
extern const unsigned long jpt_app_icon_png_size;

void jpt_app_identity_apply(const char* version) {
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, JPT_APPLICATION_NAME);
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING, JPT_APPLICATION_ID);
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_TYPE_STRING, "game");
    if (version && *version) SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_VERSION_STRING, version);
}

SDL_Surface* jpt_app_icon_load(void) {
    SDL_IOStream* io = SDL_IOFromConstMem(jpt_app_icon_png, (size_t)jpt_app_icon_png_size);
    return io ? SDL_LoadPNG_IO(io, true) : NULL;
}

int jpt_app_icon_apply(SDL_Window* window) {
    SDL_Surface* icon = window ? jpt_app_icon_load() : NULL;
    int ok = icon && SDL_SetWindowIcon(window, icon);
    SDL_DestroySurface(icon);
    return ok;
}
