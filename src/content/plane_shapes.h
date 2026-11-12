// Shapes shared by the plane meshes (planes.cpp) and their liveries (plane_textures.cpp): the fuselage and wing
// surfaces as functions, and the plane texture atlas layout, so the painter can find the 3D point behind each texel.
// Internal to src/content (docs/design.md §7).
#pragma once

#include "content/content.h"
#include "content/mesh_builder.h"
#include "content/fast_math.h"
#include "core/vmath.h"

namespace content {

// --- Plane texture atlas layout (256 x 256 pixels)

constexpr int PLANE_TEX = 256;
// Fuselage: u along the body (nose to tail), v around it (from the top, toward +X, under, up the left side).
constexpr AtlasRect PT_FUSELAGE = {0, 0, 256, 96};
// Wings: u from root to tip (linear in span), v from the leading edge to the trailing edge (linear in chord).
constexpr AtlasRect PT_WING_TOP = {0, 96, 128, 64};
constexpr AtlasRect PT_WING_BOTTOM = {128, 96, 128, 64};
constexpr AtlasRect PT_TAIL_TOP = {0, 160, 64, 32};
constexpr AtlasRect PT_TAIL_BOTTOM = {64, 160, 64, 32};
// Fin: u along the chord (linear, leading edge first), v from the tip (top) down to the root (linear).
constexpr AtlasRect PT_FIN = {128, 160, 64, 64};
// Pirate cowling ring: u around (from the top, toward +X), v from the back of the outer skin, over the lip, inside.
constexpr AtlasRect PT_COWL = {192, 160, 64, 64};
// Canopy glass: u around, v along; alpha ~0.35 with a reflection streak.
constexpr AtlasRect PT_GLASS = {192, 224, 64, 32};
// Scarf cloth: u across, v along the ribbon (fringe at v = 1).
constexpr AtlasRect PT_SCARF = {128, 224, 64, 32};
// Solid 16 x 16 swatches in an 8 x 4 grid at (0, 192).
constexpr int PT_SWATCH_X = 0;
constexpr int PT_SWATCH_Y = 192;

enum PlaneSwatch {
    SW_WHITE,       // white: the vertex colour shows as it is (pilot, details)
    SW_METAL,       // bare aluminium
    SW_DARK_METAL,  // gun barrels, engine
    SW_INK,         // near black
    SW_TIRE,        // rubber
    SW_BLADE,       // propeller blade
    SW_BLADE_TIP,   // propeller tip (yellow)
    SW_SPINNER,     // spinner (livery colour)
    SW_PRIMARY,     // livery main colour
    SW_SECONDARY,   // livery trim colour
    SW_PRIMARY_DARK,
    SW_EXHAUST,     // sooty rust
    SW_BRASS,
    SW_LEATHER,
    SW_GLASS_DARK,  // goggle lens
    SW_COUNT,
};

inline UvRect PlaneRect(const AtlasRect& r)
{
    return AtlasUv(r.x, r.y, r.w, r.h, PLANE_TEX);
}

inline UvRect PlaneSwatchUv(int swatch)
{
    return AtlasSwatch(PT_SWATCH_X + (swatch % 8) * 16 + 4, PT_SWATCH_Y + (swatch / 8) * 16 + 4, 8, 8, PLANE_TEX);
}

// --- Airframes

enum class Airframe {
    SPITFIRE,   // the player's plane
    PIRATE,     // radial-engine gull-wing fighter
    PIRATE_ACE, // the same with twin tail fins
};

inline Airframe AirframeOf(Livery livery)
{
    if (livery == Livery::PLAYER_RED)
        return Airframe::SPITFIRE;
    return livery == Livery::PIRATE_PURPLE ? Airframe::PIRATE_ACE : Airframe::PIRATE;
}

// Scale of the finished pirate meshes (built at player size, then enlarged to read at range).
constexpr float PIRATE_SCALE = 1.15f;

// --- Fuselage: cross-sections (superellipses) interpolated between keys along Z

struct FuselageKey {
    float z;
    float halfWidth;
    float top;
    float bottom;
};

struct FuselageDesc {
    const FuselageKey* keys;
    int count;
    float exponent; // superellipse exponent of the cross-section (2 = ellipse, higher = boxier)
};

constexpr FuselageKey SPITFIRE_KEYS[] = {
    {-4.35f, 0.38f, 0.40f, -0.28f}, {-4.00f, 0.45f, 0.48f, -0.38f}, {-3.20f, 0.49f, 0.54f, -0.48f},
    {-2.00f, 0.51f, 0.57f, -0.58f}, {-0.90f, 0.51f, 0.59f, -0.64f}, {0.20f, 0.49f, 0.60f, -0.62f},
    {1.30f, 0.42f, 0.60f, -0.52f},  {2.50f, 0.30f, 0.52f, -0.34f},  {3.60f, 0.18f, 0.45f, -0.12f},
    {4.40f, 0.09f, 0.42f, 0.06f},   {4.90f, 0.02f, 0.36f, 0.20f},
};

constexpr FuselageKey PIRATE_KEYS[] = {
    {-3.30f, 0.66f, 0.66f, -0.66f}, {-2.80f, 0.74f, 0.76f, -0.74f}, {-1.80f, 0.76f, 0.80f, -0.74f},
    {-0.60f, 0.72f, 0.82f, -0.70f}, {0.60f, 0.62f, 0.80f, -0.60f},  {1.80f, 0.44f, 0.66f, -0.38f},
    {2.80f, 0.24f, 0.52f, -0.10f},  {3.40f, 0.10f, 0.46f, 0.10f},   {3.70f, 0.02f, 0.40f, 0.24f},
};

inline FuselageDesc FuselageOf(Airframe a)
{
    if (a == Airframe::SPITFIRE)
        return FuselageDesc{SPITFIRE_KEYS, static_cast<int>(sizeof(SPITFIRE_KEYS) / sizeof(SPITFIRE_KEYS[0])), 2.3f};
    return FuselageDesc{PIRATE_KEYS, static_cast<int>(sizeof(PIRATE_KEYS) / sizeof(PIRATE_KEYS[0])), 2.1f};
}

// Cross-section at a fraction s (0 = front, 1 = tail) of the fuselage, Catmull-Rom between keys.
inline FuselageKey FuselageSection(const FuselageDesc& d, float s)
{
    float z = Lerp(d.keys[0].z, d.keys[d.count - 1].z, Clamp(s, 0.0f, 1.0f));
    int i = 0;
    while (i < d.count - 2 && z > d.keys[i + 1].z)
        i++;
    const FuselageKey& k1 = d.keys[i];
    const FuselageKey& k2 = d.keys[i + 1];
    const FuselageKey& k0 = d.keys[i > 0 ? i - 1 : i];
    const FuselageKey& k3 = d.keys[i + 2 < d.count ? i + 2 : i + 1];
    float t = Clamp((z - k1.z) / (k2.z - k1.z), 0.0f, 1.0f);
    float t2 = t * t;
    float t3 = t2 * t;
    float w0 = -0.5f * t3 + t2 - 0.5f * t;
    float w1 = 1.5f * t3 - 2.5f * t2 + 1.0f;
    float w2 = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
    float w3 = 0.5f * t3 - 0.5f * t2;
    FuselageKey r;
    r.z = z;
    r.halfWidth = k0.halfWidth * w0 + k1.halfWidth * w1 + k2.halfWidth * w2 + k3.halfWidth * w3;
    r.top = k0.top * w0 + k1.top * w1 + k2.top * w2 + k3.top * w3;
    r.bottom = k0.bottom * w0 + k1.bottom * w1 + k2.bottom * w2 + k3.bottom * w3;
    return r;
}

// Signed power for superellipses.
inline float SignedPow(float v, float e)
{
    return v < 0.0f ? -FastPow(-v, e) : FastPow(v, e);
}

// Point on the fuselage at fraction s along it and angle theta (0 = top, PI/2 = right side, PI = bottom).
inline Vec3 FuselagePoint(const FuselageDesc& d, float s, float theta)
{
    FuselageKey k = FuselageSection(d, s);
    float e = 2.0f / d.exponent;
    float c = SignedPow(FastCos(theta), e);
    float sn = SignedPow(FastSin(theta), e);
    float mid = (k.top + k.bottom) * 0.5f;
    float half = (k.top - k.bottom) * 0.5f;
    return Vec3(k.halfWidth * sn, mid + half * c, k.z);
}

// --- Wings (also tailplanes and fins): span along +X from rootX, chord along +Z, thickness along Y

struct WingDesc {
    float rootX;      // span start (inside the fuselage)
    float semiSpan;   // tip position
    float rootChord;
    float tipChord;   // chord where the rounded tip starts (taper)
    float ellipse;    // 1 = elliptical planform, 0 = straight taper with a rounded tip
    float lineZ;      // the chord point at lineFrac lies at z = lineZ + sweep * x
    float lineFrac;
    float sweep;
    float rootY;
    float dihedralIn;  // radians, from the root to breakX
    float dihedralOut; // radians, outboard of breakX (gull wings)
    float breakX;
    float thickRoot;   // thickness / chord at the root and tip
    float thickTip;
};

inline float WingSpanX(const WingDesc& w, float t)
{
    return w.rootX + (w.semiSpan - w.rootX) * FastSin(t * PI * 0.5f);
}

// Texture u of a span parameter t (linear in span, so the texel size is even), and back.
inline float WingSpanU(float t)
{
    return FastSin(t * PI * 0.5f);
}

inline float WingSpanT(float u)
{
    float c = Clamp(u, 0.0f, 1.0f);
    return FastAtan2(c, Sqrt(Max(1.0f - c * c, 0.0f))) * 2.0f / PI;
}

inline float WingChord(const WingDesc& w, float x)
{
    float u = Clamp(x / w.semiSpan, 0.0f, 1.0f);
    float elliptic = w.rootChord * Sqrt(Max(1.0f - u * u, 0.0f));
    float roundR = Min(w.tipChord * 0.7f, w.semiSpan * 0.3f);
    float taperEnd = w.semiSpan - roundR;
    float k = Clamp(x / taperEnd, 0.0f, 1.0f);
    float tapered = Lerp(w.rootChord, w.tipChord, k);
    if (x > taperEnd) {
        float q = (x - taperEnd) / roundR;
        tapered = w.tipChord * Sqrt(Max(1.0f - q * q, 0.0f));
    }
    return Lerp(tapered, elliptic, w.ellipse);
}

// Height of the chord plane at span x (dihedral, with a smooth gull break).
inline float WingY(const WingDesc& w, float x)
{
    float blend = 0.35f;
    float inner = x * tanf(w.dihedralIn);
    float atBreak = w.breakX * tanf(w.dihedralIn);
    float outer = atBreak + (x - w.breakX) * tanf(w.dihedralOut);
    float k = Clamp((x - (w.breakX - blend)) / (2.0f * blend), 0.0f, 1.0f);
    k = k * k * (3.0f - 2.0f * k);
    return w.rootY + Lerp(inner, outer, k);
}

// Point on the wing surface: span parameter t (0 root, 1 tip), chord fraction cf (0 LE, 1 TE), top or bottom.
inline Vec3 WingPoint(const WingDesc& w, float t, float cf, bool top)
{
    float x = WingSpanX(w, t);
    float chord = WingChord(w, x);
    float lead = w.lineZ + w.sweep * x - w.lineFrac * chord;
    float th = Lerp(w.thickRoot, w.thickTip, Clamp(x / w.semiSpan, 0.0f, 1.0f));
    float c = Clamp(cf, 0.0f, 1.0f);
    float yt = 5.0f * th * chord *
               (0.2969f * Sqrt(c) - 0.1260f * c - 0.3516f * c * c + 0.2843f * c * c * c - 0.1036f * c * c * c * c);
    float camber = 0.02f * chord * 4.0f * c * (1.0f - c);
    float y = WingY(w, x) + camber + (top ? yt : -yt);
    return Vec3(x, y, lead + c * chord);
}

// Chord fraction from a surface parameter (cosine spacing: fine at both edges).
inline float WingChordFrac(float s)
{
    return (1.0f - FastCos(s * PI)) * 0.5f;
}

constexpr WingDesc SPITFIRE_WING = {0.30f, 5.60f, 2.50f, 1.0f, 1.0f, -0.70f, 0.35f, 0.0f, -0.42f,
                                     6.0f * DEG, 6.0f * DEG, 2.0f, 0.13f, 0.07f};
constexpr WingDesc SPITFIRE_TAIL = {0.10f, 1.65f, 1.05f, 0.5f, 1.0f, 4.05f, 0.35f, 0.0f, 0.24f,
                                     0.0f, 0.0f, 1.0f, 0.10f, 0.07f};
constexpr WingDesc SPITFIRE_FIN = {0.0f, 1.55f, 1.35f, 0.6f, 0.75f, 4.25f, 0.45f, 0.10f, 0.0f,
                                    0.0f, 0.0f, 1.0f, 0.10f, 0.07f};
constexpr WingDesc PIRATE_WING = {0.40f, 5.50f, 2.30f, 1.35f, 0.0f, -0.55f, 0.30f, 0.04f, -0.40f,
                                   -16.0f * DEG, 9.0f * DEG, 1.75f, 0.15f, 0.09f};
constexpr WingDesc PIRATE_TAIL = {0.10f, 1.90f, 1.10f, 0.85f, 0.0f, 2.95f, 0.30f, 0.06f, 0.30f,
                                   0.0f, 0.0f, 1.0f, 0.10f, 0.08f};
constexpr WingDesc PIRATE_FIN = {0.0f, 1.35f, 1.40f, 0.95f, 0.3f, 3.05f, 0.40f, 0.30f, 0.0f,
                                  0.0f, 0.0f, 1.0f, 0.10f, 0.08f};

// --- Pilot and cockpit placement

// Centre of the pilot's head (plane-local, before the pirate scale).
inline Vec3 PilotHead(Airframe a)
{
    return a == Airframe::SPITFIRE ? Vec3(0.0f, 0.88f, 0.30f) : Vec3(0.0f, 1.08f, 0.45f);
}

} // namespace content
