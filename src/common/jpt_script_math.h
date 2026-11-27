// Vector3 and Rotate3 of CE's script API: the engine's quaternions (w scalar, xyz vector) rotating row vectors, so
// v * (a * b) == (v * a) * b.
#pragma once

#include <cmath>

namespace jpt::script {

struct V3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Q {
    float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;
};

inline bool Finite(const V3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline bool Finite(const Q& q) { return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z); }

inline V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 Mul(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline V3 Mul(V3 a, float f) { return {a.x * f, a.y * f, a.z * f}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float LengthSqr(V3 a) { return Dot(a, a); }
inline float Length(V3 a) { return std::sqrt(LengthSqr(a)); }

// A zero vector stays zero instead of becoming NaN.
inline V3 Normalised(V3 a) {
    const float length = Length(a);
    return length > 0.0f && std::isfinite(length) ? Mul(a, 1.0f / length) : a;
}

inline float LengthSqr(Q q) { return q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z; }
inline float Length(Q q) { return std::sqrt(LengthSqr(q)); }

// The Hamilton product: rotating by b, then by a.
inline Q Hamilton(Q a, Q b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

// A zero quaternion becomes the identity.
inline Q Normalised(Q q) {
    const float length = Length(q);
    if (!(length > 0.0f) || !std::isfinite(length)) return Q{};
    return {q.w / length, q.x / length, q.y / length, q.z / length};
}

inline Q Inverse(Q q) {
    const float n = LengthSqr(q);
    if (!(n > 0.0f)) return Q{};
    return {q.w / n, -q.x / n, -q.y / n, -q.z / n};
}

// Rotate3 * Rotate3: a first, then b.
inline Q Then(Q a, Q b) { return Hamilton(b, a); }

inline V3 Rotate(V3 v, Q q) {
    const Q r = Hamilton(Hamilton(q, Q{0.0f, v.x, v.y, v.z}), Q{q.w, -q.x, -q.y, -q.z});
    return {r.x, r.y, r.z};
}

// Rotate3 * float: the same axis, the angle scaled; 0 is the identity.
inline Q Scaled(Q q, float f) {
    const Q n = Normalised(q);
    const float s = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (!(s > 0.0f)) return Q{};
    const float half = std::atan2(s, n.w) * f;
    const float k = std::sin(half) / s;
    return {std::cos(half), n.x * k, n.y * k, n.z * k};
}

} // namespace jpt::script
