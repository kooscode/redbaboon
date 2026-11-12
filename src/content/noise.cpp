// Noise for procedural content: hashed value noise, fractal sums and cellular noise (docs/design.md §7).
#include "content/noise.h"

#include <math.h>

namespace content {

// --- Lattice hashing

static int Wrap(int v, int period)
{
    if (period <= 0)
        return v;
    int r = v % period;
    return r < 0 ? r + period : r;
}

// The hash itself, inlined into the noise functions.
static inline float HashInline(int x, int y, uint32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 0x8DA6B343u ^ static_cast<uint32_t>(y) * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return (h >> 8) * (1.0f / 16777216.0f);
}

float Hash2(int x, int y, uint32_t seed)
{
    return HashInline(x, y, seed);
}

// --- Value noise and fractal sums

float SmoothStep(float e0, float e1, float x)
{
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// floor() for the noise lattice without a library call (it is the hot path of every texture).
static inline int FastFloor(float v)
{
    int i = static_cast<int>(v);
    return v < static_cast<float>(i) ? i - 1 : i;
}

float ValueNoise(float x, float y, uint32_t seed, int period)
{
    int ix = FastFloor(x);
    int iy = FastFloor(y);
    float tx = x - static_cast<float>(ix);
    float ty = y - static_cast<float>(iy);
    tx = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);
    ty = ty * ty * ty * (ty * (ty * 6.0f - 15.0f) + 10.0f);
    int x0 = Wrap(ix, period);
    int y0 = Wrap(iy, period);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    if (period > 0) {
        x1 = x1 == period ? 0 : x1;
        y1 = y1 == period ? 0 : y1;
    }
    float a = HashInline(x0, y0, seed);
    float b = HashInline(x1, y0, seed);
    float c = HashInline(x0, y1, seed);
    float d = HashInline(x1, y1, seed);
    float top = a + (b - a) * tx;
    float bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

float Fbm(float x, float y, int octaves, uint32_t seed, int period, float gain)
{
    float sum = 0.0f;
    float amplitude = 1.0f;
    float total = 0.0f;
    for (int i = 0; i < octaves; i++) {
        sum += ValueNoise(x, y, seed + static_cast<uint32_t>(i) * 101u, period) * amplitude;
        total += amplitude;
        amplitude *= gain;
        x *= 2.0f;
        y *= 2.0f;
        if (period > 0)
            period *= 2;
    }
    return sum / total;
}

float RidgedFbm(float x, float y, int octaves, uint32_t seed, int period)
{
    float sum = 0.0f;
    float amplitude = 1.0f;
    float total = 0.0f;
    for (int i = 0; i < octaves; i++) {
        float n = 1.0f - Abs(ValueNoise(x, y, seed + static_cast<uint32_t>(i) * 131u, period) * 2.0f - 1.0f);
        sum += n * n * amplitude;
        total += amplitude;
        amplitude *= 0.5f;
        x *= 2.0f;
        y *= 2.0f;
        if (period > 0)
            period *= 2;
    }
    return sum / total;
}

// --- Cellular noise

void Cellular(float x, float y, uint32_t seed, int period, float& f1, float& f2)
{
    int ix = FastFloor(x);
    int iy = FastFloor(y);
    // Squared distances while searching; one square root each at the end.
    float d1 = 81.0f;
    float d2 = 81.0f;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int cx = ix + dx;
            int cy = iy + dy;
            int hx = Wrap(cx, period);
            int hy = Wrap(cy, period);
            float px = cx + HashInline(hx, hy, seed) - x;
            float py = cy + HashInline(hx, hy, seed + 77u) - y;
            float d = px * px + py * py;
            if (d < d1) {
                d2 = d1;
                d1 = d;
            } else if (d < d2) {
                d2 = d;
            }
        }
    }
    f1 = Sqrt(d1);
    f2 = Sqrt(d2);
}

} // namespace content
