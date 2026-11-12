// Fast float helpers for the content generator's per-texel and per-vertex loops. nxdk builds with -fno-builtin, so
// sqrtf, fabsf, floorf, sinf, expf and friends are library calls into x87 code (floorf also reloads the FPU control
// word); these inline versions use SSE (the console's Pentium III has it) or short polynomials. The content never
// has NaN or huge values, and texture painting does not need more than about 4 decimal digits. Internal to
// src/content.
#pragma once

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#define CONTENT_SSE 1
#else
#include <math.h>
#define CONTENT_SSE 0
#endif

namespace content {

constexpr float FAST_PI = 3.14159265358979f;

inline float Min(float a, float b)
{
    return a < b ? a : b;
}

inline float Max(float a, float b)
{
    return a > b ? a : b;
}

inline float Abs(float v)
{
    return __builtin_fabsf(v);
}

inline float Sqrt(float v)
{
#if CONTENT_SSE
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(v)));
#else
    return sqrtf(v);
#endif
}

// Floor for values well inside the int range.
inline float Floor(float v)
{
    int i = static_cast<int>(v);
    return static_cast<float>(v < static_cast<float>(i) ? i - 1 : i);
}

// Sine to about 1e-4 for any angle in about +-1e5 radians (range reduction, then a short odd polynomial).
inline float FastSin(float x)
{
    constexpr float INV_TWO_PI = 0.159154943f;
    x -= 2.0f * FAST_PI * Floor(x * INV_TWO_PI + 0.5f);
    // Now in [-pi, pi]; fold to [-pi/2, pi/2].
    if (x > 0.5f * FAST_PI)
        x = FAST_PI - x;
    else if (x < -0.5f * FAST_PI)
        x = -FAST_PI - x;
    float x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f + x2 * (0.0083333310f + x2 * (-0.00019840874f + x2 * 2.7525562e-6f))));
}

inline float FastCos(float x)
{
    return FastSin(x + 0.5f * FAST_PI);
}

// e^x to about 1e-4 relative for x in about [-80, 80]: 2^(x / ln 2) with the exponent built in the float's bits.
inline float FastExp(float x)
{
    if (x < -80.0f)
        return 0.0f;
    float t = x * 1.44269504f;
    float fi = Floor(t);
    float f = t - fi;
    float p = 1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    union {
        float f;
        int i;
    } bits;
    bits.f = p;
    bits.i += static_cast<int>(fi) << 23;
    return bits.f;
}

// atan2 to about 1e-4 radians.
inline float FastAtan2(float y, float x)
{
    float ax = Abs(x);
    float ay = Abs(y);
    float mx = Max(ax, ay);
    if (mx < 1e-20f)
        return 0.0f;
    float a = Min(ax, ay) / mx;
    float s = a * a;
    float r = a * (0.99997726f + s * (-0.33262347f + s * (0.19354346f + s * (-0.11643287f + s * (0.05265332f +
                                                                                                s * -0.01172120f)))));
    if (ay > ax)
        r = 0.5f * FAST_PI - r;
    if (x < 0.0f)
        r = FAST_PI - r;
    return y < 0.0f ? -r : r;
}

// x^y for x > 0 (2^(y log2 x) with a polynomial log).
inline float FastPow(float x, float y)
{
    if (x <= 0.0f)
        return 0.0f;
    union {
        float f;
        int i;
    } bits;
    bits.f = x;
    float e = static_cast<float>(((bits.i >> 23) & 0xFF) - 127);
    bits.i = (bits.i & 0x007FFFFF) | 0x3F800000;
    float m = bits.f;
    // log2 of the mantissa in [1, 2).
    float t = (m - 1.0f) / (m + 1.0f);
    float t2 = t * t;
    float l = 2.8853900f * t * (1.0f + t2 * (0.33333333f + t2 * (0.2f + t2 * 0.14285714f)));
    return FastExp((e + l) * y * 0.69314718f);
}

} // namespace content
