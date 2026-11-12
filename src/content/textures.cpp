// Environment and effect textures: terrain detail, water ripples, the particle atlas, cloud puffs and the blob
// shadow. docs/design.md §7; creative brief §1, §3.
#include "content/content.h"
#include "content/emblems.h"
#include "content/noise.h"
#include "content/paint.h"

#include <math.h>

#include <vector>

namespace content {

// --- Smooth textures at a lower resolution

// Fills a size x size tiling image from a function painted at half the size and upsampled bilinearly (with wrap):
// a quarter of the work for textures without fine detail.
template <typename F>
static ImageData HalfResTiled(int size, F texel)
{
    int half = size / 2;
    std::vector<Color> low(static_cast<size_t>(half) * half);
    for (int y = 0; y < half; y++) {
        for (int x = 0; x < half; x++)
            low[static_cast<size_t>(y) * half + x] = texel((x + 0.5f) / half, (y + 0.5f) / half);
    }
    Canvas canvas(size, size, Hex(0x808080));
    for (int y = 0; y < size; y++) {
        int y0 = (y - 1) / 2;
        int y1 = y0 + 1;
        float fy = (y % 2 == 1) ? 0.25f : 0.75f;
        if (y == 0)
            y0 = -1;
        for (int x = 0; x < size; x++) {
            int x0 = (x - 1) / 2;
            int x1 = x0 + 1;
            float fx = (x % 2 == 1) ? 0.25f : 0.75f;
            if (x == 0)
                x0 = -1;
            const Color& a = low[static_cast<size_t>((y0 + half) % half) * half + (x0 + half) % half];
            const Color& b = low[static_cast<size_t>((y0 + half) % half) * half + x1 % half];
            const Color& c = low[static_cast<size_t>(y1 % half) * half + (x0 + half) % half];
            const Color& d = low[static_cast<size_t>(y1 % half) * half + x1 % half];
            canvas.At(x, y) = Mix(Mix(a, b, fx), Mix(c, d, fx), fy);
        }
    }
    return canvas.ToImage();
}

// --- Terrain detail

// Structure of the terrain detail tile (16 m) at (u, v), as a brightness offset of about +-0.22: rounded clumps
// about 2 m across with dark gaps (bushes and crowns on vegetation, boulders on rock), tufts with dark seams
// (Worley cells), sharp grit and pebbles.
static float DetailStructure(float u, float v)
{
    float clumps = Fbm(u * 4.0f, v * 4.0f, 2, 101u, 4);
    float c1 = 0.0f;
    float c2 = 0.0f;
    Cellular(u * 8.0f, v * 8.0f, 117u, 8, c1, c2);
    float crown = SmoothStep(0.75f, 0.15f, c1) - 0.3f * SmoothStep(0.1f, 0.0f, c2 - c1);
    float f1 = 0.0f;
    float f2 = 0.0f;
    Cellular(u * 24.0f, v * 24.0f, 109u, 24, f1, f2);
    float tuft = SmoothStep(0.0f, 0.3f, f2 - f1);
    float grit = ValueNoise(u * 128.0f, v * 128.0f, 103u, 128);
    float pebble = SmoothStep(0.82f, 0.88f, ValueNoise(u * 48.0f, v * 48.0f, 113u, 48));
    float d = 0.12f * (clumps - 0.5f) + 0.2f * (crown - 0.3f) + 0.05f * (tuft - 0.6f) + 0.12f * (grit - 0.5f) +
              0.08f * pebble;
    return Clamp(d, -0.25f, 0.22f);
}

// The detail at a brightness level: centre (the average) plus the structure scaled by gain, warm-tinted.
static ImageData DetailImage(float centre, float gain)
{
    constexpr int SIZE = 256;
    Canvas canvas(SIZE, SIZE, Hex(0x808080));
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            float g = centre * (1.0f + gain * DetailStructure(static_cast<float>(x) / SIZE,
                                                              static_cast<float>(y) / SIZE));
            canvas.At(x, y) = Color{g * 1.02f, g, g * 0.96f, 1.0f};
        }
    }
    return canvas.ToImage();
}

ImageData MakeTerrainDetailTexture()
{
    // For the 2x detail path: mid grey (0x80 = neutral), structure about +-22 %.
    return DetailImage(0.5f, 1.0f);
}

ImageData MakeTerrainMacroTexture()
{
    // One tile covers 600 m: broad lighter and darker patches (+-10 %) with slight warm/cool shifts, so the
    // island does not read as one flat colour and the detail's repeat is broken up. Mid grey average.
    return HalfResTiled(256, [](float u, float v) {
        float patches = Fbm(u * 5.0f, v * 5.0f, 4, 121u, 5);
        float hue = Fbm(u * 3.0f + 9.0f, v * 3.0f, 3, 127u, 3);
        float streak = ValueNoise(u * 24.0f, v * 6.0f, 131u, 6);
        float g = 0.5f * (1.0f + 0.2f * (patches - 0.5f) + 0.04f * (streak - 0.5f));
        float warm = 0.06f * (hue - 0.5f);
        return Color{g * (1.0f + warm), g, g * (1.0f - warm), 1.0f};
    });
}

// --- Water

// Water texel: brightness (around 0.78) and alpha (around 0.76) of the 32 m ripple tile.
static Color WaterTexel(float u, float v)
{
    // Wave cells (Worley borders are crests), warped by noise so they do not look like a grid.
    float wu = u + 0.04f * (Fbm(u * 4.0f, v * 4.0f, 2, 201u, 4) - 0.5f);
    float wv = v + 0.04f * (Fbm(u * 4.0f + 7.0f, v * 4.0f, 2, 203u, 4) - 0.5f);
    float f1 = 0.0f;
    float f2 = 0.0f;
    Cellular(wu * 8.0f, wv * 8.0f, 207u, 8, f1, f2);
    float crest = SmoothStep(0.18f, 0.0f, f2 - f1);
    float swell = Fbm(u * 3.0f, v * 3.0f, 3, 211u, 3);
    float fine = ValueNoise(u * 16.0f, v * 16.0f, 213u, 16);
    float b = 0.78f + 0.14f * (swell - 0.5f) + 0.06f * (fine - 0.5f);
    Color c = {b * 0.9f, b * 0.96f, b, 0.76f + 0.08f * (swell - 0.5f)};
    return Mix(c, Color{1.0f, 1.0f, 1.0f, 0.92f}, crest * 0.6f);
}

// The water tile with its colour scaled by k.
static ImageData WaterImage(float k)
{
    constexpr int SIZE = 256;
    Canvas canvas(SIZE, SIZE, Hex(0xFFFFFF));
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            Color c = WaterTexel(static_cast<float>(x) / SIZE, static_cast<float>(y) / SIZE);
            canvas.At(x, y) = Color{c.r * k, c.g * k, c.b * k, c.a};
        }
    }
    return canvas.ToImage();
}

ImageData MakeSeaTexture()
{
    // The same ripples re-centred on mid grey for the 2x detail path (alpha unchanged).
    return WaterImage(0.5f / 0.8f);
}

ImageData MakeSeaDetailTexture()
{
    // The sea's second layer: long swells and wind streaks on a large tile (meant for ~160 m), mid grey +-12 %,
    // so the 32 m ripple tile does not repeat visibly from altitude.
    return HalfResTiled(256, [](float u, float v) {
        float swell = Fbm(u * 4.0f, v * 4.0f, 4, 221u, 4);
        float wu = u + 0.05f * (swell - 0.5f);
        float streak = ValueNoise(wu * 32.0f, v * 8.0f, 223u, 8);
        float crest = SmoothStep(0.75f, 1.0f, RidgedFbm(wu * 6.0f, v * 6.0f, 2, 227u, 6));
        float g = 0.5f * (1.0f + 0.16f * (swell - 0.5f) + 0.06f * (streak - 0.5f) + 0.1f * crest - 0.02f);
        return Color{g, g, g, 1.0f};
    });
}

// --- Particles

constexpr int CELL = 64;

// Soft puff: noisy round density, lit from the top-left; white-grey, alpha from density.
static void PaintPuff(Canvas& canvas, int cx, int cy, uint32_t seed)
{
    for (int y = 0; y < CELL; y++) {
        for (int x = 0; x < CELL; x++) {
            float px = (x + 0.5f) / CELL * 2.0f - 1.0f;
            float py = (y + 0.5f) / CELL * 2.0f - 1.0f;
            float r = Sqrt(px * px + py * py);
            float n = Fbm(px * 2.5f + seed, py * 2.5f, 4, seed);
            float d = 1.0f - r * 1.15f + (n - 0.5f) * 0.7f;
            float edge = SmoothStep(0.98f, 0.8f, Max(Abs(px), Abs(py)));
            float a = SmoothStep(0.0f, 0.45f, d) * SmoothStep(1.0f, 0.8f, r) * edge;
            float light = Clamp(0.75f + 0.35f * (-px - py) * 0.5f + 0.25f * (n - 0.5f), 0.55f, 1.0f);
            canvas.At(cx + x, cy + y) = Color{light, light, light * 0.98f, a};
        }
    }
}

// Fire: a turbulent blob ramping from a pale core through orange to red at the edges (for additive blending).
static void PaintFire(Canvas& canvas, int cx, int cy, int frame)
{
    float grow = 0.75f + frame * 0.08f;
    for (int y = 0; y < CELL; y++) {
        for (int x = 0; x < CELL; x++) {
            float px = ((x + 0.5f) / CELL * 2.0f - 1.0f) / grow;
            float py = ((y + 0.5f) / CELL * 2.0f - 1.0f) / grow;
            float r = Sqrt(px * px + py * py);
            float n = Fbm(px * 3.0f + frame * 1.7f, py * 3.0f - frame, 4, 301u + frame);
            float heat = Clamp(1.0f - r * 1.2f + (n - 0.5f) * 0.9f - frame * 0.06f, 0.0f, 1.0f);
            Color c = Mix(Hex(0xFF6A1A), Hex(0xFFB23F), SmoothStep(0.15f, 0.5f, heat));
            c = Mix(c, Hex(0xFFF6D5), SmoothStep(0.5f, 0.85f, heat));
            float edge = SmoothStep(0.98f, 0.8f, Max(Abs(px), Abs(py)) * grow);
            c.a = SmoothStep(0.0f, 0.3f, heat) * SmoothStep(1.0f, 0.8f, r * grow) * edge;
            canvas.At(cx + x, cy + y) = c;
        }
    }
}

// Star shapes: spark (4 thin spikes) and muzzle flash (6 broad petals).
static void PaintStar(Canvas& canvas, int cx, int cy, int points, float sharpness, const Color& tint)
{
    for (int y = 0; y < CELL; y++) {
        for (int x = 0; x < CELL; x++) {
            float px = (x + 0.5f) / CELL * 2.0f - 1.0f;
            float py = (y + 0.5f) / CELL * 2.0f - 1.0f;
            float r = Sqrt(px * px + py * py);
            float ang = FastAtan2(py, px);
            float spikes = FastPow(Abs(FastCos(ang * points * 0.5f)), sharpness);
            float a = FastExp(-r * r * 18.0f) + spikes * SmoothStep(1.0f, 0.0f, r) * SmoothStep(0.95f, 0.3f, r) * 0.9f;
            a = Clamp(a, 0.0f, 1.0f);
            Color c = Mix(tint, Hex(0xFFFBE0), Clamp(FastExp(-r * r * 10.0f) * 1.5f, 0.0f, 1.0f));
            c.a = a;
            canvas.At(cx + x, cy + y) = c;
        }
    }
}

// Shapes drawn with an emblem painter over a transparent cell.
template <typename F>
static void PaintCell(Canvas& canvas, int cx, int cy, float scale, F paint)
{
    for (int y = 0; y < CELL; y++) {
        for (int x = 0; x < CELL; x++) {
            Vec2 p = {((x + 0.5f) / CELL * 2.0f - 1.0f) * scale, ((y + 0.5f) / CELL * 2.0f - 1.0f) * scale};
            Color c = canvas.At(cx + x, cy + y);
            canvas.At(cx + x, cy + y) = paint(c, p, 2.0f * scale / CELL);
        }
    }
}

// A banana peel: three floppy petals around a stub, yellow outside with a cream inside.
static Color PaintPeel(Color c, Vec2 p, float aa, float twist)
{
    float d = 1e9f;
    float inner = 1e9f;
    for (int i = 0; i < 3; i++) {
        float a = twist + i * 2.1f;
        Vec2 tip = {0.55f * FastCos(a), 0.55f * FastSin(a) + 0.15f};
        float petal = SdSegment(p, Vec2{0.0f, -0.05f}, tip) - 0.17f + 0.08f * SmoothStep(0.0f, 0.6f, Sqrt(
                          (p.x - tip.x) * (p.x - tip.x) + (p.y - tip.y) * (p.y - tip.y)));
        d = Min(d, petal);
        inner = Min(inner, SdSegment(p, Vec2{0.0f, -0.05f}, Vec2{tip.x * 0.8f, tip.y * 0.8f}) - 0.07f);
    }
    float stub = SdBox(p, Vec2{0.0f, -0.32f}, Vec2{0.08f, 0.22f}, 0.0f, 0.05f);
    c = Over(c, Hex(PAL_INK), Coverage(Min(d, stub) - 0.06f, aa));
    c = Over(c, Hex(PAL_BANANA), Coverage(Min(d, stub), aa));
    c = Over(c, Hex(0xF4ECC8), Coverage(inner, aa));
    return Over(c, Hex(0x6A4A22), Coverage(SdBox(p, Vec2{0.0f, -0.52f}, Vec2{0.06f, 0.05f}), aa));
}

ImageData MakeParticleAtlas()
{
    Canvas canvas(256, 256, Color{1.0f, 1.0f, 1.0f, 0.0f});
    for (int i = 0; i < 4; i++)
        PaintPuff(canvas, i * CELL, 0, 401u + i * 13u);
    for (int i = 0; i < 4; i++)
        PaintFire(canvas, i * CELL, CELL, i);
    PaintStar(canvas, 0, 2 * CELL, 4, 40.0f, Hex(0xFFD23F));
    PaintStar(canvas, CELL, 2 * CELL, 6, 3.0f, Hex(0xFFB23F));
    PaintCell(canvas, 2 * CELL, 2 * CELL, 1.25f, [](Color c, Vec2 p, float aa) {
        return PaintBanana(c, Vec2{p.x, p.y + 0.1f}, aa);
    });
    for (int y = 0; y < CELL; y++) {
        for (int x = 0; x < CELL; x++) {
            float px = (x + 0.5f) / CELL * 2.0f - 1.0f;
            float py = (y + 0.5f) / CELL * 2.0f - 1.0f;
            float r2 = px * px + py * py;
            float glow = Clamp(FastExp(-r2 * 5.0f) * (1.0f - r2), 0.0f, 1.0f);
            canvas.At(3 * CELL + x, 2 * CELL + y) = Color{1.0f, 1.0f, 1.0f, glow};
        }
    }
    PaintCell(canvas, 0, 3 * CELL, 1.0f, [](Color c, Vec2 p, float aa) {
        return PaintPeel(c, p, aa, -1.57f);
    });
    PaintCell(canvas, CELL, 3 * CELL, 1.0f, [](Color c, Vec2 p, float aa) {
        return PaintPeel(c, Vec2{p.x * 1.1f, p.y}, aa, -0.9f);
    });
    PaintCell(canvas, 2 * CELL, 3 * CELL, 1.0f, [](Color c, Vec2 p, float aa) {
        Vec2 shard[5] = {{-0.6f, -0.3f}, {0.1f, -0.7f}, {0.7f, -0.1f}, {0.3f, 0.6f}, {-0.4f, 0.4f}};
        float d = SdPolygon(p, shard, 5);
        c = Over(c, Hex(0x2A2A2E), Coverage(d, aa));
        c = Over(c, Hex(0x8A8E94), Coverage(d + 0.07f, aa));
        return Over(c, Hex(0xD8DCE0), Coverage(Max(d + 0.07f, SdSegment(p, Vec2{-0.3f, -0.2f},
                                                                              Vec2{0.3f, -0.45f}) - 0.06f), aa));
    });
    PaintCell(canvas, 3 * CELL, 3 * CELL, 1.0f, [](Color c, Vec2 p, float aa) {
        Vec2 plank[4] = {{-0.8f, -0.25f}, {0.75f, -0.45f}, {0.85f, -0.1f}, {-0.7f, 0.2f}};
        float d = SdPolygon(p, plank, 4);
        c = Over(c, Hex(0x3A2412), Coverage(d, aa));
        Color wood = Mix(Hex(0x8A5A30), Hex(0xB07A44), 0.5f + 0.5f * FastSin(p.y * 40.0f + p.x * 6.0f));
        return Over(c, wood, Coverage(d + 0.06f, aa));
    });
    return canvas.ToImage();
}

// --- Clouds

ImageData MakeCloudTexture()
{
    // Four puffs in a 2 x 2 atlas. Each is overlapping soft lobes (bigger in the middle, a flat base); noise
    // roughens the edges only, so the cell borders stay clear. Soft enough to paint at half size and upsample.
    constexpr int LOBES = 11;
    Vec2 lobes[4][LOBES];
    float radii[4][LOBES];
    for (int f = 0; f < 4; f++) {
        Rng rng(901u + f * 77u);
        for (int i = 0; i < LOBES; i++) {
            float x = rng.Range(-0.62f, 0.62f);
            lobes[f][i] = Vec2{x, rng.Range(-0.05f, 0.22f) - 0.5f * (1.0f - Abs(x) * 1.5f) * rng.Range(0.6f, 1.0f)};
            radii[f][i] = rng.Range(0.2f, 0.32f) * (1.0f - Abs(x) * 0.45f);
        }
    }
    return HalfResTiled(256, [&](float u, float v) {
        int f = (u >= 0.5f ? 1 : 0) + (v >= 0.5f ? 2 : 0);
        float px = (u * 2.0f - (f % 2)) * 2.0f - 1.0f;
        float py = (v * 2.0f - (f / 2)) * 2.0f - 1.0f;
        float density = 0.0f;
        for (int i = 0; i < LOBES; i++) {
            float dx = (px - lobes[f][i].x) / radii[f][i];
            float dy = (py - lobes[f][i].y) / radii[f][i];
            float q = dx * dx + dy * dy;
            if (q < 6.0f)
                density += FastExp(-q * 2.2f);
        }
        density *= SmoothStep(0.42f, 0.22f, py);
        float n = Fbm(px * 5.0f + f * 5.0f, py * 5.0f, 4, 907u + f);
        density *= 0.6f + 0.8f * n;
        float edge = SmoothStep(0.97f, 0.75f, Max(Abs(px), Abs(py)));
        float a = SmoothStep(0.25f, 0.75f, density) * edge;
        float light = Clamp(0.75f - py * 0.9f + (n - 0.5f) * 0.4f - SmoothStep(0.8f, 1.6f, density) * 0.15f, 0.0f,
                            1.0f);
        Color c = Mix(Hex(0xB9C6D6), Hex(0xFFFFFF), light);
        c.a = a;
        return c;
    });
}

// --- Blob shadow

ImageData MakeShadowTexture()
{
    constexpr int SIZE = 64;
    Canvas canvas(SIZE, SIZE, Color{0.1f, 0.08f, 0.06f, 0.0f});
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            float px = (x + 0.5f) / SIZE * 2.0f - 1.0f;
            float py = (y + 0.5f) / SIZE * 2.0f - 1.0f;
            float r = Sqrt(px * px + py * py);
            canvas.At(x, y).a = 0.6f * SmoothStep(1.0f, 0.25f, r);
        }
    }
    return canvas.ToImage();
}

} // namespace content
