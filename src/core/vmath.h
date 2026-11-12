// Vector math shared by every part of RedBaboon: vectors, quaternions, 4x4 matrices, a small RNG and colour packing.
// Header-only and platform independent (builds for the Xbox and for host tests).
//
// Conventions (docs/design.md §4):
//   - World: right-handed, +Y up, sea level at y = 0, 1 unit = 1 metre.
//   - Object local frame: +X right, +Y up, -Z forward (the nose points to -Z).
//   - Mat4 is row-major m[row][col] with ROW VECTORS: p' = p * M, translation in m[3][0..2].
//     "First A, then B" is A * B. This matches Cg's mul(float4, float4x4) with the 16 floats uploaded as they are.
#pragma once

#include <math.h>
#include <stdint.h>

// --- Scalars

constexpr float PI = 3.14159265358979f;
constexpr float DEG = PI / 180.0f;

inline float Clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float Lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

inline float Sign(float v)
{
    return v < 0.0f ? -1.0f : 1.0f;
}

// Frame-rate independent exponential approach: moves a toward b at rate k (1/s).
inline float Approach(float a, float b, float k, float dt)
{
    return b + (a - b) * expf(-k * dt);
}

// Wraps an angle to [-PI, PI].
inline float WrapPi(float a)
{
    while (a > PI)
        a -= 2.0f * PI;
    while (a < -PI)
        a += 2.0f * PI;
    return a;
}

// --- Vectors

struct Vec2 {
    float x;
    float y;
};

struct Vec3 {
    float x;
    float y;
    float z;

    constexpr Vec3() : x(0), y(0), z(0) {}
    constexpr Vec3(float ax, float ay, float az) : x(ax), y(ay), z(az) {}
};

inline Vec3 operator+(const Vec3& a, const Vec3& b)
{
    return Vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

inline Vec3 operator-(const Vec3& a, const Vec3& b)
{
    return Vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

inline Vec3 operator-(const Vec3& a)
{
    return Vec3(-a.x, -a.y, -a.z);
}

inline Vec3 operator*(const Vec3& a, float s)
{
    return Vec3(a.x * s, a.y * s, a.z * s);
}

inline Vec3 operator*(float s, const Vec3& a)
{
    return Vec3(a.x * s, a.y * s, a.z * s);
}

inline Vec3 operator/(const Vec3& a, float s)
{
    return Vec3(a.x / s, a.y / s, a.z / s);
}

inline Vec3& operator+=(Vec3& a, const Vec3& b)
{
    a = a + b;
    return a;
}

inline Vec3& operator-=(Vec3& a, const Vec3& b)
{
    a = a - b;
    return a;
}

inline Vec3& operator*=(Vec3& a, float s)
{
    a = a * s;
    return a;
}

inline float Dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

inline float LengthSq(const Vec3& v)
{
    return Dot(v, v);
}

inline float Length(const Vec3& v)
{
    return sqrtf(Dot(v, v));
}

// Unit vector along v, or zero for a (near) zero vector.
inline Vec3 Normalize(const Vec3& v)
{
    float l = Length(v);
    return l > 1e-8f ? v / l : Vec3();
}

inline Vec3 Lerp(const Vec3& a, const Vec3& b, float t)
{
    return a + (b - a) * t;
}

// Component-wise product.
inline Vec3 Mul(const Vec3& a, const Vec3& b)
{
    return Vec3(a.x * b.x, a.y * b.y, a.z * b.z);
}

struct Vec4 {
    float x;
    float y;
    float z;
    float w;
};

// --- Quaternions (unit rotations)

struct Quat {
    float w;
    float x;
    float y;
    float z;

    constexpr Quat() : w(1), x(0), y(0), z(0) {}
    constexpr Quat(float aw, float ax, float ay, float az) : w(aw), x(ax), y(ay), z(az) {}
};

// Hamilton product: a * b rotates by b first, then by a.
inline Quat operator*(const Quat& a, const Quat& b)
{
    return Quat(a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
                a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w);
}

inline Quat QuatAxisAngle(const Vec3& axis, float angle)
{
    Vec3 n = Normalize(axis);
    float s = sinf(angle * 0.5f);
    return Quat(cosf(angle * 0.5f), n.x * s, n.y * s, n.z * s);
}

inline Quat QuatNormalize(const Quat& q)
{
    float l = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (l < 1e-8f)
        return Quat();
    return Quat(q.w / l, q.x / l, q.y / l, q.z / l);
}

inline Quat QuatConjugate(const Quat& q)
{
    return Quat(q.w, -q.x, -q.y, -q.z);
}

inline Vec3 Rotate(const Quat& q, const Vec3& v)
{
    Vec3 u(q.x, q.y, q.z);
    Vec3 t = Cross(u, v) * 2.0f;
    return v + t * q.w + Cross(u, t);
}

inline Quat QuatSlerp(const Quat& a, Quat b, float t)
{
    float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < 0.0f) {
        b = Quat(-b.w, -b.x, -b.y, -b.z);
        d = -d;
    }
    if (d > 0.9995f)
        return QuatNormalize(Quat(Lerp(a.w, b.w, t), Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t)));
    float th = acosf(d);
    float s = sinf(th);
    float wa = sinf((1.0f - t) * th) / s;
    float wb = sinf(t * th) / s;
    return Quat(a.w * wa + b.w * wb, a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb);
}

// Local axes of a rotation, in world space.
inline Vec3 QuatRight(const Quat& q)
{
    return Rotate(q, Vec3(1, 0, 0));
}

inline Vec3 QuatUp(const Quat& q)
{
    return Rotate(q, Vec3(0, 1, 0));
}

inline Vec3 QuatForward(const Quat& q)
{
    return Rotate(q, Vec3(0, 0, -1));
}

// Rotation with local axes r (+X), u (+Y) and b (+Z, backward); the axes must be orthonormal.
inline Quat QuatFromBasis(const Vec3& r, const Vec3& u, const Vec3& b)
{
    float m00 = r.x;
    float m01 = u.x;
    float m02 = b.x;
    float m10 = r.y;
    float m11 = u.y;
    float m12 = b.y;
    float m20 = r.z;
    float m21 = u.z;
    float m22 = b.z;
    float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        return QuatNormalize(Quat(0.25f * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s));
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        return QuatNormalize(Quat((m21 - m12) / s, 0.25f * s, (m01 + m10) / s, (m02 + m20) / s));
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        return QuatNormalize(Quat((m02 - m20) / s, (m01 + m10) / s, 0.25f * s, (m12 + m21) / s));
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        return QuatNormalize(Quat((m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25f * s));
    }
}

// Rotation whose forward (-Z) is fwd and whose up is as close to up as possible.
inline Quat QuatLook(const Vec3& fwd, const Vec3& up)
{
    Vec3 b = Normalize(-fwd);
    Vec3 r = Normalize(Cross(up, b));
    if (LengthSq(r) < 1e-8f)
        r = Normalize(Cross(Vec3(0, 0, 1), b));
    Vec3 u = Cross(b, r);
    return QuatFromBasis(r, u, b);
}

// --- Matrices (row vectors, see the file comment)

struct Mat4 {
    float m[4][4];
};

inline Mat4 MatIdentity()
{
    Mat4 r = {};
    r.m[0][0] = 1.0f;
    r.m[1][1] = 1.0f;
    r.m[2][2] = 1.0f;
    r.m[3][3] = 1.0f;
    return r;
}

inline Mat4 operator*(const Mat4& a, const Mat4& b)
{
    Mat4 r;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    }
    return r;
}

inline Mat4 MatTranslate(const Vec3& t)
{
    Mat4 r = MatIdentity();
    r.m[3][0] = t.x;
    r.m[3][1] = t.y;
    r.m[3][2] = t.z;
    return r;
}

inline Mat4 MatScale(const Vec3& s)
{
    Mat4 r = MatIdentity();
    r.m[0][0] = s.x;
    r.m[1][1] = s.y;
    r.m[2][2] = s.z;
    return r;
}

// Rotation matrix: its rows are the rotated local axes.
inline Mat4 MatRotate(const Quat& q)
{
    Vec3 r = QuatRight(q);
    Vec3 u = QuatUp(q);
    Vec3 b = Rotate(q, Vec3(0, 0, 1));
    Mat4 m = MatIdentity();
    m.m[0][0] = r.x;
    m.m[0][1] = r.y;
    m.m[0][2] = r.z;
    m.m[1][0] = u.x;
    m.m[1][1] = u.y;
    m.m[1][2] = u.z;
    m.m[2][0] = b.x;
    m.m[2][1] = b.y;
    m.m[2][2] = b.z;
    return m;
}

// Model matrix: scale, then rotate, then translate.
inline Mat4 MatTRS(const Vec3& t, const Quat& q, const Vec3& s)
{
    return MatScale(s) * MatRotate(q) * MatTranslate(t);
}

inline Vec3 TransformPoint(const Mat4& m, const Vec3& p)
{
    return Vec3(p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
                p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
                p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]);
}

inline Vec3 TransformDir(const Mat4& m, const Vec3& p)
{
    return Vec3(p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0],
                p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1],
                p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2]);
}

// View matrix of a camera at eye with rotation q (the camera looks along its -Z).
inline Mat4 MatView(const Vec3& eye, const Quat& q)
{
    Vec3 r = QuatRight(q);
    Vec3 u = QuatUp(q);
    Vec3 b = Rotate(q, Vec3(0, 0, 1));
    Mat4 m = MatIdentity();
    m.m[0][0] = r.x;
    m.m[0][1] = u.x;
    m.m[0][2] = b.x;
    m.m[1][0] = r.y;
    m.m[1][1] = u.y;
    m.m[1][2] = b.y;
    m.m[2][0] = r.z;
    m.m[2][1] = u.z;
    m.m[2][2] = b.z;
    m.m[3][0] = -Dot(r, eye);
    m.m[3][1] = -Dot(u, eye);
    m.m[3][2] = -Dot(b, eye);
    return m;
}

// --- Random numbers and colours

// Small deterministic RNG (xorshift32): procedural content is the same on every run.
struct Rng {
    uint32_t state;

    explicit Rng(uint32_t seed = 0x1234567u) : state(seed ? seed : 1u) {}

    uint32_t Next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    // Uniform in [0, 1).
    float Uniform()
    {
        return (Next() >> 8) * (1.0f / 16777216.0f);
    }

    float Range(float lo, float hi)
    {
        return lo + (hi - lo) * Uniform();
    }
};

// Packs a colour (components 0..1) as 0xAARRGGBB.
inline uint32_t PackARGB(float r, float g, float b, float a = 1.0f)
{
    uint32_t R = static_cast<uint32_t>(Clamp(r, 0, 1) * 255.0f + 0.5f);
    uint32_t G = static_cast<uint32_t>(Clamp(g, 0, 1) * 255.0f + 0.5f);
    uint32_t B = static_cast<uint32_t>(Clamp(b, 0, 1) * 255.0f + 0.5f);
    uint32_t A = static_cast<uint32_t>(Clamp(a, 0, 1) * 255.0f + 0.5f);
    return (A << 24) | (R << 16) | (G << 8) | B;
}
