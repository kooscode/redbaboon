// Plane liveries: paints the 256x256 atlas behind MakePlane's UVs (plane_shapes.h) for each livery. Every texel is
// traced back to its point on the fuselage, wing, tail or fin, and the livery is painted in metres there: cheat
// lines, roundels, insignia, nose art, numbers, hazard stripes, panel lines, exhaust stains and wear.
// docs/design.md §7, creative brief §2.
#include "content/content.h"
#include "content/emblems.h"
#include "content/noise.h"
#include "content/paint.h"
#include "content/plane_shapes.h"

#include <math.h>

namespace content {

// Size of one texel on the airframe in metres (for antialiasing painted shapes).
constexpr float TEXEL_M = 0.04f;

// Colours of one livery.
struct LiveryColors {
    Livery livery;
    Airframe airframe;
    Color primary;
    Color belly;
    Color trim;
    Color stripe; // hazard stripe or gold accent
    Color spinner;
    Color insignia;
};

static LiveryColors ColorsOf(Livery livery)
{
    LiveryColors c;
    c.livery = livery;
    c.airframe = AirframeOf(livery);
    if (livery == Livery::PLAYER_RED) {
        c.primary = Hex(PAL_CRIMSON);
        c.belly = Hex(0x9A0E18);
        c.trim = Hex(PAL_CREAM);
        c.stripe = Hex(PAL_BANANA);
        c.spinner = Hex(0xD9D4C7);
        c.insignia = Hex(PAL_CREAM);
    } else if (livery == Livery::PIRATE_PURPLE) {
        c.primary = Hex(PAL_PURPLE);
        c.belly = Hex(0x3E1C5C);
        c.trim = Hex(PAL_GOLD);
        c.stripe = Hex(PAL_GOLD);
        c.spinner = Hex(PAL_GOLD);
        c.insignia = Hex(0xF0D060);
    } else {
        c.primary = Hex(PAL_GUNMETAL);
        c.belly = Hex(0x59606A);
        c.trim = Hex(PAL_HAZARD);
        c.stripe = Hex(PAL_HAZARD);
        c.spinner = Hex(PAL_BONE);
        c.insignia = Hex(PAL_BONE);
    }
    return c;
}

// --- Shared painting helpers

// Darkens a 1-texel-wide line where |d| is under half a texel (d in metres).
static Color PanelLine(Color c, float d, float strength = 0.62f)
{
    if (Abs(d) >= TEXEL_M * 0.75f)
        return c;
    float k = Clamp(1.0f - Abs(d) / (TEXEL_M * 0.75f), 0.0f, 1.0f);
    return Mix(c, Shade(c, strength), k);
}

// Diagonal hazard stripes (stripe colour and ink) along a coordinate a, width w metres.
static Color Hazard(const Color& stripe, float a, float w)
{
    float f = a / w - Floor(a / w);
    return f < 0.5f ? stripe : Hex(PAL_INK);
}

// Grime and paint variation: multiplies by ~0.92..1.04 with a soft noise.
static Color Weather(Color c, float x, float y, uint32_t seed)
{
    float n = ValueNoise(x * 3.4f, y * 3.4f, seed);
    float speck = Hash2(static_cast<int>(x * 40.0f), static_cast<int>(y * 40.0f), seed + 9u);
    float k = 0.9f + 0.14f * n + (speck > 0.985f ? 0.08f : 0.0f);
    return Shade(c, k);
}

// Paints chunky text (cream fill, ink outline) centred at (cx, cy) in a 2D frame with x right and y down;
// unit = metres per font unit.
static Color PaintText(Color dst, Vec2 p, const char* text, Vec2 center, float unit, const Color& fill)
{
    float w = TextWidthUnits(text, 2.0f);
    Vec2 q = {(p.x - center.x) / unit + w * 0.5f, (p.y - center.y) / unit + 3.0f};
    float d = SdText(q, text, 2.0f);
    float aa = TEXEL_M / unit;
    dst = Over(dst, Hex(PAL_INK), Coverage(d - 1.35f, aa));
    return Over(dst, fill, Coverage(d - 0.8f, aa));
}

// --- Fuselage

static Color PaintPlayerFuselage(const LiveryColors& lc, const Vec3& p, float side, const FuselageKey& k)
{
    Color c = lc.primary;
    // Darker belly.
    float belly = SmoothStep(k.bottom + 0.35f, k.bottom + 0.1f, p.y);
    c = Mix(c, lc.belly, belly);
    // Cream nose ring behind the spinner.
    if (p.z < -4.18f)
        c = lc.trim;
    // Cheat line: a cream stripe along the side, ending in a point at the tail.
    float lineY = 0.30f + 0.04f * Clamp((p.z + 1.0f) / 4.0f, 0.0f, 1.0f);
    float half = Lerp(0.055f, 0.012f, Clamp((p.z + 4.2f) / 9.0f, 0.0f, 1.0f));
    float dLine = Abs(p.y - lineY) - half;
    c = Over(c, Hex(PAL_CRIMSON_SHADE), Coverage(dLine - 0.025f, TEXEL_M) * (p.z > -4.18f ? 1.0f : 0.0f));
    c = Over(c, lc.trim, Coverage(dLine, TEXEL_M) * (p.z > -4.18f && p.z < 4.6f ? 1.0f : 0.0f));
    // Exhaust stains streaking back from the stubs.
    if (p.z > -3.9f && p.z < -0.8f && Abs(p.x) > 0.2f) {
        float spread = 0.05f + 0.05f * (p.z + 3.9f);
        float dy = (p.y - 0.12f + 0.02f * (p.z + 3.9f)) / spread;
        float fade = Clamp(1.0f - (p.z + 3.9f) / 3.1f, 0.0f, 1.0f);
        float streak = 0.6f + 0.4f * Fbm(p.y * 30.0f, p.z * 2.0f, 2, 51u);
        c = Mix(c, Hex(0x2A2220), Clamp(FastExp(-dy * dy) * fade * streak * 0.85f, 0.0f, 1.0f));
    }
    // Nose number "01" low on the cowling, reading from tail to nose on each side.
    Vec2 tp = {side > 0.0f ? -p.z : p.z, -p.y};
    Vec2 center = {side > 0.0f ? 3.45f : -3.45f, 0.17f};
    if (Abs(tp.x - center.x) < 0.5f && Abs(tp.y - center.y) < 0.3f)
        c = PaintText(c, tp, "01", center, 0.045f, lc.trim);
    // Nose art: the baboon badge under the windscreen.
    Vec2 bp = {(tp.x - (side > 0.0f ? 1.85f : -1.85f)) / 0.27f, (-p.y - 0.0f) / 0.27f};
    if (bp.x * bp.x + bp.y * bp.y < 1.6f) {
        c = Over(c, Hex(PAL_INK), Coverage(Sqrt(bp.x * bp.x + bp.y * bp.y) - 1.12f, TEXEL_M / 0.27f));
        c = Over(c, lc.trim, Coverage(Sqrt(bp.x * bp.x + bp.y * bp.y) - 1.05f, TEXEL_M / 0.27f));
        c = PaintBaboonFace(c, Vec2{bp.x, bp.y + 0.05f}, TEXEL_M / 0.27f);
    }
    // Wear along the cockpit sill.
    if (p.z > -0.8f && p.z < 1.3f && p.y > k.top - 0.12f)
        c = Mix(c, Hex(0xD8A0A0), 0.35f * Fbm(p.x * 20.0f, p.z * 20.0f, 2, 7u));
    return c;
}

static Color PaintPirateFuselage(const LiveryColors& lc, const Vec3& p, float side, const FuselageKey& k)
{
    bool ace = lc.airframe == Airframe::PIRATE_ACE;
    Color c = lc.primary;
    c = Mix(c, lc.belly, SmoothStep(k.bottom + 0.45f, k.bottom + 0.1f, p.y));
    // Nose band right behind the cowling.
    if (p.z < -2.95f)
        c = ace ? lc.trim : Hazard(lc.stripe, p.z * 0.7f + FastAtan2(p.x, p.y) * 0.25f, 0.18f);
    // Hazard (or gold) band behind the cockpit and on the tail cone.
    if ((p.z > 1.25f && p.z < 1.75f) || (p.z > 2.75f && p.z < 3.05f))
        c = ace ? lc.trim : Hazard(lc.stripe, p.z + p.y, 0.2f);
    // A yellow (gold) stripe along the spine, so the top view reads at range.
    if (p.y > k.top - 0.16f && p.z > -2.95f && p.z < 3.3f)
        c = lc.stripe;
    // Ace: a gold cheat line with an ink pinstripe.
    if (ace) {
        float dLine = Abs(p.y - 0.22f) - 0.04f;
        c = Over(c, Hex(PAL_INK), Coverage(dLine - 0.02f, TEXEL_M) * (p.z > -2.95f ? 1.0f : 0.0f));
        c = Over(c, lc.trim, Coverage(dLine, TEXEL_M) * (p.z > -2.95f ? 1.0f : 0.0f));
    }
    // Exhaust stains below the stubs.
    if (p.z > -2.9f && p.z < -0.4f && Abs(p.x) > 0.3f) {
        float spread = 0.08f + 0.06f * (p.z + 2.9f);
        float dy = (p.y + 0.42f) / spread;
        float fade = Clamp(1.0f - (p.z + 2.9f) / 2.5f, 0.0f, 1.0f);
        c = Mix(c, Hex(0x1E1A18), Clamp(FastExp(-dy * dy) * fade * 0.8f, 0.0f, 1.0f));
    }
    // Insignia on both sides, forward of the cockpit.
    Vec2 tp = {side > 0.0f ? -p.z : p.z, -p.y};
    Vec2 ip = {(tp.x - (side > 0.0f ? 1.25f : -1.25f)) / 0.42f, (tp.y + 0.12f) / 0.42f};
    if (Abs(ip.x) < 1.6f && Abs(ip.y) < 1.6f)
        c = PaintPirateInsignia(c, ip, TEXEL_M / 0.42f, lc.insignia);
    return c;
}

static void PaintFuselage(Canvas& canvas, const LiveryColors& lc)
{
    FuselageDesc d = FuselageOf(lc.airframe);
    const AtlasRect& r = PT_FUSELAGE;
    bool player = lc.airframe == Airframe::SPITFIRE;
    static const float PLAYER_PANELS[] = {-4.18f, -3.0f, -1.55f, 1.45f, 2.6f, 3.7f};
    static const float PIRATE_PANELS[] = {-2.95f, -1.9f, 1.25f, 1.75f, 2.4f, 3.05f};
    const float* panels = player ? PLAYER_PANELS : PIRATE_PANELS;
    // Cross-sections per column and superellipse terms per row (FuselagePoint, split for speed).
    FuselageKey sections[PT_FUSELAGE.w];
    for (int x = 0; x < r.w; x++)
        sections[x] = FuselageSection(d, (x + 0.5f) / r.w);
    float e = 2.0f / d.exponent;
    for (int y = 0; y < r.h; y++) {
        float theta = (y + 0.5f) / r.h * 2.0f * PI;
        float cs = SignedPow(FastCos(theta), e);
        float sn = SignedPow(FastSin(theta), e);
        for (int x = 0; x < r.w; x++) {
            const FuselageKey& k = sections[x];
            Vec3 p(k.halfWidth * sn, (k.top + k.bottom) * 0.5f + (k.top - k.bottom) * 0.5f * cs, k.z);
            float side = p.x >= 0.0f ? 1.0f : -1.0f;
            Color c = player ? PaintPlayerFuselage(lc, p, side, k) : PaintPirateFuselage(lc, p, side, k);
            for (int i = 0; i < 6; i++)
                c = PanelLine(c, p.z - panels[i]);
            c = PanelLine(c, (theta - PI * 0.5f) * 0.5f, 0.75f);
            c = PanelLine(c, (theta - PI * 1.5f) * 0.5f, 0.75f);
            c = Weather(c, p.z, theta * 0.5f, 3u);
            canvas.At(r.x + x, r.y + y) = c;
        }
    }
}

// --- Wings, tailplanes and fins

// Paints one wing-like region. kind: 0 wing, 1 tailplane, 2 fin. top: upper surface.
static void PaintWing(Canvas& canvas, const LiveryColors& lc, const AtlasRect& r, const WingDesc& w, int kind,
                      bool top)
{
    bool player = lc.airframe == Airframe::SPITFIRE;
    bool ace = lc.airframe == Airframe::PIRATE_ACE;
    for (int y = 0; y < r.h; y++) {
        for (int x = 0; x < r.w; x++) {
            float t = 0.0f;
            float cf = 0.0f;
            if (kind == 2) {
                cf = (x + 0.5f) / r.w;
                t = WingSpanT(1.0f - (y + 0.5f) / r.h);
            } else {
                t = WingSpanT((x + 0.5f) / r.w);
                cf = (y + 0.5f) / r.h;
            }
            Vec3 p = WingPoint(w, t, cf, top);
            float span = p.x;
            float tipFrac = span / w.semiSpan;
            Color c = top || kind == 2 ? lc.primary : lc.belly;
            if (kind == 2 && ace) {
                // Ace fins are ellipsoids: v is the height from the top.
                c = (y + 0.5f) / r.h < 0.22f ? lc.trim : lc.primary;
                if (Abs((y + 0.5f) / r.h - 0.24f) < 0.02f)
                    c = Hex(PAL_INK);
                canvas.At(r.x + x, r.y + y) = Weather(c, x * 0.1f, y * 0.1f, 5u);
                continue;
            }
            // Pirates carry a lot of hazard yellow (gold on aces) on top so they read at range (review 1 §1):
            // a yellow leading edge, a broad band inboard, and bigger striped tips.
            if (!player && kind == 0 && top) {
                if (cf < 0.2f)
                    c = lc.stripe;
                if (span > 1.6f && span < 2.7f)
                    c = Abs(span - 1.65f) < 0.08f || Abs(span - 2.65f) < 0.08f ? Hex(PAL_INK) : lc.stripe;
            }
            if (!player && kind == 1 && top)
                c = ace ? lc.trim : Hazard(lc.stripe, span + p.z, 0.3f);
            // Tips: cream (player), gold (ace) or hazard stripes (pirate).
            float tipStart = kind == 0 ? (player ? 0.86f : 0.74f) : 0.8f;
            if (tipFrac > tipStart) {
                if (player || ace)
                    c = lc.trim;
                else
                    c = Hazard(lc.stripe, span + p.z, 0.22f);
            }
            if (kind == 2 && player && cf > 0.64f) {
                // Fin flash on the rudder: cream, banana yellow, crimson.
                c = cf < 0.76f ? lc.trim : (cf < 0.88f ? lc.stripe : lc.primary);
            } else if (kind == 2 && !player && cf > 0.62f) {
                c = Hazard(lc.stripe, span * 0.8f + p.z, 0.2f);
            }
            // Worn leading edge and wing root walkway.
            if (kind == 0 && cf < 0.07f) {
                float wear = Fbm(span * 6.0f, cf * 40.0f, 3, 21u);
                c = Mix(c, Hex(0xC8C4BC), Clamp((0.07f - cf) / 0.07f * wear * 1.2f, 0.0f, 0.75f));
            }
            if (kind == 0 && top && span < 0.85f)
                c = Mix(c, Hex(0x2A2624), 0.55f);
            // Emblems on the main wings.
            if (kind == 0) {
                float chord = WingChord(w, 3.4f);
                float lead = w.lineZ + w.sweep * 3.4f - w.lineFrac * chord;
                Vec2 center = {3.4f, lead + chord * 0.45f};
                float radius = player ? 0.62f : 0.58f;
                Vec2 q = {(p.z - center.y) / radius, (span - center.x) / radius};
                if (q.x * q.x + q.y * q.y < 2.5f) {
                    if (player)
                        c = PaintRoundel(c, q, TEXEL_M / radius);
                    else
                        c = PaintPirateInsignia(c, Vec2{-q.y, q.x}, TEXEL_M / radius, lc.insignia);
                }
            }
            // Panel lines: spars and ribs, the control surface hinge, gun ports.
            if (kind == 0) {
                c = PanelLine(c, (cf - 0.25f) * 2.0f);
                c = PanelLine(c, (cf - 0.74f) * 2.0f, 0.5f);
                for (int i = 1; i <= 4; i++)
                    c = PanelLine(c, span - i * 1.05f);
                if (player) {
                    for (int g = 0; g < 2; g++) {
                        float gx = 2.3f + g * 0.35f;
                        if (!top)
                            continue;
                        c = Over(c, Hex(PAL_INK), Coverage(Sqrt((span - gx) * (span - gx) + cf * cf * 4.0f) -
                                                           0.06f, TEXEL_M));
                    }
                }
            } else {
                c = PanelLine(c, (cf - (kind == 2 ? 0.64f : 0.68f)) * 1.5f, 0.5f);
            }
            canvas.At(r.x + x, r.y + y) = Weather(c, span, p.z, 11u + kind);
        }
    }
}

// --- Small regions: cowling, glass, scarf, swatches

static void PaintCowl(Canvas& canvas, const LiveryColors& lc)
{
    const AtlasRect& r = PT_COWL;
    bool ace = lc.airframe == Airframe::PIRATE_ACE;
    Color bone = ace ? Hex(PAL_GOLD) : Hex(PAL_BONE);
    for (int y = 0; y < r.h; y++) {
        float v = (y + 0.5f) / r.h;
        for (int x = 0; x < r.w; x++) {
            float u = (x + 0.5f) / r.w;
            float theta = u * 2.0f * PI;
            Color c = bone;
            float a = theta > PI ? theta - 2.0f * PI : theta;
            if (v > 0.6f) {
                c = Hex(0x23272D);
            } else {
                // Skull face around the ring: eye sockets high on both sides, a toothy grin underneath.
                Vec2 q = {a, v};
                float eye = Min(SdEllipse(q, Vec2{1.0f, 0.3f}, Vec2{0.42f, 0.16f}),
                                  SdEllipse(q, Vec2{-1.0f, 0.3f}, Vec2{0.42f, 0.16f}));
                c = Over(c, Hex(PAL_INK), Coverage(eye, 0.05f));
                c = Over(c, Hex(0xFF3B2F), Coverage(Min(SdCircle(q, Vec2{1.0f, 0.33f}, 0.06f),
                                                          SdCircle(q, Vec2{-1.0f, 0.33f}, 0.06f)), 0.05f));
                float b = Abs(a) - PI;
                b = Abs(b);
                float mouth = Max(Abs(v - 0.36f) - 0.2f * Clamp((1.5f - b) / 0.4f, 0.0f, 1.0f), b - 1.5f);
                if (mouth < 0.03f) {
                    // Teeth: triangles hanging from the lip and standing up from the jaw.
                    Color m = Hex(PAL_INK);
                    float k = b / 0.3f;
                    float upper = (v - 0.36f) / 0.2f;
                    float tooth = Abs(k - Floor(k) - 0.5f) * 2.0f;
                    float toothLow = Abs(k + 0.5f - Floor(k + 0.5f) - 0.5f) * 2.0f;
                    float dUp = Max(tooth * 0.9f + 0.05f - upper, -upper);
                    float dLow = Max(toothLow * 0.9f + 0.1f + upper, upper);
                    m = Over(m, bone, Coverage(Min(dUp, dLow) * 0.25f, 0.025f));
                    c = Over(c, m, Coverage(mouth, 0.025f));
                }
                if (v < 0.04f)
                    c = Hex(PAL_INK);
            }
            canvas.At(r.x + x, r.y + y) = Weather(c, u * 4.0f, v, 13u);
        }
    }
}

static void PaintGlass(Canvas& canvas)
{
    const AtlasRect& r = PT_GLASS;
    for (int y = 0; y < r.h; y++) {
        float v = (y + 0.5f) / r.h;
        for (int x = 0; x < r.w; x++) {
            float u = (x + 0.5f) / r.w;
            float s1 = (u * 3.0f + v * 1.2f - 1.1f) / 0.12f;
            float s2 = (u * 3.0f + v * 1.2f - 1.45f) / 0.05f;
            float streak = FastExp(-s1 * s1) * 0.55f + FastExp(-s2 * s2) * 0.35f;
            float a = 0.22f + 0.18f * v + streak;
            Color c = Mix(Hex(0x9CC8DC), Hex(0xF5F2EA), Clamp(streak * 1.5f, 0.0f, 1.0f));
            c.a = Clamp(a, 0.0f, 0.85f);
            canvas.At(r.x + x, r.y + y) = c;
        }
    }
}

static void PaintScarf(Canvas& canvas)
{
    const AtlasRect& r = PT_SCARF;
    for (int y = 0; y < r.h; y++) {
        float v = (y + 0.5f) / r.h;
        for (int x = 0; x < r.w; x++) {
            float u = (x + 0.5f) / r.w;
            Color c = Shade(Hex(PAL_CRIMSON), 0.85f + 0.2f * FastSin(u * PI * 3.0f + v * 9.0f));
            if (v > 0.84f && v < 0.88f)
                c = Hex(PAL_CREAM);
            if (v > 0.93f)
                c = (x / 3) % 2 == 0 ? Hex(PAL_CREAM) : Hex(0xA00E1A);
            canvas.At(r.x + x, r.y + y) = c;
        }
    }
}

static void PaintSwatches(Canvas& canvas, const LiveryColors& lc)
{
    Color colors[SW_COUNT];
    colors[SW_WHITE] = Hex(0xFFFFFF);
    colors[SW_METAL] = Hex(0xB8BCC2);
    colors[SW_DARK_METAL] = Hex(0x34363A);
    colors[SW_INK] = Hex(PAL_INK);
    colors[SW_TIRE] = Hex(0x262626);
    colors[SW_BLADE] = Hex(0x3A3330);
    colors[SW_BLADE_TIP] = Hex(PAL_BANANA);
    colors[SW_SPINNER] = lc.spinner;
    colors[SW_PRIMARY] = lc.primary;
    colors[SW_SECONDARY] = lc.trim;
    colors[SW_PRIMARY_DARK] = Shade(lc.primary, 0.6f);
    colors[SW_EXHAUST] = Hex(0x5A3A2A);
    colors[SW_BRASS] = Hex(PAL_BRASS);
    colors[SW_LEATHER] = Hex(0x6B4423);
    colors[SW_GLASS_DARK] = Hex(0x1E3A4A);
    for (int i = 0; i < SW_COUNT; i++) {
        int x0 = PT_SWATCH_X + (i % 8) * 16;
        int y0 = PT_SWATCH_Y + (i / 8) * 16;
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++)
                canvas.At(x0 + x, y0 + y) = colors[i];
        }
    }
}

// --- Textures

ImageData MakePlaneTexture(Livery livery)
{
    LiveryColors lc = ColorsOf(livery);
    Canvas canvas(PLANE_TEX, PLANE_TEX, Hex(0xFFFFFF));
    PaintFuselage(canvas, lc);
    bool player = livery == Livery::PLAYER_RED;
    const WingDesc& wing = player ? SPITFIRE_WING : PIRATE_WING;
    const WingDesc& tail = player ? SPITFIRE_TAIL : PIRATE_TAIL;
    const WingDesc& fin = player ? SPITFIRE_FIN : PIRATE_FIN;
    PaintWing(canvas, lc, PT_WING_TOP, wing, 0, true);
    PaintWing(canvas, lc, PT_WING_BOTTOM, wing, 0, false);
    PaintWing(canvas, lc, PT_TAIL_TOP, tail, 1, true);
    PaintWing(canvas, lc, PT_TAIL_BOTTOM, tail, 1, false);
    PaintWing(canvas, lc, PT_FIN, fin, 2, true);
    PaintCowl(canvas, lc);
    PaintGlass(canvas);
    PaintScarf(canvas);
    PaintSwatches(canvas, lc);
    return canvas.ToImage();
}

ImageData MakePropellerTexture()
{
    // A soft translucent blur: alpha 0.35 at the hub fading radially to 0 at the rim, no outline, with two faint
    // blade ghosts (review 1 §10).
    constexpr int SIZE = 128;
    Canvas canvas(SIZE, SIZE, Color{0.23f, 0.2f, 0.19f, 0.0f});
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            float px = (x + 0.5f) / SIZE * 2.0f - 1.0f;
            float py = (y + 0.5f) / SIZE * 2.0f - 1.0f;
            float r = Sqrt(px * px + py * py);
            if (r >= 1.0f)
                continue;
            float ang = FastAtan2(py, px);
            float fade = 1.0f - SmoothStep(0.15f, 1.0f, r);
            float ghost = FastPow(Abs(FastCos(ang - r * 0.6f)), 16.0f) * 0.12f;
            Color c = Color{0.23f, 0.2f, 0.19f, 0.0f};
            c.a = Clamp((0.35f + ghost) * fade * SmoothStep(0.05f, 0.18f, r), 0.0f, 1.0f);
            canvas.At(x, y) = c;
        }
    }
    return canvas.ToImage();
}

} // namespace content
