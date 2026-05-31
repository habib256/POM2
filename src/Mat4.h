// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Mat4 — minimal column-major 4×4 matrix + Vec3 math for the 3D voxel view.
// Header-only, no GL, no dependency (the repo has no glm). Column-major to
// match OpenGL's uniform layout, so the float[16] can be handed straight to
// glUniformMatrix4fv with transpose=GL_FALSE. Pure CPU → unit-testable
// (voxel3d_math_test); the GL renderer just consumes the resulting matrix.

#ifndef POM2_MAT4_H
#define POM2_MAT4_H

#include <cmath>

namespace pom2 {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b)
{
    return { a.y * b.z - a.z * b.y,
             a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x };
}
inline Vec3 normalize(Vec3 v)
{
    const float len = std::sqrt(dot(v, v));
    return len > 1e-8f ? v * (1.0f / len) : Vec3{ 0, 0, 0 };
}

// Column-major: m[col*4 + row]. Identity by default.
struct Mat4 {
    float m[16] = { 1, 0, 0, 0,
                    0, 1, 0, 0,
                    0, 0, 1, 0,
                    0, 0, 0, 1 };

    static Mat4 identity() { return Mat4{}; }

    // Right-handed perspective, GL clip space z ∈ [-1, 1]. `fovY` in radians.
    static Mat4 perspective(float fovY, float aspect, float znear, float zfar)
    {
        const float f = 1.0f / std::tan(fovY * 0.5f);
        Mat4 r;
        for (float& v : r.m) v = 0.0f;
        r.m[0]  = f / aspect;
        r.m[5]  = f;
        r.m[10] = (zfar + znear) / (znear - zfar);
        r.m[11] = -1.0f;
        r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
        return r;
    }

    // Right-handed look-at (gluLookAt). Maps `eye` → origin, `center` onto -Z.
    static Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up)
    {
        const Vec3 f = normalize(center - eye);
        const Vec3 s = normalize(cross(f, up));
        const Vec3 u = cross(s, f);
        Mat4 r;
        r.m[0] = s.x; r.m[4] = s.y; r.m[8]  = s.z;
        r.m[1] = u.x; r.m[5] = u.y; r.m[9]  = u.z;
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
        r.m[3] = 0; r.m[7] = 0; r.m[11] = 0;
        r.m[12] = -dot(s, eye);
        r.m[13] = -dot(u, eye);
        r.m[14] = dot(f, eye);
        r.m[15] = 1.0f;
        return r;
    }

    // this * b  (apply b first, then this — standard GL composition).
    Mat4 operator*(const Mat4& b) const
    {
        Mat4 r;
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                    sum += m[k * 4 + row] * b.m[col * 4 + k];
                r.m[col * 4 + row] = sum;
            }
        return r;
    }

    // Transform a point (w=1), perspective-divided. For tests / picking.
    Vec3 transformPoint(Vec3 p) const
    {
        const float x = m[0] * p.x + m[4] * p.y + m[8]  * p.z + m[12];
        const float y = m[1] * p.x + m[5] * p.y + m[9]  * p.z + m[13];
        const float z = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
        const float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
        const float inv = (w != 0.0f) ? 1.0f / w : 1.0f;
        return { x * inv, y * inv, z * inv };
    }
};

// Orbit camera: spherical position around `target`, producing a view-proj
// matrix. Azimuth/elevation in radians, distance/fov in world units. This is
// the camera state the UI mutates (mouse drag → azimuth/elevation, scroll →
// distance) in later phases; here it's already testable.
struct OrbitCamera {
    // Defaults frame the upright 4:3 "monitor" nearly head-on with a gentle
    // 3/4 turn, so the uniform voxel depth reads as relief without the screen
    // tipping onto its back (the old floor-map angle). Drag/scroll mutate these.
    float azimuth   = 0.32f;         // around +Y — slight turn to the right
    float elevation = 0.20f;         // up from the horizon — slight look-down
    float distance  = 2.8f;
    float fovY      = 0.7f;          // ~40°, less perspective stretch
    Vec3  target    = { 0.0f, 0.0f, 0.0f };
    float znear     = 0.05f;
    float zfar      = 100.0f;

    Vec3 eye() const
    {
        const float ce = std::cos(elevation), se = std::sin(elevation);
        const float ca = std::cos(azimuth),   sa = std::sin(azimuth);
        return { target.x + distance * ce * sa,
                 target.y + distance * se,
                 target.z + distance * ce * ca };
    }

    // Camera basis in world space at the current orientation (matches the
    // lookAt() basis with world-up {0,1,0}). Used to strafe/pan the target.
    Vec3 forward() const { return normalize(target - eye()); }
    Vec3 right()   const { return normalize(cross(forward(), Vec3{ 0, 1, 0 })); }
    Vec3 up()      const { return cross(right(), forward()); }

    // Pan (strafe): slide the orbit target — and therefore eye() — across the
    // camera's right/up plane. `rx`/`uy` are world distances.
    void pan(float rx, float uy) { target = target + right() * rx + up() * uy; }

    Mat4 viewProj(float aspect) const
    {
        const Mat4 view = Mat4::lookAt(eye(), target, { 0, 1, 0 });
        const Mat4 proj = Mat4::perspective(fovY, aspect, znear, zfar);
        return proj * view;
    }
};

}  // namespace pom2

#endif  // POM2_MAT4_H
