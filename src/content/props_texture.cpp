// The props texture atlas (props_layout.h): palm fronds and banana leaves (cut-outs), bark, terracotta roof tiles,
// whitewashed walls, the supply crate, planks, rock, the pirates' flag, banana skin and solid swatches.
// docs/design.md §7; creative brief §1.
#include "content/content.h"
#include "content/emblems.h"
#include "content/noise.h"
#include "content/paint.h"
#include "content/props_layout.h"

#include <math.h>

namespace content {

// Calls paint(local x 0..1, local y 0..1, pixel size) for every pixel of an atlas rectangle.
template <typename F>
static void FillRect(Canvas& canvas, const AtlasRect& r, F paint)
{
    for (int y = 0; y < r.h; y++) {
        for (int x = 0; x < r.w; x++) {
            Vec2 p = {(x + 0.5f) / r.w, (y + 0.5f) / r.h};
            canvas.At(r.x + x, r.y + y) = paint(p, canvas.At(r.x + x, r.y + y));
        }
    }
}

// --- Foliage

// One frond as a distance: a tapered blade around a curve from a to the tip, with notches along its edges.
static float FrondDistance(Vec2 p, Vec2 a, Vec2 mid, Vec2 tip, float width)
{
    // Two segments approximate the arch; the blade narrows to the tip.
    float d1 = SdSegment(p, a, mid);
    float d2 = SdSegment(p, mid, tip);
    float tipDist = Sqrt((p.x - tip.x) * (p.x - tip.x) + (p.y - tip.y) * (p.y - tip.y));
    float total = Sqrt((tip.x - a.x) * (tip.x - a.x) + (tip.y - a.y) * (tip.y - a.y)) + 1e-3f;
    float taper = Clamp(tipDist / total, 0.0f, 1.0f);
    float notch = 0.25f * width * (0.5f + 0.5f * FastSin((1.0f - taper) * 40.0f));
    return Min(d1, d2) - (width * (0.25f + 0.75f * taper) - notch);
}

// Frond colour: deep green at the base to bright tips (review 2 §4), lighter along the rib.
static Color FrondColor(float along, float d, float width)
{
    Color c = Mix(Hex(0x2F7A2E), Hex(0x7BB043), Clamp(along, 0.0f, 1.0f));
    c = Mix(c, Hex(0xA8C870), Clamp(1.0f + d / (width * 0.3f), 0.0f, 1.0f) * 0.4f);
    c.a = 1.0f;
    return c;
}

// Crown from the side: fronds arching out and drooping to both sides from the top of the trunk (the centre of the
// top edge region), solid blades so the cut-out keeps its shape in the smaller mipmaps.
static Color CrownSideTexel(Vec2 p, Color)
{
    // The card is twice as wide as tall: work in units where x spans -1..1 and y 0..1 (down).
    Vec2 q = {p.x * 2.0f - 1.0f, p.y};
    Vec2 a = {0.0f, 0.3f};
    float best = 1e9f;
    float along = 0.0f;
    static const float SPREAD[5] = {0.28f, 0.55f, 0.8f, 0.95f, 1.0f};
    static const float DROOP[5] = {-0.2f, 0.05f, 0.35f, 0.62f, 0.85f};
    for (int side = -1; side <= 1; side += 2) {
        for (int k = 0; k < 5; k++) {
            Vec2 tip = {side * SPREAD[k] * 0.97f, 0.3f + DROOP[k] * 0.7f};
            Vec2 mid = {side * SPREAD[k] * 0.55f, 0.3f + DROOP[k] * 0.15f - 0.12f};
            float d = FrondDistance(q, a, mid, tip, 0.065f);
            if (d < best) {
                best = d;
                along = Abs(q.x) / Max(SPREAD[k], 0.01f);
            }
        }
    }
    float up = FrondDistance(q, a, Vec2{0.05f, 0.12f}, Vec2{0.12f, 0.02f}, 0.05f);
    if (up < best) {
        best = up;
        along = (0.3f - q.y) / 0.3f;
    }
    if (best > 0.0f)
        return Color{0.18f, 0.48f, 0.18f, 0.0f};
    return FrondColor(along, best, 0.065f);
}

// Crown from above: seven fronds radiating from the centre.
static Color CrownTopTexel(Vec2 p, Color)
{
    Vec2 q = {p.x * 2.0f - 1.0f, p.y * 2.0f - 1.0f};
    float best = 1e9f;
    float along = 0.0f;
    for (int k = 0; k < 7; k++) {
        float ang = k * 2.0f * PI / 7.0f + 0.3f;
        Vec2 dir = {FastCos(ang), FastSin(ang)};
        Vec2 side = {-dir.y, dir.x};
        Vec2 tip = {dir.x * 0.95f + side.x * 0.12f, dir.y * 0.95f + side.y * 0.12f};
        Vec2 mid = {dir.x * 0.5f + side.x * 0.1f, dir.y * 0.5f + side.y * 0.1f};
        float d = FrondDistance(q, Vec2{0.0f, 0.0f}, mid, tip, 0.11f);
        if (d < best) {
            best = d;
            along = Sqrt(q.x * q.x + q.y * q.y);
        }
    }
    if (best > 0.0f)
        return Color{0.18f, 0.48f, 0.18f, 0.0f};
    return FrondColor(along, best, 0.11f);
}

// Canyon strata: wavy beds of three rock colours (twelve per tile), cracks and grit; tiles in both directions.
static Color StrataTexel(Vec2 p, Color)
{
    float wobble = 0.03f * (ValueNoise(p.x * 3.0f, p.y * 3.0f, 541u, 3) - 0.5f) + 0.01f * FastSin(p.x * 2.0f * PI);
    float y = (p.y + wobble) * 12.0f;
    int band = static_cast<int>(Floor(y));
    float f = y - Floor(y);
    static const uint32_t STRATA[3] = {0x7A5E4E, 0x6A4F42, 0x8A6E58};
    Color c = Hex(STRATA[(band % 3 + 3) % 3]);
    // Darker seams between beds, lighter tops, grit.
    if (f > 0.9f)
        c = Shade(c, 0.7f);
    else if (f < 0.12f)
        c = Shade(c, 1.12f);
    float grit = ValueNoise(p.x * 32.0f, p.y * 32.0f, 543u, 32);
    float crack = ValueNoise(p.x * 16.0f, p.y * 4.0f, 547u, 4);
    c = Shade(c, 0.9f + 0.18f * grit - (crack > 0.82f ? 0.18f : 0.0f));
    c.a = 1.0f;
    return c;
}

// Banana plant leaf: a broad blade with a pale midrib, side veins and a few tears.
static Color BananaLeafTexel(Vec2 p, Color)
{
    float t = 1.0f - p.y;
    float half = 0.44f * Sqrt(Max(FastSin(Min(t * 1.05f, 1.0f) * PI), 0.0f));
    float dx = Abs(p.x - 0.5f);
    float tear = 0.0f;
    float k = t * 9.0f;
    if (Abs(k - Floor(k) - 0.5f) < 0.06f && (static_cast<int>(k) % 3 == 1))
        tear = 1.0f;
    if (dx > half || (tear > 0.0f && dx > 0.08f))
        return Color{0.35f, 0.6f, 0.25f, 0.0f};
    Color c = Mix(Hex(0x3E8A2E), Hex(0x6AAA3E), Clamp(dx / 0.44f + 0.15f * Fbm(p.x * 10.0f, p.y * 10.0f, 2, 503u),
                                                        0.0f, 1.0f));
    float vein = Abs(((p.y + dx * 0.6f) * 22.0f) - Floor((p.y + dx * 0.6f) * 22.0f) - 0.5f);
    if (vein < 0.08f)
        c = Shade(c, 0.88f);
    if (dx < 0.025f)
        c = Hex(0xC8D890);
    c.a = 1.0f;
    return c;
}

static Color TrunkTexel(Vec2 p, Color)
{
    float ring = p.y * 16.0f - Floor(p.y * 16.0f);
    float n = Fbm(p.x * 8.0f, p.y * 40.0f, 3, 507u, 8);
    Color c = Mix(Hex(0x7A6A52), Hex(0xA08E70), n);
    if (ring < 0.18f)
        c = Shade(c, 0.6f + ring * 2.0f);
    return c;
}

// --- Buildings

static Color RoofTexel(Vec2 p, Color)
{
    float rows = p.y * 8.0f;
    int row = static_cast<int>(rows);
    float fy = rows - row;
    float cols = p.x * 16.0f + (row % 2) * 0.5f;
    int col = static_cast<int>(cols);
    float fx = cols - col;
    // Light tiles: the roof's colour comes from the vertex colour (terracotta, blue, green, ochre...).
    float var = Hash2(col, row, 509u);
    Color c = Shade(Hex(0xF4EEE6), 0.86f + 0.14f * var);
    // Barrel tiles: lit crown, shaded valleys, a shadow line under each row.
    c = Shade(c, 0.8f + 0.3f * FastSin(fx * PI));
    if (fy > 0.82f)
        c = Shade(c, 0.6f);
    return c;
}

static Color WallTexel(Vec2 p, Color)
{
    float n = Fbm(p.x * 12.0f, p.y * 8.0f, 3, 511u);
    Color c = Shade(Hex(0xF1E8D6), 0.92f + 0.1f * n);
    // Dirt rising from the ground.
    c = Mix(c, Hex(0xB09A78), SmoothStep(0.75f, 1.0f, p.y) * 0.6f);
    // Door (left), windows with shutters (right).
    float px = p.x * 96.0f;
    float py = p.y * 64.0f;
    if (px > 12.0f && px < 34.0f && py > 20.0f) {
        c = Hex(0x7A4A2A);
        if (px < 14.0f || px > 32.0f || py < 22.0f)
            c = Hex(0x4A2A1A);
        float plank = Abs((px - 12.0f) / 5.5f - Floor((px - 12.0f) / 5.5f) - 0.5f);
        if (plank > 0.44f)
            c = Shade(c, 0.75f);
    }
    for (int w = 0; w < 2; w++) {
        float wx = 52.0f + w * 24.0f;
        if (px > wx - 8.0f && px < wx + 8.0f && py > 16.0f && py < 38.0f) {
            c = Hex(0x1E2A30);
            if (Abs(px - wx) < 0.8f || Abs(py - 27.0f) < 0.8f)
                c = Hex(0xF1E8D6);
        }
        if ((px > wx - 13.0f && px < wx - 8.0f) || (px > wx + 8.0f && px < wx + 13.0f)) {
            if (py > 16.0f && py < 38.0f)
                c = Mix(Hex(0x2F7A6A), Hex(0x3F8A7A), (static_cast<int>(py) % 3) == 0 ? 0.0f : 1.0f);
        }
    }
    return c;
}

// --- Materials

static Color PlanksTexel(Vec2 p, Color)
{
    float rows = p.y * 6.0f;
    int row = static_cast<int>(rows);
    float fy = rows - row;
    float grain = Fbm(p.x * 3.0f + row * 7.0f, fy * 2.0f + row, 3, 513u, 3);
    Color c = Mix(Hex(0x8A6A48), Hex(0xB89068), grain);
    c = Shade(c, 0.9f + 0.15f * Hash2(row, 0, 515u));
    if (fy < 0.08f)
        c = Shade(c, 0.5f);
    return c;
}

static Color RockTexel(Vec2 p, Color)
{
    float n = Fbm(p.x * 6.0f, p.y * 6.0f, 4, 517u, 6);
    float f1 = 0.0f;
    float f2 = 0.0f;
    Cellular(p.x * 5.0f, p.y * 5.0f, 519u, 5, f1, f2);
    Color c = Mix(Hex(0x6A5E56), Hex(0xA89A8C), n);
    if (f2 - f1 < 0.05f)
        c = Shade(c, 0.6f);
    return c;
}

static Color CrateTexel(Vec2 p, Color)
{
    float aa = 1.0f / 64.0f;
    Color c = PlanksTexel(Vec2{p.x, p.y * 0.66f}, Color{});
    c = Shade(Mix(c, Hex(0xC8A070), 0.3f), 1.05f);
    // Frame, cross brace, the banana logo on a cream disc.
    float frame = Min(Min(p.x, 1.0f - p.x), Min(p.y, 1.0f - p.y));
    if (frame < 0.1f)
        c = Shade(Hex(0x7A5230), 0.9f + frame);
    Vec2 q = {(p.x - 0.5f) * 2.0f, (p.y - 0.5f) * 2.0f};
    float disc = Sqrt(q.x * q.x + q.y * q.y);
    c = Over(c, Hex(PAL_INK), Coverage(disc - 0.62f, aa * 2.0f));
    c = Over(c, Hex(PAL_CREAM), Coverage(disc - 0.56f, aa * 2.0f));
    return PaintBanana(c, Vec2{q.x * 2.1f, q.y * 2.1f + 0.15f}, aa * 4.2f);
}

static Color FlagTexel(Vec2 p, Color)
{
    float aa = 1.0f / 64.0f;
    // Frayed fly end (right edge) as a cut-out.
    float fray = 0.9f + 0.08f * FastSin(p.y * 40.0f) * FastSin(p.y * 13.0f);
    if (p.x > fray)
        return Color{0.1f, 0.1f, 0.1f, 0.0f};
    Color c = Shade(Hex(0x1A1A1A), 0.9f + 0.2f * Fbm(p.x * 6.0f, p.y * 6.0f, 2, 521u));
    Vec2 q = {(p.x - 0.47f) * 2.4f, (p.y - 0.5f) * 2.4f};
    c = PaintPirateInsignia(c, q, aa * 2.4f, Hex(PAL_BONE));
    if (p.y < 0.06f || p.y > 0.94f)
        c = Hex(PAL_HAZARD);
    c.a = 1.0f;
    return c;
}

// The harbour's flag: crimson and banana-yellow stripes with a cream disc and a banana.
static Color TownFlagTexel(Vec2 p, Color)
{
    float aa = 1.0f / 32.0f;
    Color c = static_cast<int>(p.y * 5.0f) % 2 == 0 ? Hex(PAL_CRIMSON) : Hex(PAL_BANANA);
    Vec2 q = {(p.x - 0.32f) * 4.0f, (p.y - 0.5f) * 2.0f};
    float d = Sqrt(q.x * q.x + q.y * q.y);
    c = Over(c, Hex(PAL_INK), Coverage(d - 0.82f, aa * 4.0f));
    c = Over(c, Hex(PAL_CREAM), Coverage(d - 0.72f, aa * 4.0f));
    c = PaintBanana(c, Vec2{q.x * 1.6f, q.y * 1.6f + 0.15f}, aa * 6.4f);
    c.a = 1.0f;
    return c;
}

static Color BananaSkinTexel(Vec2 p, Color)
{
    Color c = Hex(PAL_BANANA);
    c = Mix(c, Hex(0xB8C040), SmoothStep(0.15f, 0.0f, p.y));
    c = Shade(c, 0.88f + 0.12f * FastSin(p.x * PI * 10.0f));
    float speck = Hash2(static_cast<int>(p.x * 40.0f), static_cast<int>(p.y * 40.0f), 523u);
    if (speck > 0.93f)
        c = Hex(0x8A5A20);
    if (p.y > 0.93f)
        c = Hex(0x3A2A18);
    return c;
}

// --- Palm impostor

ImageData MakePalmImpostorTexture()
{
    // A whole palm in a square: a thin curved trunk from the bottom centre up to the crown at 70 % of the height,
    // and a full crown of thick fronds radiating and drooping. Solid shapes, so the mipmaps keep their coverage.
    constexpr int SIZE = 128;
    Canvas canvas(SIZE, SIZE, Color{0.24f, 0.55f, 0.23f, 0.0f});
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            Vec2 p = {(x + 0.5f) / SIZE * 2.0f - 1.0f, (y + 0.5f) / SIZE * 2.0f - 1.0f};
            Vec2 crown = {0.06f, -0.42f};
            // Trunk: a slight curve, about 2 texels wide.
            float ty = Clamp((p.y - crown.y) / (1.0f - crown.y), 0.0f, 1.0f);
            float trunkX = crown.x * (1.0f - ty) * (1.0f - ty);
            float trunk = Abs(p.x - trunkX) - 0.018f;
            trunk = Max(trunk, Max(crown.y - p.y, p.y - 1.0f));
            float best = 1e9f;
            float along = 0.0f;
            // Eleven fronds fanned over the top half, drooping more the farther out they reach.
            for (int k = 0; k < 11; k++) {
                float ang = -PI * (0.97f - k * 0.094f);
                Vec2 dir = {FastCos(ang), FastSin(ang)};
                float out = Abs(dir.x);
                Vec2 tip = {crown.x + dir.x * 0.88f, crown.y + dir.y * 0.55f + 0.45f * out * out};
                Vec2 mid = {crown.x + dir.x * 0.48f, crown.y + dir.y * 0.4f + 0.06f * out};
                float d = FrondDistance(p, crown, mid, tip, 0.11f);
                if (d < best) {
                    best = d;
                    along = Sqrt((p.x - crown.x) * (p.x - crown.x) + (p.y - crown.y) * (p.y - crown.y)) / 0.9f;
                }
            }
            Color c = canvas.At(x, y);
            if (trunk < 0.0f)
                c = Color{0.42f, 0.33f, 0.24f, 1.0f};
            float cover = Coverage(best, 2.0f / SIZE);
            if (cover > 0.0f) {
                Color frond = Mix(Hex(0x3E8E3A), Hex(0x7BB043), Clamp(along, 0.0f, 1.0f));
                frond.a = 1.0f;
                c = Over(c, frond, cover);
            }
            canvas.At(x, y) = c;
        }
    }
    return canvas.ToImage();
}

// --- Texture

ImageData MakePropsTexture()
{
    Canvas canvas(PROPS_TEX, PROPS_TEX, Hex(0xFFFFFF));
    FillRect(canvas, PX_CROWN_SIDE, CrownSideTexel);
    FillRect(canvas, PX_CROWN_TOP, CrownTopTexel);
    FillRect(canvas, PX_STRATA, StrataTexel);
    FillRect(canvas, PX_BANANA_LEAF, BananaLeafTexel);
    FillRect(canvas, PX_TRUNK, TrunkTexel);
    FillRect(canvas, PX_ROOF, RoofTexel);
    FillRect(canvas, PX_WALL, WallTexel);
    FillRect(canvas, PX_CRATE, CrateTexel);
    FillRect(canvas, PX_PLANKS, PlanksTexel);
    FillRect(canvas, PX_ROCK, RockTexel);
    FillRect(canvas, PX_FLAG, FlagTexel);
    FillRect(canvas, PX_TOWN_FLAG, TownFlagTexel);
    FillRect(canvas, PX_BANANA_SKIN, BananaSkinTexel);
    static const uint32_t SWATCHES[PS_COUNT] = {0xFFFFFF, 0x1A1410, 0x4A3220, 0x9AA0A6,
                                                0xC1121F, 0xFFD23F, 0xFFE9A0, 0xB8A070};
    for (int i = 0; i < PS_COUNT; i++) {
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++)
                canvas.At(PX_SWATCH_X + (i % 4) * 16 + x, PX_SWATCH_Y + (i / 4) * 16 + y) = Hex(SWATCHES[i]);
        }
    }
    return canvas.ToImage();
}

} // namespace content
