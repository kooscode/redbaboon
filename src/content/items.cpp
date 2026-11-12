// Small meshes: the banana (bullet), the banana rocket, the supply crate, the stunt ring and the sky dome.
// The first four use the props texture (props_layout.h). docs/design.md §7; creative brief §1.
#include "content/content.h"
#include "content/mesh_builder.h"
#include "content/props_layout.h"

#include <math.h>

namespace content {

constexpr uint32_t WHITE = 0xFFFFFFFFu;

// --- Bananas

// A curved banana along Z (stem at +Z, tip at -Z), bulging down; length and thickness in metres.
static void AddBananaBody(MeshData& mesh, float length, float radius, int segments, int sides)
{
    AddSweep(
        mesh, segments, sides,
        [&](float t) {
            float k = 2.0f * t - 1.0f;
            return Vec3(0.0f, -0.2f * length * (1.0f - k * k), -k * length * 0.5f);
        },
        [&](float t) {
            return radius * FastPow(Max(FastSin(PI * (0.06f + 0.88f * t)), 0.0f), 0.6f) + radius * 0.08f;
        },
        PropRect(PX_BANANA_SKIN), WHITE);
}

MeshData MakeBanana()
{
    MeshData mesh;
    AddBananaBody(mesh, 0.6f, 0.07f, 8, 6);
    AddTube(mesh, Vec3(0, -0.01f, 0.29f), Vec3(0, 0.02f, 0.36f), 0.025f, 0.02f, 4, PropSwatchUv(PS_DARK_WOOD), WHITE,
            true);
    return mesh;
}

MeshData MakeBananaRocket()
{
    MeshData mesh;
    AddBananaBody(mesh, 1.25f, 0.15f, 10, 8);
    // Brass band, three crimson fins and a dark nozzle at the stem end.
    AddTube(mesh, Vec3(0, -0.08f, 0.3f), Vec3(0, -0.06f, 0.38f), 0.16f, 0.15f, 8, PropSwatchUv(PS_LAMP), 0xFFC9A227u,
            false);
    for (int i = 0; i < 3; i++) {
        MeshData fin;
        std::vector<Vec2> poly = {{0.0f, 0.0f}, {0.3f, 0.0f}, {0.42f, 0.28f}, {0.12f, 0.2f}};
        AddPrism(fin, poly, 0.03f, PropSwatchUv(PS_RED), WHITE);
        float a = i * 2.0f * PI / 3.0f;
        Vec3 radial(FastSin(a), FastCos(a), 0.0f);
        Append(mesh, fin, Basis(Vec3(0, 0, 1), radial, Vec3(0, 0.05f, 0.42f) + radial * 0.1f));
    }
    AddTube(mesh, Vec3(0, 0.05f, 0.62f), Vec3(0, 0.05f, 0.82f), 0.07f, 0.12f, 8, PropSwatchUv(PS_INK), WHITE, true);
    return mesh;
}

// --- Crate and ring

MeshData MakeCrate()
{
    MeshData mesh;
    AddBox(mesh, Vec3(), Vec3(1.0f, 1.0f, 1.0f), PropRect(PX_CRATE), WHITE);
    // Rope straps around it.
    for (int i = 0; i < 2; i++) {
        AddBox(mesh, Vec3(0, 0, -0.5f + i), Vec3(1.03f, 1.03f, 0.06f), PropSwatchUv(PS_ROPE), WHITE);
    }
    return mesh;
}

MeshData MakeRing()
{
    MeshData mesh;
    constexpr int BANDS = 12;
    constexpr float RING_R = 15.0f;
    constexpr float TUBE_R = 1.0f;
    static const uint32_t COLORS[3] = {0xFFC1121Fu, 0xFFFFD23Fu, 0xFFF2E6C9u};
    UvRect white = PropSwatchUv(PS_WHITE);
    for (int b = 0; b < BANDS; b++) {
        float a0 = static_cast<float>(b) / BANDS * 2.0f * PI;
        float a1 = static_cast<float>(b + 1) / BANDS * 2.0f * PI;
        uint32_t color = COLORS[b % 3];
        AddSurface(
            mesh, 4, 8,
            [&](float s, float t) {
                float a = Lerp(a0, a1, s);
                float c = t * 2.0f * PI;
                float d = RING_R + TUBE_R * FastCos(c);
                return Vec3(d * FastCos(a), d * FastSin(a), TUBE_R * FastSin(c));
            },
            [&](float, float) {
                return Vec2{white.u0, white.v0};
            },
            [&](float, float) {
                return color;
            },
            SurfaceOptions{false, false});
    }
    return mesh;
}

// --- Sky

MeshData MakeSkyDome()
{
    MeshData mesh;
    Vec3 zenith(0.165f, 0.384f, 0.722f);
    Vec3 mid(0.369f, 0.576f, 0.839f);
    Vec3 horizon(0.788f, 0.867f, 0.922f);
    Vec3 sunHorizon(0.953f, 0.863f, 0.682f);
    Vec3 sunDir = Normalize(Vec3(-0.643f, 0.0f, 0.766f));
    AddSurface(
        mesh, 32, 10,
        [](float s, float t) {
            float a = s * 2.0f * PI;
            float e = Lerp(PI * 0.5f, -0.15f, t);
            return Vec3(FastCos(e) * FastSin(a), FastSin(e), FastCos(e) * FastCos(a));
        },
        [](float s, float t) {
            return Vec2{s, t};
        },
        [&](float s, float t) {
            float a = s * 2.0f * PI;
            float e = Lerp(PI * 0.5f, -0.15f, t);
            float up = Max(FastSin(e), 0.0f);
            float toward = Max(FastSin(a) * sunDir.x + FastCos(a) * sunDir.z, 0.0f);
            Vec3 h = Lerp(horizon, sunHorizon, toward * toward);
            Vec3 c = up < 0.35f ? Lerp(h, mid, up / 0.35f) : Lerp(mid, zenith, (up - 0.35f) / 0.65f);
            return RgbF(c);
        },
        SurfaceOptions{false, true});
    return mesh;
}

} // namespace content
