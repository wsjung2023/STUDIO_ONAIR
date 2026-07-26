#pragma once

#include <array>
#include <cmath>

namespace creator::avatar::vrm {

struct Vec2 final {
    float x{0}, y{0};
};

struct Vec3 final {
    float x{0}, y{0}, z{0};
    friend Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
};

struct Vec4 final {
    float x{0}, y{0}, z{0}, w{0};
};

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }

inline Vec3 normalize(Vec3 a) {
    const float l = length(a);
    return l > 1.0e-8F ? a * (1.0F / l) : Vec3{0, 0, 0};
}

/// A quaternion (x, y, z, w) — glTF node rotation and VRM bone rotation format.
struct Quat final {
    float x{0}, y{0}, z{0}, w{1};
    friend Quat operator*(Quat a, Quat b) {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }
};

inline Quat quatFromAxisAngle(Vec3 axis, float radians) {
    const Vec3 n = normalize(axis);
    const float s = std::sin(radians * 0.5F);
    return {n.x * s, n.y * s, n.z * s, std::cos(radians * 0.5F)};
}

/// A column-major 4x4 matrix (glTF convention), stored as m[col*4 + row].
struct Mat4 final {
    std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static Mat4 identity() { return {}; }

    static Mat4 translation(Vec3 t) {
        Mat4 r;
        r.m[12] = t.x;
        r.m[13] = t.y;
        r.m[14] = t.z;
        return r;
    }

    static Mat4 scale(Vec3 s) {
        Mat4 r;
        r.m[0] = s.x;
        r.m[5] = s.y;
        r.m[10] = s.z;
        return r;
    }

    static Mat4 rotation(Quat q) {
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        Mat4 r;
        r.m[0] = 1 - 2 * (yy + zz);
        r.m[1] = 2 * (xy + wz);
        r.m[2] = 2 * (xz - wy);
        r.m[4] = 2 * (xy - wz);
        r.m[5] = 1 - 2 * (xx + zz);
        r.m[6] = 2 * (yz + wx);
        r.m[8] = 2 * (xz + wy);
        r.m[9] = 2 * (yz - wx);
        r.m[10] = 1 - 2 * (xx + yy);
        return r;
    }

    friend Mat4 operator*(const Mat4& a, const Mat4& b) {
        Mat4 r;
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0;
                for (int k = 0; k < 4; ++k)
                    sum += a.m[static_cast<std::size_t>(k) * 4 + row] *
                           b.m[static_cast<std::size_t>(col) * 4 + k];
                r.m[static_cast<std::size_t>(col) * 4 + row] = sum;
            }
        }
        return r;
    }

    /// Transforms a point (w = 1), returning the xyz of the result.
    [[nodiscard]] Vec3 transformPoint(Vec3 p) const {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
    }

    /// Transforms a direction (w = 0), ignoring translation.
    [[nodiscard]] Vec3 transformDir(Vec3 d) const {
        return {m[0] * d.x + m[4] * d.y + m[8] * d.z,
                m[1] * d.x + m[5] * d.y + m[9] * d.z,
                m[2] * d.x + m[6] * d.y + m[10] * d.z};
    }
};

/// Composes a node's local transform from translation, rotation, scale.
inline Mat4 trs(Vec3 t, Quat r, Vec3 s) {
    return Mat4::translation(t) * Mat4::rotation(r) * Mat4::scale(s);
}

}  // namespace creator::avatar::vrm
