// Plane meshes: the player's Spitfire-like fighter, the pirates' radial gull-wing fighters (the aces with twin
// fins), their pilots (a baboon, gorillas), propellers, propeller discs, canopy glass and the player's scarf.
// docs/design.md §7; the livery painter (plane_textures.cpp) shares the shapes in plane_shapes.h.
#include "content/content.h"
#include "content/mesh_builder.h"
#include "content/plane_shapes.h"

#include <math.h>

namespace content {

// Colours of the pilots and fittings (vertex colours over the white swatch).
constexpr uint32_t WHITE = 0xFFFFFFFFu;
constexpr uint32_t BABOON_FUR = 0xFF9C7A48u;
constexpr uint32_t BABOON_CHEEK = 0xFFC9AC72u;
constexpr uint32_t BABOON_MUZZLE = 0xFFD9927Au;
constexpr uint32_t BABOON_JAW = 0xFFC07A62u;
constexpr uint32_t BABOON_NOSE = 0xFF4A2A26u;
constexpr uint32_t GORILLA_HEAD = 0xFF2A2A2Au;
constexpr uint32_t GORILLA_BROW = 0xFFB8B8B8u;
constexpr uint32_t GORILLA_FACE = 0xFF4E4644u;
constexpr uint32_t LEATHER = 0xFF6B4423u;
constexpr uint32_t LEATHER_DARK = 0xFF3B2A1Eu;
constexpr uint32_t BRASS = 0xFFC9A227u;
constexpr uint32_t LENS = 0xFF1E3A4Au;
constexpr uint32_t GLINT = 0xFFF5F2EAu;
constexpr uint32_t SCARF_RED = 0xFFC1121Fu;
constexpr uint32_t INK = 0xFF1A1410u;

// Cartoon scale of the pilots' heads and the canopy.
constexpr float HEAD_SCALE = 1.3f;

// Scarf ribbon dimensions.
constexpr int SCARF_SEGMENTS = 5;
constexpr float SCARF_SEGMENT_LENGTH = 0.34f;
constexpr float SCARF_WIDTH = 0.16f;

// --- Helpers

static UvRect Swatch(int swatch)
{
    return PlaneSwatchUv(swatch);
}

static uint32_t LiveryPrimary(Livery livery)
{
    if (livery == Livery::PLAYER_RED)
        return 0xFFC1121Fu;
    return livery == Livery::PIRATE_PURPLE ? 0xFF5B2A86u : 0xFF3A3F47u;
}

static uint32_t LiveryTrim(Livery livery)
{
    if (livery == Livery::PLAYER_RED)
        return 0xFFF2E6C9u;
    return livery == Livery::PIRATE_PURPLE ? 0xFFD4A017u : 0xFFE8B923u;
}

// An ellipsoid placed with a rotation (yaw, pitch in radians) around its centre.
static void AddBlob(MeshData& mesh, const Vec3& c, const Vec3& r, uint32_t color, int slices, int stacks,
                    float yaw = 0.0f, float pitch = 0.0f, float roll = 0.0f)
{
    MeshData part;
    AddEllipsoid(part, Vec3(), r, slices, stacks, Swatch(SW_WHITE), color);
    Append(mesh, part, Place(c, yaw, pitch, roll));
}

// --- Fuselage and flying surfaces

static void AddFuselage(MeshData& mesh, Airframe airframe)
{
    FuselageDesc d = FuselageOf(airframe);
    UvRect r = PlaneRect(PT_FUSELAGE);
    AddSurface(
        mesh, 16, 22,
        [&](float s, float t) {
            return FuselagePoint(d, t, s * 2.0f * PI);
        },
        [&](float s, float t) {
            return Vec2{r.u0 + t * (r.u1 - r.u0), r.v0 + s * (r.v1 - r.v0)};
        },
        [&](float, float) {
            return WHITE;
        },
        SurfaceOptions{true, true});
}

// One wing surface pair (top and bottom), right side only, in wing space.
static MeshData MakeWingHalf(const WingDesc& w, const AtlasRect& topRect, const AtlasRect& bottomRect, int chordCols,
                             int spanRows)
{
    MeshData m;
    for (int side = 0; side < 2; side++) {
        bool top = side == 0;
        UvRect r = PlaneRect(top ? topRect : bottomRect);
        AddSurface(
            m, chordCols, spanRows,
            [&](float s, float t) {
                return WingPoint(w, t, WingChordFrac(s), top);
            },
            [&](float s, float t) {
                return Vec2{r.u0 + WingSpanU(t) * (r.u1 - r.u0), r.v0 + WingChordFrac(s) * (r.v1 - r.v0)};
            },
            [&](float, float) {
                return WHITE;
            },
            SurfaceOptions{!top, false});
    }
    return m;
}

// Both halves of a wing or tailplane.
static void AddWingPair(MeshData& mesh, const WingDesc& w, const AtlasRect& topRect, const AtlasRect& bottomRect,
                        int chordCols, int spanRows)
{
    MeshData half = MakeWingHalf(w, topRect, bottomRect, chordCols, spanRows);
    Append(mesh, half);
    Append(mesh, half, MatScale(Vec3(-1, 1, 1)));
}

// A fin: a wing standing up (span along +Y), both faces mapped to the fin rectangle (v from the tip down).
static void AddFin(MeshData& mesh, const WingDesc& w, const Vec3& root, float lean)
{
    MeshData fin;
    UvRect r = PlaneRect(PT_FIN);
    for (int side = 0; side < 2; side++) {
        bool top = side == 0;
        AddSurface(
            fin, 6, 7,
            [&](float s, float t) {
                return WingPoint(w, t, WingChordFrac(s), top);
            },
            [&](float s, float t) {
                return Vec2{r.u0 + WingChordFrac(s) * (r.u1 - r.u0), r.v1 - WingSpanU(t) * (r.v1 - r.v0)};
            },
            [&](float, float) {
                return WHITE;
            },
            SurfaceOptions{!top, false});
    }
    Append(mesh, fin, Place(root, 0.0f, 0.0f, PI * 0.5f + lean));
}

// --- Pilots

static void AddGoggle(MeshData& mesh, const Vec3& c, const Vec3& dir, float r)
{
    AddTube(mesh, c, c + dir * 0.05f, r, r * 0.95f, 8, Swatch(SW_WHITE), BRASS, true);
    Vec3 front = c + dir * 0.052f;
    AddBlob(mesh, front, Vec3(r * 0.8f, r * 0.8f, 0.01f), LENS, 6, 3, FastAtan2(-dir.x, -dir.z), asinf(dir.y));
    AddBlob(mesh, front + Vec3(r * 0.3f, r * 0.35f, -0.004f), Vec3(r * 0.22f, r * 0.22f, 0.006f), GLINT, 4, 2);
}

// The Red Baboon: olive fur, long muzzle, leather cap, brass goggles, crimson scarf collar, leather jacket.
static void AddBaboon(MeshData& mesh, const Vec3& h)
{
    MeshData head;
    AddBlob(head, Vec3(0, -0.33f, 0.10f), Vec3(0.24f, 0.13f, 0.17f), LEATHER_DARK, 8, 4);
    AddBlob(head, Vec3(0, 0, 0), Vec3(0.15f, 0.16f, 0.165f), BABOON_FUR, 10, 6);
    AddBlob(head, Vec3(0, 0.05f, 0.03f), Vec3(0.158f, 0.14f, 0.168f), LEATHER, 10, 5);
    AddBlob(head, Vec3(0.148f, -0.03f, 0.03f), Vec3(0.03f, 0.075f, 0.06f), LEATHER, 6, 4);
    AddBlob(head, Vec3(-0.148f, -0.03f, 0.03f), Vec3(0.03f, 0.075f, 0.06f), LEATHER, 6, 4);
    AddBlob(head, Vec3(0.105f, -0.07f, -0.05f), Vec3(0.07f, 0.075f, 0.07f), BABOON_CHEEK, 6, 4);
    AddBlob(head, Vec3(-0.105f, -0.07f, -0.05f), Vec3(0.07f, 0.075f, 0.07f), BABOON_CHEEK, 6, 4);
    AddBlob(head, Vec3(0, 0.045f, -0.125f), Vec3(0.125f, 0.035f, 0.05f), BABOON_FUR, 6, 4);
    AddBlob(head, Vec3(0, -0.055f, -0.215f), Vec3(0.072f, 0.068f, 0.135f), BABOON_MUZZLE, 10, 5, 0.0f, -0.22f);
    AddBlob(head, Vec3(0, -0.108f, -0.175f), Vec3(0.062f, 0.04f, 0.1f), BABOON_JAW, 8, 4, 0.0f, -0.1f);
    AddBlob(head, Vec3(0, -0.03f, -0.338f), Vec3(0.052f, 0.036f, 0.028f), BABOON_NOSE, 6, 4);
    AddSweep(
        head, 10, 3,
        [](float t) {
            float a = t * 2.0f * PI;
            return Vec3(0.163f * FastSin(a), 0.02f, -0.172f * FastCos(a));
        },
        [](float) {
            return 0.012f;
        },
        Swatch(SW_WHITE), LEATHER_DARK);
    AddGoggle(head, Vec3(0.06f, 0.025f, -0.14f), Normalize(Vec3(0.25f, 0.05f, -1.0f)), 0.045f);
    AddGoggle(head, Vec3(-0.06f, 0.025f, -0.14f), Normalize(Vec3(-0.25f, 0.05f, -1.0f)), 0.045f);
    MeshData collar;
    AddTorus(collar, 0.1f, 0.045f, 10, 4, Swatch(SW_WHITE), SCARF_RED);
    Append(head, collar, Place(Vec3(0, -0.175f, 0.03f), 0.0f, PI * 0.5f, 0.0f, Vec3(1.15f, 1.0f, 1.0f)));
    Append(mesh, head, MatScale(Vec3(HEAD_SCALE, HEAD_SCALE, HEAD_SCALE)) * MatTranslate(h));
}

// A pirate gorilla: black head with a silver brow, a pointed crest, leather cap with goggles pushed up, an eyepatch
// and a neckerchief in the livery's trim colour.
static void AddGorilla(MeshData& mesh, const Vec3& h, uint32_t kerchief)
{
    MeshData head;
    AddBlob(head, Vec3(0, -0.32f, 0.10f), Vec3(0.27f, 0.15f, 0.18f), LEATHER_DARK, 6, 4);
    AddBlob(head, Vec3(0, 0, 0), Vec3(0.17f, 0.17f, 0.165f), GORILLA_HEAD, 10, 6);
    AddBlob(head, Vec3(0, 0.08f, 0.04f), Vec3(0.165f, 0.13f, 0.16f), LEATHER, 10, 5);
    AddBlob(head, Vec3(0, 0.03f, -0.13f), Vec3(0.135f, 0.038f, 0.045f), GORILLA_BROW, 8, 4);
    AddBlob(head, Vec3(0, -0.065f, -0.13f), Vec3(0.12f, 0.085f, 0.075f), GORILLA_FACE, 9, 5);
    AddBlob(head, Vec3(0, -0.125f, -0.1f), Vec3(0.115f, 0.05f, 0.075f), GORILLA_FACE, 6, 4);
    AddBlob(head, Vec3(0.032f, -0.04f, -0.2f), Vec3(0.026f, 0.02f, 0.012f), INK, 6, 4);
    AddBlob(head, Vec3(-0.032f, -0.04f, -0.2f), Vec3(0.026f, 0.02f, 0.012f), INK, 6, 4);
    AddBlob(head, Vec3(-0.058f, -0.002f, -0.15f), Vec3(0.022f, 0.018f, 0.01f), 0xFFE8C060u, 6, 4);
    AddBlob(head, Vec3(0.058f, -0.002f, -0.152f), Vec3(0.04f, 0.035f, 0.012f), INK, 6, 4);
    AddSweep(
        head, 10, 3,
        [](float t) {
            float a = t * 2.0f * PI;
            return Vec3(0.178f * FastSin(a), 0.0f + 0.07f * FastCos(a), -0.17f * FastCos(a));
        },
        [](float) {
            return 0.008f;
        },
        Swatch(SW_WHITE), INK);
    AddBlob(head, Vec3(0.168f, -0.02f, 0.02f), Vec3(0.03f, 0.05f, 0.04f), GORILLA_HEAD, 6, 4);
    AddBlob(head, Vec3(-0.168f, -0.02f, 0.02f), Vec3(0.03f, 0.05f, 0.04f), GORILLA_HEAD, 6, 4);
    AddGoggle(head, Vec3(0.06f, 0.13f, -0.1f), Normalize(Vec3(0.2f, 0.6f, -1.0f)), 0.042f);
    AddGoggle(head, Vec3(-0.06f, 0.13f, -0.1f), Normalize(Vec3(-0.2f, 0.6f, -1.0f)), 0.042f);
    MeshData collar;
    AddTorus(collar, 0.12f, 0.05f, 10, 4, Swatch(SW_WHITE), kerchief);
    Append(head, collar, Place(Vec3(0, -0.19f, 0.03f), 0.0f, PI * 0.5f, 0.0f, Vec3(1.15f, 1.0f, 1.0f)));
    Append(mesh, head, MatScale(Vec3(HEAD_SCALE, HEAD_SCALE, HEAD_SCALE)) * MatTranslate(h));
}

// --- The player's plane

// Canopy bubble: centre and radii (already at the cartoon 130 % scale).
constexpr Vec3 SPITFIRE_CANOPY_C = Vec3(0.0f, 0.55f, 0.32f);
constexpr Vec3 SPITFIRE_CANOPY_R = Vec3(0.40f, 0.64f, 1.05f);

// Point on the canopy bubble: s around (0 front ... 1), t from the top (0) to the base (1).
static Vec3 CanopyPoint(float s, float t)
{
    float a = s * 2.0f * PI;
    float b = t * PI * 0.5f;
    return SPITFIRE_CANOPY_C + Vec3(SPITFIRE_CANOPY_R.x * FastSin(b) * FastSin(a), SPITFIRE_CANOPY_R.y * FastCos(b),
                                    -SPITFIRE_CANOPY_R.z * FastSin(b) * FastCos(a));
}

// An arch of the canopy frame at a fraction k of the bubble's length (-1 front .. 1 back).
static void AddCanopyArch(MeshData& mesh, float k)
{
    float q = Sqrt(Max(1.0f - k * k, 0.0f));
    AddSweep(
        mesh, 8, 4,
        [&](float t) {
            float phi = (t - 0.5f) * PI;
            Vec3 c = SPITFIRE_CANOPY_C;
            return Vec3(c.x + SPITFIRE_CANOPY_R.x * q * FastSin(phi) * 1.02f,
                        c.y + SPITFIRE_CANOPY_R.y * q * FastCos(phi),
                        c.z + SPITFIRE_CANOPY_R.z * k);
        },
        [](float) {
            return 0.024f;
        },
        Swatch(SW_PRIMARY_DARK), WHITE);
}

static void AddSpitfireDetails(MeshData& mesh)
{
    const WingDesc& w = SPITFIRE_WING;
    // Canopy frame: windscreen arch, hood arch, base rails.
    AddCanopyArch(mesh, -0.6f);
    AddCanopyArch(mesh, 0.62f);
    for (int side = -1; side <= 1; side += 2) {
        AddSweep(
            mesh, 10, 3,
            [&](float t) {
                float phi = t * PI;
                Vec3 c = SPITFIRE_CANOPY_C;
                return Vec3(c.x + side * SPITFIRE_CANOPY_R.x * FastSin(phi) * 1.01f, c.y + 0.02f,
                            c.z - SPITFIRE_CANOPY_R.z * FastCos(phi));
            },
            [](float) {
                return 0.028f;
            },
            Swatch(SW_PRIMARY_DARK), WHITE);
    }
    // Radiator under the right wing, oil cooler under the left.
    float ry = WingY(w, 1.45f);
    AddBlob(mesh, Vec3(1.45f, ry - 0.16f, -0.05f), Vec3(0.24f, 0.16f, 0.8f), 0xFF8A0E18u, 8, 5);
    AddBlob(mesh, Vec3(1.45f, ry - 0.17f, -0.82f), Vec3(0.19f, 0.11f, 0.05f), INK, 6, 4);
    AddBlob(mesh, Vec3(-1.25f, ry - 0.12f, -0.4f), Vec3(0.13f, 0.12f, 0.45f), 0xFF8A0E18u, 6, 4);
    AddBlob(mesh, Vec3(-1.25f, ry - 0.12f, -0.84f), Vec3(0.09f, 0.08f, 0.04f), INK, 6, 4);
    // Landing gear fairings (doors of the retracted gear) under the wings.
    for (int side = -1; side <= 1; side += 2)
        AddBlob(mesh, Vec3(side * 1.05f, WingY(w, 1.05f) - 0.18f, -0.55f), Vec3(0.2f, 0.06f, 0.55f), 0xFF8A0E18u,
                8, 5);
    // Exhaust stubs: three per side on the nose.
    for (int side = -1; side <= 1; side += 2) {
        for (int i = 0; i < 3; i++) {
            float z = -3.82f + i * 0.3f;
            Vec3 a(side * 0.42f, 0.16f, z);
            Vec3 b(side * 0.58f, 0.13f, z + 0.12f);
            AddTube(mesh, a, b, 0.055f, 0.06f, 6, Swatch(SW_EXHAUST), WHITE, true);
        }
    }
    // Wing guns: two barrels per wing; the inner ones are the muzzles of GunMuzzle.
    for (int side = -1; side <= 1; side += 2) {
        for (int i = 0; i < 2; i++) {
            float x = 2.3f + i * 0.35f;
            float chord = WingChord(w, x);
            float lead = w.lineZ - w.lineFrac * chord;
            Vec3 a(side * x, WingY(w, x) + 0.01f, lead + 0.15f);
            AddTube(mesh, a, a + Vec3(0, 0, -0.55f + i * 0.15f), 0.035f, 0.03f, 6, Swatch(SW_DARK_METAL), WHITE,
                    true);
        }
    }
    // Antenna mast behind the canopy, and a pitot tube under the left wing.
    AddTube(mesh, Vec3(0, 0.56f, 1.75f), Vec3(0, 1.0f, 2.05f), 0.035f, 0.012f, 6, Swatch(SW_DARK_METAL), WHITE,
            false);
    float px = -3.6f;
    float pc = WingChord(w, Abs(px));
    AddTube(mesh, Vec3(px, WingY(w, 3.6f) - 0.06f, w.lineZ - w.lineFrac * pc + 0.1f),
            Vec3(px, WingY(w, 3.6f) - 0.06f, w.lineZ - w.lineFrac * pc - 0.45f), 0.018f, 0.014f, 5,
            Swatch(SW_METAL), WHITE, false);
}

static MeshData MakeSpitfire()
{
    MeshData mesh;
    AddFuselage(mesh, Airframe::SPITFIRE);
    AddWingPair(mesh, SPITFIRE_WING, PT_WING_TOP, PT_WING_BOTTOM, 10, 12);
    AddWingPair(mesh, SPITFIRE_TAIL, PT_TAIL_TOP, PT_TAIL_BOTTOM, 6, 6);
    AddFin(mesh, SPITFIRE_FIN, Vec3(0, 0.30f, 0), 0.0f);
    AddSpitfireDetails(mesh);
    AddBaboon(mesh, PilotHead(Airframe::SPITFIRE));
    return mesh;
}

// --- The pirates' planes

// Radial engine cowling ring, its engine face with cylinders, and exhaust stubs.
static void AddCowling(MeshData& mesh)
{
    std::vector<Vec2> profile = {{0.80f, 2.95f}, {0.86f, 3.25f}, {0.87f, 3.55f}, {0.84f, 3.82f}, {0.78f, 3.94f},
                                 {0.70f, 3.93f}, {0.65f, 3.82f}, {0.63f, 3.55f}, {0.62f, 3.25f}};
    MeshData ring;
    AddLathe(ring, profile, 20, PlaneRect(PT_COWL), WHITE, true);
    Append(mesh, ring, Place(Vec3(), 0.0f, -PI * 0.5f, 0.0f));
    // Engine face: dark crankcase disc, nine cylinders, the crankcase hub.
    MeshData engine;
    AddTube(engine, Vec3(0, 0, -3.28f), Vec3(0, 0, -3.3f), 0.64f, 0.64f, 18, Swatch(SW_DARK_METAL), 0xFF404040u,
            true);
    for (int i = 0; i < 9; i++) {
        float a = i * 2.0f * PI / 9.0f;
        Vec3 dir(FastSin(a), FastCos(a), 0);
        AddTube(engine, dir * 0.18f + Vec3(0, 0, -3.42f), dir * 0.56f + Vec3(0, 0, -3.42f), 0.085f, 0.075f, 6,
                Swatch(SW_DARK_METAL), WHITE, true);
    }
    AddBlob(engine, Vec3(0, 0, -3.42f), Vec3(0.24f, 0.24f, 0.14f), 0xFF6A6A6Au, 8, 4);
    Append(mesh, engine);
    for (int side = -1; side <= 1; side += 2) {
        for (int i = 0; i < 2; i++) {
            Vec3 a(side * 0.62f, -0.30f - i * 0.22f, -2.9f);
            AddTube(mesh, a, a + Vec3(side * 0.08f, -0.03f, 0.45f), 0.07f, 0.065f, 7, Swatch(SW_EXHAUST), WHITE,
                    true);
        }
    }
}

static void AddPirateDetails(MeshData& mesh, Livery livery)
{
    const WingDesc& w = PIRATE_WING;
    uint32_t primary = LiveryPrimary(livery);
    Vec3 head = PilotHead(Airframe::PIRATE);
    // Cockpit coaming and the headrest fairing behind the pilot.
    MeshData coaming;
    AddTorus(coaming, 0.36f, 0.045f, 14, 4, Swatch(SW_WHITE), LEATHER);
    Append(mesh, coaming, Place(Vec3(0, 0.79f, head.z - 0.02f), 0.0f, PI * 0.5f, 0.0f, Vec3(1.0f, 1.45f, 1.0f)));
    AddBlob(mesh, Vec3(0, 0.74f, head.z + 0.95f), Vec3(0.18f, 0.18f, 0.8f), primary, 8, 4);
    // Windscreen frame.
    AddSweep(
        mesh, 10, 4,
        [&](float t) {
            float phi = (t - 0.5f) * 1.7f;
            return Vec3(0.40f * FastSin(phi), 1.20f, head.z - 0.46f + 0.14f * (1.0f - FastCos(phi)));
        },
        [](float) {
            return 0.025f;
        },
        Swatch(SW_DARK_METAL), WHITE);
    // Fixed landing gear: struts and wheel spats under the gull-wing knees.
    for (int side = -1; side <= 1; side += 2) {
        float x = side * w.breakX;
        Vec3 knee(x, WingY(w, w.breakX) - 0.05f, -0.45f);
        Vec3 spat(x, -1.55f, -0.55f);
        AddTube(mesh, knee, spat + Vec3(0, 0.2f, 0), 0.07f, 0.06f, 6, Swatch(SW_WHITE), primary, false);
        AddBlob(mesh, spat, Vec3(0.17f, 0.30f, 0.62f), primary, 8, 5);
        AddBlob(mesh, spat + Vec3(0, -0.26f, 0.05f), Vec3(0.08f, 0.1f, 0.22f), 0xFF262626u, 6, 4);
    }
    // Guns: two on the cowling, one in each wing.
    for (int side = -1; side <= 1; side += 2) {
        AddTube(mesh, Vec3(side * 0.22f, 0.76f, -1.9f), Vec3(side * 0.22f, 0.76f, -3.7f), 0.045f, 0.04f, 6,
                Swatch(SW_DARK_METAL), WHITE, true);
        float gx = 2.6f;
        float chord = WingChord(w, gx);
        Vec3 a(side * gx, WingY(w, gx), w.lineZ + w.sweep * gx - w.lineFrac * chord + 0.1f);
        AddTube(mesh, a, a + Vec3(0, 0, -0.6f), 0.05f, 0.04f, 6, Swatch(SW_DARK_METAL), WHITE, true);
    }
}

static MeshData MakePirate(Livery livery)
{
    Airframe airframe = AirframeOf(livery);
    MeshData mesh;
    AddFuselage(mesh, airframe);
    AddCowling(mesh);
    AddWingPair(mesh, PIRATE_WING, PT_WING_TOP, PT_WING_BOTTOM, 8, 10);
    AddWingPair(mesh, PIRATE_TAIL, PT_TAIL_TOP, PT_TAIL_BOTTOM, 6, 5);
    if (airframe == Airframe::PIRATE_ACE) {
        UvRect r = PlaneRect(PT_FIN);
        for (int side = -1; side <= 1; side += 2) {
            MeshData fin;
            AddSurface(
                fin, 10, 6,
                [&](float s, float t) {
                    float a = s * 2.0f * PI;
                    float b = t * PI;
                    return Vec3(0.07f * FastSin(b) * FastSin(a), 0.78f * FastCos(b), 0.62f * FastSin(b) * FastCos(a));
                },
                [&](float s, float t) {
                    return Vec2{r.u0 + s * (r.u1 - r.u0), r.v0 + t * (r.v1 - r.v0)};
                },
                [&](float, float) {
                    return WHITE;
                },
                SurfaceOptions{true, true});
            Append(mesh, fin, Place(Vec3(side * 1.86f, 0.52f, 3.2f), 0.0f, 0.0f, side * -0.08f));
        }
    } else {
        AddFin(mesh, PIRATE_FIN, Vec3(0, 0.30f, 0), 0.0f);
    }
    AddPirateDetails(mesh, livery);
    AddGorilla(mesh, PilotHead(airframe), LiveryTrim(livery));
    TransformMesh(mesh, MatScale(Vec3(PIRATE_SCALE, PIRATE_SCALE, PIRATE_SCALE)));
    return mesh;
}

MeshData MakePlane(Livery livery)
{
    if (livery == Livery::PLAYER_RED)
        return MakeSpitfire();
    return MakePirate(livery);
}

// --- Canopy glass

MeshData MakeCanopyGlass(Livery livery)
{
    MeshData mesh;
    UvRect r = PlaneRect(PT_GLASS);
    if (livery == Livery::PLAYER_RED) {
        AddSurface(
            mesh, 20, 6,
            [](float s, float t) {
                return CanopyPoint(s, t);
            },
            [&](float s, float t) {
                return Vec2{r.u0 + s * (r.u1 - r.u0), r.v0 + t * (r.v1 - r.v0)};
            },
            [](float, float) {
                return WHITE;
            },
            SurfaceOptions{true, true});
        return mesh;
    }
    Vec3 head = PilotHead(Airframe::PIRATE);
    for (int side = 0; side < 2; side++) {
        AddSurface(
            mesh, 8, 3,
            [&](float s, float t) {
                float phi = (s - 0.5f) * 1.7f;
                float zc = head.z - 0.62f + 0.14f * (1.0f - FastCos(phi));
                return Vec3(0.40f * FastSin(phi), 0.80f + t * 0.40f, zc + t * 0.16f);
            },
            [&](float s, float t) {
                return Vec2{r.u0 + s * (r.u1 - r.u0), r.v0 + t * (r.v1 - r.v0)};
            },
            [](float, float) {
                return WHITE;
            },
            SurfaceOptions{side == 1, false});
    }
    TransformMesh(mesh, MatScale(Vec3(PIRATE_SCALE, PIRATE_SCALE, PIRATE_SCALE)));
    return mesh;
}

// --- Propellers

Vec3 PropellerHub()
{
    return Vec3(0.0f, 0.06f, -4.42f);
}

Vec3 PropellerHub(Livery livery)
{
    if (livery == Livery::PLAYER_RED)
        return PropellerHub();
    return Vec3(0.0f, 0.0f, -3.96f) * PIRATE_SCALE;
}

static float PropellerRadius(Livery livery)
{
    return livery == Livery::PLAYER_RED ? 1.4f : 1.6f;
}

// One blade along +Y from the hub: paddle planform, twisted (steep at the root, flat at the tip), yellow tip.
static void AddBlade(MeshData& mesh, float radius, float hubRadius, float width, float angle)
{
    MeshData blade;
    float tipStart = 0.87f;
    for (int part = 0; part < 2; part++) {
        float t0 = part == 0 ? 0.0f : tipStart;
        float t1 = part == 0 ? tipStart : 1.0f;
        AddSurface(
            blade, 8, part == 0 ? 7 : 3,
            [&](float s, float tt) {
                float t = Lerp(t0, t1, tt);
                float r = Lerp(hubRadius, radius, t);
                float chord = width * (0.55f + 0.45f * FastSin(Clamp(t * 1.25f, 0.0f, 1.0f) * PI * 0.5f));
                if (t > 0.8f)
                    chord *= Sqrt(Max(1.0f - (t - 0.8f) * (t - 0.8f) / 0.04f, 0.0f));
                float twist = Lerp(55.0f, 18.0f, t) * DEG;
                float a = s * 2.0f * PI;
                float cx = 0.5f * chord * FastCos(a);
                float cz = 0.07f * chord * FastSin(a) + 0.02f;
                return Vec3(cx * FastCos(twist) - cz * FastSin(twist), r, cx * FastSin(twist) + cz * FastCos(twist));
            },
            [&](float, float) {
                UvRect sw = Swatch(part == 0 ? SW_BLADE : SW_BLADE_TIP);
                return Vec2{sw.u0, sw.v0};
            },
            [](float, float) {
                return WHITE;
            },
            SurfaceOptions{true, true});
    }
    Append(mesh, blade, Place(Vec3(), 0.0f, 0.0f, angle));
}

static MeshData BuildPropeller(Livery livery)
{
    MeshData mesh;
    bool player = livery == Livery::PLAYER_RED;
    float radius = PropellerRadius(livery);
    int blades = player ? 3 : 4;
    for (int i = 0; i < blades; i++)
        AddBlade(mesh, radius, 0.2f, player ? 0.30f : 0.27f, i * 2.0f * PI / blades);
    MeshData spinner;
    std::vector<Vec2> profile;
    float length = player ? 0.62f : 0.42f;
    float base = player ? 0.40f : 0.30f;
    for (int i = 0; i <= 8; i++) {
        float k = static_cast<float>(i) / 8.0f;
        profile.push_back(Vec2{base * Sqrt(FastSin(k * PI * 0.5f)) * (1.0f - 0.05f * k), length * (1.0f - k)});
    }
    profile.push_back(Vec2{base, -0.08f});
    UvRect sw = Swatch(SW_SPINNER);
    AddLathe(spinner, profile, 16, sw, WHITE, false);
    Append(mesh, spinner, Place(Vec3(), 0.0f, -PI * 0.5f, 0.0f));
    return mesh;
}

MeshData MakePropeller()
{
    return BuildPropeller(Livery::PLAYER_RED);
}

MeshData MakePropeller(Livery livery)
{
    return BuildPropeller(livery);
}

MeshData MakePropellerDisc(Livery livery)
{
    MeshData mesh;
    float radius = PropellerRadius(livery) * 1.02f;
    constexpr int SEGMENTS = 32;
    for (int face = 0; face < 2; face++) {
        Vec3 n(0, 0, face == 0 ? -1.0f : 1.0f);
        int base = static_cast<int>(mesh.vertices.size());
        for (int i = 0; i <= SEGMENTS; i++) {
            float a = i * 2.0f * PI / SEGMENTS;
            for (int ring = 0; ring < 2; ring++) {
                float r = ring == 0 ? 0.18f : radius;
                float x = r * FastCos(a);
                float y = r * FastSin(a);
                AddVertex(mesh, Vec3(x, y, 0.0f), n, 0.5f + 0.5f * x / radius, 0.5f - 0.5f * y / radius, WHITE);
            }
        }
        for (int i = 0; i < SEGMENTS; i++) {
            int a = base + i * 2;
            if (face == 0)
                AddQuad(mesh, a, a + 1, a + 3, a + 2);
            else
                AddQuad(mesh, a, a + 2, a + 3, a + 1);
        }
    }
    return mesh;
}

MeshData MakePropellerDisc()
{
    return MakePropellerDisc(Livery::PLAYER_RED);
}

// --- Guns

Vec3 GunMuzzle(Livery livery, int index)
{
    float side = index == 0 ? -1.0f : 1.0f;
    if (livery == Livery::PLAYER_RED) {
        const WingDesc& w = SPITFIRE_WING;
        float x = 2.3f;
        float lead = w.lineZ - w.lineFrac * WingChord(w, x);
        return Vec3(side * x, WingY(w, x) + 0.01f, lead + 0.15f - 0.55f);
    }
    const WingDesc& w = PIRATE_WING;
    float x = 2.6f;
    float lead = w.lineZ + w.sweep * x - w.lineFrac * WingChord(w, x);
    return Vec3(side * x, WingY(w, x), lead + 0.1f - 0.6f) * PIRATE_SCALE;
}

Vec3 GunMuzzle(int index)
{
    return GunMuzzle(Livery::PLAYER_RED, index);
}

// --- Scarf

int ScarfSegmentCount()
{
    return SCARF_SEGMENTS;
}

float ScarfSegmentLength()
{
    return SCARF_SEGMENT_LENGTH;
}

Vec3 ScarfAnchor()
{
    Vec3 h = PilotHead(Airframe::SPITFIRE);
    return h + Vec3(0.0f, -0.17f, 0.15f) * HEAD_SCALE;
}

MeshData MakeScarfSegment(int index)
{
    MeshData mesh;
    UvRect r = PlaneRect(PT_SCARF);
    float v0 = static_cast<float>(index) / SCARF_SEGMENTS;
    float v1 = static_cast<float>(index + 1) / SCARF_SEGMENTS;
    float taper = 1.0f - 0.08f * index;
    for (int face = 0; face < 2; face++) {
        AddSurface(
            mesh, 3, 2,
            [&](float s, float t) {
                float w = SCARF_WIDTH * Lerp(taper, taper - 0.08f, t);
                float x = (s - 0.5f) * w;
                float curl = 0.025f * (1.0f - 4.0f * (s - 0.5f) * (s - 0.5f));
                return Vec3(x, curl, t * SCARF_SEGMENT_LENGTH);
            },
            [&](float s, float t) {
                return Vec2{r.u0 + s * (r.u1 - r.u0), r.v0 + Lerp(v0, v1, t) * (r.v1 - r.v0)};
            },
            [](float, float) {
                return WHITE;
            },
            SurfaceOptions{face == 1, false});
    }
    return mesh;
}

MeshData MakeScarf()
{
    MeshData mesh;
    Vec3 p = ScarfAnchor();
    Quat q = QuatAxisAngle(Vec3(1, 0, 0), 0.12f);
    for (int i = 0; i < SCARF_SEGMENTS; i++) {
        MeshData seg = MakeScarfSegment(i);
        Append(mesh, seg, MatRotate(q) * MatTranslate(p));
        p = p + Rotate(q, Vec3(0, 0, SCARF_SEGMENT_LENGTH));
        q = q * QuatAxisAngle(Vec3(1, 0, 0), 0.05f) * QuatAxisAngle(Vec3(0, 1, 0), i % 2 == 0 ? 0.18f : -0.22f) *
            QuatAxisAngle(Vec3(0, 0, 1), 0.25f);
    }
    return mesh;
}

} // namespace content
