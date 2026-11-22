/* What the desktop knows the application by: SDL turns the identifier into the Wayland app_id and the X11 window class, and the
 * icon is the one embedded in the executable (packaging/icons/app-icon-256.png), so nothing depends on the AppImage's desktop
 * integration. Needs SDL3; used by the Linux executable and the launcher. */
#ifndef JPT_APP_IDENTITY_H
#define JPT_APP_IDENTITY_H

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Surface;
struct SDL_Window;

/* Sets the application's name, version and identifier (JPT_APPLICATION_ID). Call before the video subsystem starts. */
void jpt_app_identity_apply(const char* version);

/* The embedded icon as an SDL surface (256x256 RGBA), or NULL. The caller destroys it. */
struct SDL_Surface* jpt_app_icon_load(void);

/* Gives the window the icon. Returns 1 when SDL accepted it. */
int jpt_app_icon_apply(struct SDL_Window* window);

#ifdef __cplusplus
}
#endif

#endif
