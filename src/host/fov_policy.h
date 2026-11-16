/* The main camera's field of view without the guest. The game keeps tan(half the horizontal angle) of a 4:3 view in its
 * camera properties (75 degrees from CCamera::SProperties, other angles from a level's CSetRendererAction). */
#ifndef FOV_POLICY_H
#define FOV_POLICY_H

#ifdef __cplusplus
extern "C" {
#endif

#define FOV_GAME_DEGREES 75
#define FOV_HALF_ANGLE_LIMIT 85.0

/* The game's angle scaled by fov / 75, as Trespasser CE scales it; 75 returns game_tan_half unchanged. */
double fov_tan_half(double game_tan_half, int fov);
/* How much wider than 4:3 a w x h picture is; 1 for 4:3, narrower or unknown shapes. */
double fov_hor_plus(double w, double h);
/* Full angles in degrees for a tan(half horizontal angle) of the 4:3 view shown at w x h. */
double fov_horizontal_degrees(double tan_half_4x3, double w, double h);
double fov_vertical_degrees(double tan_half_4x3);

#ifdef __cplusplus
}
#endif

#endif
