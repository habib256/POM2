// Mat4 / OrbitCamera math test (3D voxel view, Phase 0).
//
// The 3D voxel renderer is GPU code (not unit-testable here), but its CPU-side
// camera math is. This pins the column-major Mat4 primitives and the orbit
// camera so a regression in the matrix layout (the classic source of a
// black/garbled 3D view) is caught off-GPU.

#include "Mat4.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using pom2::Mat4;
using pom2::Vec3;
using pom2::OrbitCamera;

namespace {

bool approx(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

bool approxV(Vec3 a, Vec3 b, float eps = 1e-4f)
{
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) && approx(a.z, b.z, eps);
}

}  // namespace

int main()
{
    // ── identity & multiply ───────────────────────────────────────────────
    {
        const Mat4 I = Mat4::identity();
        assert(approxV(I.transformPoint({ 3, -2, 5 }), { 3, -2, 5 }));

        // A non-trivial matrix times identity is itself.
        Mat4 P = Mat4::perspective(0.9f, 1.5f, 0.1f, 50.0f);
        Mat4 PI = P * I;
        for (int i = 0; i < 16; ++i) assert(approx(P.m[i], PI.m[i]));
        Mat4 IP = I * P;
        for (int i = 0; i < 16; ++i) assert(approx(P.m[i], IP.m[i]));
    }

    // ── perspective: known entries ────────────────────────────────────────
    {
        const float fovY = 1.0f, aspect = 16.0f / 9.0f, zn = 0.5f, zf = 100.0f;
        const Mat4 P = Mat4::perspective(fovY, aspect, zn, zf);
        const float f = 1.0f / std::tan(fovY * 0.5f);
        assert(approx(P.m[0], f / aspect));
        assert(approx(P.m[5], f));
        assert(approx(P.m[10], (zf + zn) / (zn - zf)));
        assert(approx(P.m[11], -1.0f));
        assert(approx(P.m[14], (2.0f * zf * zn) / (zn - zf)));
        // The perspective row-3 carries -1 so w = -z_eye after transform.
        assert(approx(P.m[3], 0.0f) && approx(P.m[15], 0.0f));
    }

    // ── lookAt: maps eye→origin, looks down -Z ────────────────────────────
    {
        const Mat4 V = Mat4::lookAt({ 0, 0, 5 }, { 0, 0, 0 }, { 0, 1, 0 });
        // Eye goes to the view-space origin.
        assert(approxV(V.transformPoint({ 0, 0, 5 }), { 0, 0, 0 }));
        // The look target sits straight ahead on -Z at the eye distance.
        assert(approxV(V.transformPoint({ 0, 0, 0 }), { 0, 0, -5 }));
        // A point to the world +X is on the camera's +X (right) at that depth.
        const Vec3 r = V.transformPoint({ 1, 0, 0 });
        assert(r.x > 0.9f && approx(r.y, 0.0f) && approx(r.z, -5.0f));

        // Basis rows orthonormal: rows 0,1,2 are s,u,-f.
        const Vec3 s{ V.m[0], V.m[4], V.m[8] };
        const Vec3 u{ V.m[1], V.m[5], V.m[9] };
        const Vec3 nf{ V.m[2], V.m[6], V.m[10] };
        assert(approx(dot(s, s), 1.0f) && approx(dot(u, u), 1.0f) && approx(dot(nf, nf), 1.0f));
        assert(approx(dot(s, u), 0.0f) && approx(dot(s, nf), 0.0f) && approx(dot(u, nf), 0.0f));
    }

    // ── orbit camera: target projects near screen centre, in front ────────
    {
        OrbitCamera cam;
        cam.azimuth = 0.7f; cam.elevation = 0.4f; cam.distance = 3.0f;
        const Mat4 vp = cam.viewProj(1.4f);
        const Vec3 ndc = vp.transformPoint(cam.target);
        assert(std::fabs(ndc.x) < 0.05f && std::fabs(ndc.y) < 0.05f);  // centred
        assert(ndc.z > -1.0f && ndc.z < 1.0f);                          // within clip

        // The eye is `distance` from the target.
        const Vec3 d = cam.eye() - cam.target;
        assert(approx(std::sqrt(dot(d, d)), cam.distance, 1e-3f));

        // Rotating azimuth keeps the target centred (camera still aims at it).
        cam.azimuth = 2.3f;
        const Vec3 ndc2 = cam.viewProj(1.4f).transformPoint(cam.target);
        assert(std::fabs(ndc2.x) < 0.05f && std::fabs(ndc2.y) < 0.05f);
    }

    std::printf("Voxel3D math: OK (mat4 perspective/lookAt/multiply + orbit camera)\n");
    return 0;
}
