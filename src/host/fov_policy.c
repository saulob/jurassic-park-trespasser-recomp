#include "fov_policy.h"

#include <math.h>

#define PI 3.14159265358979323846

double fov_tan_half(double game_tan_half, int fov) {
    double half;
    if (fov == FOV_GAME_DEGREES || !(game_tan_half > 0.0) || !isfinite(game_tan_half) || fov <= 0) return game_tan_half;
    half = atan(game_tan_half) * fov / FOV_GAME_DEGREES;
    if (half > FOV_HALF_ANGLE_LIMIT * PI / 180.0) half = FOV_HALF_ANGLE_LIMIT * PI / 180.0;
    return tan(half);
}

double fov_hor_plus(double w, double h) {
    double k = w > 0.0 && h > 0.0 ? (w / h) / (4.0 / 3.0) : 1.0;
    return k > 1.0 && isfinite(k) ? k : 1.0;
}

double fov_horizontal_degrees(double tan_half_4x3, double w, double h) {
    return 2.0 * atan(tan_half_4x3 * fov_hor_plus(w, h)) * 180.0 / PI;
}

double fov_vertical_degrees(double tan_half_4x3) { return 2.0 * atan(tan_half_4x3 * 0.75) * 180.0 / PI; }
