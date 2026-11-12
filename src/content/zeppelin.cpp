// The zeppelin "Silverback": hull skin in burnable sections, girder frame, cruciform fins, gondolas, turret
// sponsons, engine pods on struts, turrets, and its texture (alternating fabric panels, girder shadows, a black and
// crimson band, the gorilla-skull emblem, gondola windows). docs/design.md §2 (boss), §7; creative brief §1, §8.
#include "content/content.h"
#include "content/emblems.h"
#include "content/mesh_builder.h"
#include "content/noise.h"
#include "content/paint.h"

#include <math.h>

namespace content {

// Hull: length along Z (nose at -LENGTH/2), maximum radius.
constexpr float LENGTH = 180.0f;
constexpr float RADIUS = 21.0f;
// Hull skin tessellation: segments around, rows along (split evenly into the sections).
constexpr int HULL_AROUND = 24;
// The hull skin burns away in 12 sections: 6 along the length, each split into its right and left half.
constexpr int HULL_LENGTHS = 6;
constexpr int HULL_SECTIONS = HULL_LENGTHS * 2;
constexpr int ROWS_PER_SECTION = 8;
// Texture: the hull uses rows 0..HULL_TEX_ROWS-1, the gondola strip and swatches the rows below.
constexpr int ZEP_TEX = 256;
constexpr int HULL_TEX_ROWS = 160;
// The fins' own panel painting (panels, the black and crimson band, a skull roundel).
constexpr AtlasRect ZT_FIN = {0, 160, 128, 64};
constexpr AtlasRect ZT_GONDOLA = {0, 224, 128, 32};
constexpr int ZT_SWATCH_X = 128;
constexpr int ZT_SWATCH_Y = 224;

enum ZeppelinSwatch {
    ZS_WHITE,
    ZS_DARK_METAL,
    ZS_GIRDER,
    ZS_INK,
    ZS_CRIMSON,
    ZS_HULL,
    ZS_BRASS,
    ZS_GLASS,
    ZS_COUNT,
};

constexpr uint32_t WHITE = 0xFFFFFFFFu;
constexpr uint32_t HULL_GREY = 0xFFC2C8CEu;
constexpr uint32_t CRIMSON = 0xFFC1121Fu;
constexpr uint32_t DARK = 0xFF3A3A3Eu;
constexpr uint32_t GIRDER = 0xFF4A4038u;

// Engine pods: positions (hull-local).
static const Vec3 ENGINE_POS[4] = {Vec3(31.0f, -10.0f, -8.0f), Vec3(-31.0f, -10.0f, -8.0f),
                                   Vec3(29.0f, -9.0f, 40.0f), Vec3(-29.0f, -9.0f, 40.0f)};
// Engine pods are modelled at 1 m units and scaled up to read as targets.
constexpr float ENGINE_SCALE = 1.6f;
// Turrets likewise.
constexpr float TURRET_SCALE = 1.4f;
// Turrets: on sponsons low on the flanks, upright (they yaw around +Y).
static const Vec3 TURRET_POS[4] = {Vec3(22.6f, -7.0f, -34.0f), Vec3(-22.6f, -7.0f, -34.0f),
                                   Vec3(22.2f, -7.0f, 18.0f), Vec3(-22.2f, -7.0f, 18.0f)};

// --- Shape

// Hull radius at z: elliptical nose, parallel middle, long tapering tail.
static float HullRadius(float z)
{
    float u = Clamp((z + LENGTH * 0.5f) / LENGTH, 0.0f, 1.0f);
    if (u < 0.26f) {
        float k = (0.26f - u) / 0.26f;
        return RADIUS * Sqrt(Max(1.0f - k * k, 0.0f)) * (1.0f - 0.04f * k);
    }
    if (u < 0.55f)
        return RADIUS;
    float k = (u - 0.55f) / 0.45f;
    return RADIUS * (1.0f - 0.9f * FastPow(k, 1.9f));
}

static Vec3 HullPoint(float u, float theta)
{
    float z = -LENGTH * 0.5f + u * LENGTH;
    float r = HullRadius(z);
    return Vec3(r * FastSin(theta), r * FastCos(theta), z);
}

static UvRect Swatch(int s)
{
    return AtlasSwatch(ZT_SWATCH_X + (s % 8) * 16 + 4, ZT_SWATCH_Y + (s / 8) * 16 + 4, 8, 8, ZEP_TEX);
}

// --- Hull skin

int ZeppelinHullSectionCount()
{
    return HULL_SECTIONS;
}

MeshData MakeZeppelinHullSection(int index)
{
    MeshData mesh;
    int along = index / 2;
    float s0 = (index % 2) * 0.5f;
    float u0 = static_cast<float>(along) / HULL_LENGTHS;
    float u1 = static_cast<float>(along + 1) / HULL_LENGTHS;
    float vScale = static_cast<float>(HULL_TEX_ROWS) / ZEP_TEX;
    AddSurface(
        mesh, HULL_AROUND / 2, ROWS_PER_SECTION,
        [&](float s, float t) {
            return HullPoint(Lerp(u0, u1, t), (s0 + s * 0.5f) * 2.0f * PI);
        },
        [&](float s, float t) {
            float a = s0 + s * 0.5f;
            float half = a <= 0.5f ? a * 2.0f : (1.0f - a) * 2.0f;
            return Vec2{Lerp(u0, u1, t), half * vScale};
        },
        [](float, float) {
            return WHITE;
        },
        SurfaceOptions{true, false});
    return mesh;
}

Vec3 ZeppelinHullSectionCenter(int index)
{
    float u = (index / 2 + 0.5f) / HULL_LENGTHS;
    float z = -LENGTH * 0.5f + u * LENGTH;
    float side = index % 2 == 0 ? 1.0f : -1.0f;
    return Vec3(side * HullRadius(z) * 0.64f, 0.0f, z);
}

// --- Girder frame

MeshData MakeZeppelinFrame()
{
    // Charred girders 1.2 m thick (readable at 300 m), about a third of them still glowing near the flames.
    MeshData mesh;
    UvRect white = Swatch(ZS_WHITE);
    constexpr uint32_t CHARRED = 0xFF2A2420u;
    constexpr uint32_t GLOWING = 0xFFC04010u;
    Rng rng(0x5EEDu);
    constexpr int RINGS = 11;
    for (int i = 1; i <= RINGS; i++) {
        float u = static_cast<float>(i) / (RINGS + 1);
        AddSweep(
            mesh, 20, 4,
            [&](float t) {
                return HullPoint(u, t * 2.0f * PI) * 0.97f + Vec3(0, 0, HullPoint(u, 0).z * 0.03f);
            },
            [](float) {
                return 0.6f;
            },
            white, rng.Uniform() < 0.3f ? GLOWING : CHARRED);
    }
    constexpr int STRINGERS = 16;
    for (int i = 0; i < STRINGERS; i++) {
        float theta = (i + 0.5f) * 2.0f * PI / STRINGERS;
        AddSweep(
            mesh, 20, 4,
            [&](float t) {
                Vec3 p = HullPoint(Lerp(0.02f, 0.97f, t), theta);
                return Vec3(p.x * 0.96f, p.y * 0.96f, p.z);
            },
            [](float) {
                return 0.5f;
            },
            white, rng.Uniform() < 0.3f ? GLOWING : CHARRED);
    }
    // The keel: a triangular walkway girder along the bottom.
    AddSweep(
        mesh, 12, 3,
        [&](float t) {
            Vec3 p = HullPoint(Lerp(0.08f, 0.9f, t), PI);
            return Vec3(0.0f, p.y * 0.9f, p.z);
        },
        [](float) {
            return 1.4f;
        },
        white, CHARRED);
    return mesh;
}

// --- Fins, gondolas, sponsons, struts

// A flat plate in local (x along the hull, y outward) extruded to thickness, placed with its outward axis along
// dir (perpendicular to Z).
static void AddFinPlate(MeshData& mesh, const std::vector<Vec2>& poly, float thickness, const Vec3& dir,
                        uint32_t color, const UvRect& uv)
{
    MeshData plate;
    AddPrism(plate, poly, thickness, uv, color);
    Append(mesh, plate, Basis(Vec3(0, 0, 1), Normalize(dir), Vec3()));
}

static void AddFins(MeshData& mesh)
{
    float z0 = 48.0f;
    float z1 = 84.0f;
    float r0 = HullRadius(z0) - 1.0f;
    float r1 = HullRadius(z1) - 0.5f;
    float span = 17.0f;
    for (int i = 0; i < 4; i++) {
        float a = i * PI * 0.5f;
        Vec3 dir(FastSin(a), FastCos(a), 0.0f);
        // Main fin: swept leading edge, straight trailing edge; the crimson rudder behind it.
        std::vector<Vec2> fin = {{z0, r0}, {z1 - 8.0f, r1}, {z1 - 8.0f, r1 + span * 0.9f}, {z0 + 16.0f, r0 + span}};
        AddFinPlate(mesh, fin, 1.2f, dir, WHITE, AtlasUv(ZT_FIN.x, ZT_FIN.y, ZT_FIN.w, ZT_FIN.h, ZEP_TEX));
        std::vector<Vec2> rudder = {{z1 - 8.0f, r1}, {z1, r1 + 0.5f}, {z1 + 1.0f, r1 + span * 0.85f},
                                    {z1 - 8.0f, r1 + span * 0.9f}};
        AddFinPlate(mesh, rudder, 0.9f, dir, CRIMSON, Swatch(ZS_WHITE));
        std::vector<Vec2> tip = {{z0 + 16.0f, r0 + span - 0.2f}, {z1 - 8.0f, r1 + span * 0.9f - 0.2f},
                                 {z1 - 8.0f, r1 + span * 0.9f + 1.4f}, {z0 + 17.0f, r0 + span + 1.4f}};
        AddFinPlate(mesh, tip, 1.3f, dir, 0xFF1A1A1Au, Swatch(ZS_WHITE));
    }
}

// A streamlined gondola: superellipse sections lofted along Z, texture strip with windows.
static void AddGondola(MeshData& mesh, float zFront, float zBack, float halfWidth, float height, float topY)
{
    UvRect r = AtlasUv(ZT_GONDOLA.x, ZT_GONDOLA.y, ZT_GONDOLA.w, ZT_GONDOLA.h, ZEP_TEX);
    AddSurface(
        mesh, 16, 10,
        [&](float s, float t) {
            float k = t * 2.0f - 1.0f;
            float shape = Sqrt(Max(1.0f - FastPow(Abs(k), 3.0f), 0.0f));
            float theta = s * 2.0f * PI;
            float c = FastCos(theta);
            float sn = FastSin(theta);
            float ex = 0.6f;
            float x = halfWidth * shape * (sn < 0.0f ? -FastPow(-sn, ex) : FastPow(sn, ex));
            float y = topY - height * 0.5f + height * 0.5f * shape * (c < 0.0f ? -FastPow(-c, ex) : FastPow(c, ex));
            return Vec3(x, y + height * 0.5f * (1.0f - shape), Lerp(zFront, zBack, t));
        },
        [&](float s, float t) {
            return Vec2{r.u0 + t * (r.u1 - r.u0), r.v0 + s * (r.v1 - r.v0)};
        },
        [](float, float) {
            return WHITE;
        },
        SurfaceOptions{true, true});
}

static void AddBody(MeshData& mesh)
{
    AddFins(mesh);
    float bottom = -RADIUS;
    AddGondola(mesh, -50.0f, -12.0f, 4.5f, 6.5f, bottom + 1.5f);
    AddGondola(mesh, 22.0f, 40.0f, 3.0f, 4.0f, -HullRadius(31.0f) + 1.2f);
    // The keel catwalk between the two gondolas, with a lit lamp every 6 m.
    AddBox(mesh, Vec3(0, bottom + 0.2f, 5.0f), Vec3(1.3f, 0.35f, 17.5f), Swatch(ZS_WHITE), DARK);
    for (int i = 0; i < 6; i++)
        AddBox(mesh, Vec3(0, bottom - 0.25f, -10.0f + i * 6.0f), Vec3(0.35f, 0.2f, 0.35f), Swatch(ZS_WHITE),
               0xFFFFD98Au);
    // Struts from the gondola roof to the hull.
    for (int i = 0; i < 4; i++) {
        float z = -45.0f + i * 10.0f;
        AddTube(mesh, Vec3(-3.0f, bottom + 1.0f, z), Vec3(-2.0f, bottom + 3.0f, z), 0.3f, 0.3f, 4, Swatch(ZS_WHITE),
                DARK, false);
        AddTube(mesh, Vec3(3.0f, bottom + 1.0f, z), Vec3(2.0f, bottom + 3.0f, z), 0.3f, 0.3f, 4, Swatch(ZS_WHITE),
                DARK, false);
    }
    // Turret sponsons: a short drum on a bracket.
    for (const Vec3& t : TURRET_POS) {
        float side = Sign(t.x);
        AddTube(mesh, t - Vec3(0, 0.9f, 0), t, 3.3f, 3.1f, 12, Swatch(ZS_WHITE), DARK, true);
        AddTube(mesh, t + Vec3(-side * 1.0f, -0.9f, 0), t + Vec3(-side * 3.5f, 1.5f, 0), 0.9f, 0.9f, 6,
                Swatch(ZS_WHITE), DARK, false);
    }
    // Engine struts: two per pod, to the hull.
    for (const Vec3& e : ENGINE_POS) {
        float side = Sign(e.x);
        float r = HullRadius(e.z);
        Vec3 hullPoint = Normalize(Vec3(e.x, e.y + 4.0f, 0.0f)) * (r - 1.0f) + Vec3(0, 0, e.z);
        AddTube(mesh, e + Vec3(-side * 2.0f, 0.8f, -2.4f), hullPoint + Vec3(0, 0, -4.0f), 0.6f, 0.6f, 5,
                Swatch(ZS_WHITE), GIRDER, false);
        AddTube(mesh, e + Vec3(-side * 2.0f, 0.8f, 2.4f), hullPoint + Vec3(0, 0, 4.0f), 0.6f, 0.6f, 5,
                Swatch(ZS_WHITE), GIRDER, false);
        AddTube(mesh, e + Vec3(-side * 1.4f, -1.6f, 0.0f), Vec3(e.x * 0.55f, e.y - 8.0f, e.z), 0.45f, 0.45f, 4,
                Swatch(ZS_WHITE), GIRDER, false);
    }
}

MeshData MakeZeppelinBody()
{
    MeshData mesh;
    AddBody(mesh);
    return mesh;
}

MeshData MakeZeppelin()
{
    MeshData mesh;
    for (int i = 0; i < HULL_SECTIONS; i++)
        Append(mesh, MakeZeppelinHullSection(i));
    AddBody(mesh);
    return mesh;
}

// --- Engines and turrets

int ZeppelinEngineCount()
{
    return 4;
}

Vec3 ZeppelinEngine(int index)
{
    return ENGINE_POS[index];
}

MeshData MakeZeppelinEngine()
{
    MeshData mesh;
    std::vector<Vec2> profile;
    for (int i = 0; i <= 10; i++) {
        float k = static_cast<float>(i) / 10.0f;
        float z = Lerp(-4.5f, 4.0f, k);
        float r = 1.9f * Sqrt(Max(FastSin(Clamp(k * 1.1f + 0.04f, 0.0f, 1.0f) * PI), 0.0f));
        if (k > 0.85f)
            r = Max(r, 0.5f);
        profile.push_back(Vec2{r, -z});
    }
    MeshData nacelle;
    AddLathe(nacelle, profile, 14, Swatch(ZS_WHITE), HULL_GREY, false);
    Append(mesh, nacelle, Place(Vec3(), 0.0f, -PI * 0.5f, 0.0f));
    // Crimson nose ring, radiator grille, exhaust stacks, pusher propeller.
    AddTube(mesh, Vec3(0, 0, -3.3f), Vec3(0, 0, -2.6f), 1.62f, 1.82f, 14, Swatch(ZS_WHITE), CRIMSON, false);
    AddTube(mesh, Vec3(0, 0, -4.05f), Vec3(0, 0, -4.15f), 1.05f, 1.05f, 10, Swatch(ZS_WHITE), 0xFF2A2A2Au, true);
    for (int side = -1; side <= 1; side += 2)
        AddTube(mesh, Vec3(side * 1.4f, 0.9f, 0.5f), Vec3(side * 1.6f, 1.2f, 2.6f), 0.22f, 0.25f, 6,
                Swatch(ZS_WHITE), 0xFF5A3A2Au, true);
    MeshData hub;
    AddEllipsoid(hub, Vec3(0, 0, 4.3f), Vec3(0.55f, 0.55f, 0.8f), 10, 5, Swatch(ZS_WHITE), CRIMSON);
    Append(mesh, hub);
    for (int i = 0; i < 4; i++) {
        MeshData blade;
        AddBox(blade, Vec3(0, 1.7f, 0), Vec3(0.35f, 1.6f, 0.06f), Swatch(ZS_WHITE), 0xFF3A3330u);
        AddBox(blade, Vec3(0, 3.1f, 0), Vec3(0.3f, 0.2f, 0.07f), Swatch(ZS_WHITE), 0xFFFFD23Fu);
        Append(mesh, blade, Place(Vec3(0, 0, 4.4f), 0.0f, 0.35f, i * PI * 0.5f + 0.4f));
    }
    TransformMesh(mesh, MatScale(Vec3(ENGINE_SCALE, ENGINE_SCALE, ENGINE_SCALE)));
    return mesh;
}

int ZeppelinTurretCount()
{
    return 4;
}

Vec3 ZeppelinTurret(int index)
{
    return TURRET_POS[index];
}

MeshData MakeTurret()
{
    MeshData mesh;
    // Rotating base ring, armoured dome with a slit, twin barrels with flash hiders.
    AddTube(mesh, Vec3(0, 0, 0), Vec3(0, 0.6f, 0), 2.1f, 2.0f, 14, Swatch(ZS_WHITE), DARK, true);
    MeshData dome;
    AddEllipsoid(dome, Vec3(0, 0.6f, 0), Vec3(1.8f, 1.6f, 1.9f), 14, 6, Swatch(ZS_WHITE), 0xFF5A5E64u);
    Append(mesh, dome);
    AddBox(mesh, Vec3(0, 1.5f, -1.45f), Vec3(1.0f, 0.12f, 0.35f), Swatch(ZS_WHITE), 0xFF1A1A1Au);
    AddBox(mesh, Vec3(0, 0.9f, -1.6f), Vec3(1.2f, 0.45f, 0.5f), Swatch(ZS_WHITE), CRIMSON);
    for (int side = -1; side <= 1; side += 2) {
        Vec3 a(side * 0.55f, 0.95f, -1.6f);
        AddTube(mesh, a, a + Vec3(0, 0, -3.6f), 0.2f, 0.17f, 8, Swatch(ZS_WHITE), 0xFF2A2A2Eu, true);
        AddTube(mesh, a + Vec3(0, 0, -3.3f), a + Vec3(0, 0, -3.9f), 0.27f, 0.27f, 8, Swatch(ZS_WHITE), 0xFF1A1A1Au,
                true);
    }
    TransformMesh(mesh, MatScale(Vec3(TURRET_SCALE, TURRET_SCALE, TURRET_SCALE)));
    return mesh;
}

// --- Texture

// Hull texel: alternating panels, girder shadows, seams, band, emblem.
static Color HullTexel(float u, float v)
{
    float z = -LENGTH * 0.5f + u * LENGTH;
    float r = HullRadius(z);
    float arc = v * PI * r;
    int gore = static_cast<int>(v * 12.0f);
    // Bright silver (albedo about B4BAC0, review 4 §3), so the hull's shadowed side stays silver-grey, not black.
    Color c = gore % 2 == 0 ? Hex(0xC2C8CE) : Hex(0xB2B8BE);
    // Every panel (gore x bay) slightly lighter or darker, +-8 %: big things read big through small variation.
    int bayIndex = static_cast<int>(Floor(z / 11.25f));
    c = Shade(c, 0.92f + 0.16f * Hash2(gore, bayIndex, 79u));
    // Fabric: lengthwise weave noise, gentle sag between frames (darker in the middle of each bay).
    float bay = z / 11.25f - Floor(z / 11.25f);
    c = Shade(c, 0.94f + 0.06f * FastCos(bay * 2.0f * PI) + 0.05f * (Fbm(u * 60.0f, v * 8.0f, 3, 71u) - 0.5f));
    // Frames and seams.
    float frameD = Abs(bay - 0.5f) * 2.0f;
    if (frameD > 0.93f)
        c = Shade(c, 0.78f);
    float seam = Abs(v * 12.0f - Floor(v * 12.0f + 0.5f));
    if (seam < 0.045f)
        c = Shade(c, 0.85f);
    // Weathering streaks running down the sides.
    c = Shade(c, 1.0f - 0.07f * Fbm(u * 90.0f, v * 2.0f, 2, 73u) * SmoothStep(0.3f, 0.9f, v));
    // Nose cap and tail cone in crimson; the black band with crimson pinstripes behind the nose.
    if (z < -86.0f)
        c = Hex(0xC1121F);
    if (z > -60.0f && z < -54.0f)
        c = Hex(0x1A1A1A);
    if ((z > -61.2f && z < -60.0f) || (z > -54.0f && z < -52.8f))
        c = Hex(0xC1121F);
    if (z > 70.0f && z < 73.0f)
        c = Hex(0x1A1A1A);
    // The emblem on each flank: crimson disc, ink rim, the skull and crossed bananas.
    Vec2 p = {(z + 18.0f) / 15.0f, (arc - PI * r * 0.5f + 1.0f) / 15.0f};
    float aa = (LENGTH / 256.0f) / 15.0f;
    float d = Sqrt(p.x * p.x + p.y * p.y);
    if (d < 1.2f) {
        c = Over(c, Hex(PAL_INK), Coverage(d - 1.08f, aa));
        c = Over(c, Hex(0xC1121F), Coverage(d - 1.0f, aa));
        c = Over(c, Hex(PAL_INK), Coverage(Abs(d - 0.9f) - 0.02f, aa));
        c = PaintPirateInsignia(c, Vec2{p.x * 1.05f, p.y * 1.05f}, aa * 1.05f, Hex(PAL_BONE));
    }
    return c;
}

static Color GondolaTexel(float u, float v)
{
    float side = v < 0.5f ? v : 1.0f - v;
    Color c = Hex(0x3A3A3E);
    if (side < 0.1f)
        c = Hex(0x9AA0A6);
    if (side > 0.12f && side < 0.16f)
        c = Hex(0xC1121F);
    // Two rows of windows glowing warm FFD98A, with dark frames: a tall lower row and a small upper one.
    bool lower = side > 0.19f && side < 0.3f;
    bool upper = side > 0.135f && side < 0.165f;
    if ((lower || upper) && u > 0.1f && u < 0.93f) {
        float w = u * (upper ? 30.0f : 18.0f);
        w -= Floor(w);
        c = w > 0.2f ? Mix(Hex(0xFFD98A), Hex(0xFFF2C0), lower ? (side - 0.19f) * 6.0f : 0.3f) : Hex(0x1A1A1A);
    }
    if (u < 0.08f && side > 0.12f)
        c = Hex(0x6A8CA0);
    return c;
}

// Fin texel: u along the fin (leading edge at 0), v from the tip (0) to the root (1). Hull panels, a black band with
// crimson pinstripes near the root, and the gorilla-skull roundel.
static Color FinTexel(float u, float v)
{
    Color c = (static_cast<int>(u * 6.0f) % 2 == 0) ? Hex(0xC2C8CE) : Hex(0xB2B8BE);
    c = Shade(c, 0.95f + 0.08f * Hash2(static_cast<int>(u * 6.0f), static_cast<int>(v * 3.0f), 83u));
    if (Abs(u * 6.0f - Floor(u * 6.0f + 0.5f)) < 0.03f || Abs(v * 3.0f - Floor(v * 3.0f + 0.5f)) < 0.02f)
        c = Shade(c, 0.8f);
    if (v > 0.8f && v < 0.9f)
        c = Hex(0x1A1A1A);
    if ((v > 0.78f && v < 0.8f) || (v > 0.9f && v < 0.92f))
        c = Hex(0xC1121F);
    // Roundel: 36 x 18 m of fin over the rectangle, so the circle is drawn in metres.
    Vec2 p = {(u - 0.5f) * 36.0f / 6.0f, (v - 0.45f) * 18.0f / 6.0f};
    float d = Sqrt(p.x * p.x + p.y * p.y);
    float aa = (36.0f / 128.0f) / 6.0f;
    if (d < 1.2f) {
        c = Over(c, Hex(PAL_INK), Coverage(d - 1.08f, aa));
        c = Over(c, Hex(0xC1121F), Coverage(d - 1.0f, aa));
        c = PaintGorillaSkull(c, Vec2{p.x * 1.35f, p.y * 1.35f + 0.05f}, aa * 1.35f, Hex(PAL_BONE));
    }
    return c;
}

ImageData MakeZeppelinTexture()
{
    Canvas canvas(ZEP_TEX, ZEP_TEX, Hex(0xFFFFFF));
    for (int y = 0; y < HULL_TEX_ROWS; y++) {
        for (int x = 0; x < ZEP_TEX; x++)
            canvas.At(x, y) = HullTexel((x + 0.5f) / ZEP_TEX, (y + 0.5f) / HULL_TEX_ROWS);
    }
    for (int y = 0; y < ZT_FIN.h; y++) {
        for (int x = 0; x < ZT_FIN.w; x++)
            canvas.At(ZT_FIN.x + x, ZT_FIN.y + y) = FinTexel((x + 0.5f) / ZT_FIN.w, (y + 0.5f) / ZT_FIN.h);
    }
    for (int y = 0; y < ZT_GONDOLA.h; y++) {
        for (int x = 0; x < ZT_GONDOLA.w; x++)
            canvas.At(ZT_GONDOLA.x + x, ZT_GONDOLA.y + y) = GondolaTexel((x + 0.5f) / ZT_GONDOLA.w,
                                                                         (y + 0.5f) / ZT_GONDOLA.h);
    }
    static const uint32_t SWATCHES[ZS_COUNT] = {0xFFFFFF, 0x3A3A3E, 0x4A4038, 0x1A1410,
                                                0xC1121F, 0xA9AEB3, 0xC9A227, 0x6A8CA0};
    for (int i = 0; i < ZS_COUNT; i++) {
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++)
                canvas.At(ZT_SWATCH_X + (i % 8) * 16 + x, ZT_SWATCH_Y + (i / 8) * 16 + y) = Hex(SWATCHES[i]);
        }
    }
    return canvas.ToImage();
}

} // namespace content
