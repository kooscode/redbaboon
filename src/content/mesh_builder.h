// Mesh building blocks for the procedural content: vertices and triangles, parametric surfaces (lofts, lathes,
// wings), primitives (boxes, tubes, ellipsoids, tori, prisms, cards), transforms and merging.
// Internal to src/content (docs/design.md §7). Conventions from core/geometry.h: counter-clockwise front faces seen
// from outside, 16-bit indices.
#pragma once

#include <math.h>
#include <stdint.h>

#include <vector>

#include "content/fast_math.h"
#include "core/geometry.h"
#include "core/vmath.h"

namespace content {

// --- Colours and texture rectangles

// Opaque colour from 0xRRGGBB.
inline uint32_t Rgb(uint32_t rgb)
{
    return 0xFF000000u | rgb;
}

// Colour from components 0..1 with alpha 1.
inline uint32_t RgbF(const Vec3& c)
{
    return PackARGB(c.x, c.y, c.z, 1.0f);
}

// A rectangle in texture space; a swatch (one solid texel area) has u0 == u1 and v0 == v1.
struct UvRect {
    float u0;
    float v0;
    float u1;
    float v1;
};

// Maps (s, t) into a texture rectangle (s along u, t along v).
Vec2 MapUv(const UvRect& r, float s, float t);

// Rectangle from atlas pixels (x, y, w, h) in a size x size texture.
UvRect AtlasUv(int x, int y, int w, int h, int size);

// The centre of an atlas pixel rectangle, as a swatch.
UvRect AtlasSwatch(int x, int y, int w, int h, int size);

// --- Vertices and triangles

int AddVertex(MeshData& mesh, const Vec3& p, const Vec3& n, float u, float v, uint32_t color);
void AddTriangle(MeshData& mesh, int a, int b, int c);
// Two triangles a-b-c and a-c-d (counter-clockwise quad).
void AddQuad(MeshData& mesh, int a, int b, int c, int d);

// --- Parametric surfaces

// A grid of (cols + 1) x (rows + 1) vertices over s, t in [0, 1]. pos(s, t) gives the point, uv(s, t) the texture
// coordinate, color(s, t) the vertex colour. Normals are cross(dP/ds, dP/dt) (numerical); triangles face that way
// unless flip is set. closedS: pos is periodic in s (normals wrap across the seam).
struct SurfaceOptions {
    bool flip = false;
    bool closedS = false;
};

template <typename PosF, typename UvF, typename ColorF>
void AddSurface(MeshData& mesh, int cols, int rows, PosF pos, UvF uv, ColorF color, SurfaceOptions options = {})
{
    constexpr float EPS = 1e-3f;
    int base = static_cast<int>(mesh.vertices.size());
    for (int j = 0; j <= rows; j++) {
        for (int i = 0; i <= cols; i++) {
            float s = static_cast<float>(i) / cols;
            float t = static_cast<float>(j) / rows;
            Vec3 p = pos(s, t);
            Vec3 n;
            float tn = t;
            for (int attempt = 0; attempt < 3; attempt++) {
                float s0 = options.closedS ? s - EPS : Clamp(s - EPS, 0.0f, 1.0f);
                float s1 = options.closedS ? s + EPS : Clamp(s + EPS, 0.0f, 1.0f);
                float t0 = Clamp(tn - EPS, 0.0f, 1.0f);
                float t1 = Clamp(tn + EPS, 0.0f, 1.0f);
                Vec3 ds = pos(s1, tn) - pos(s0, tn);
                Vec3 dt = pos(s, t1) - pos(s, t0);
                Vec3 c = Cross(ds, dt);
                if (LengthSq(c) > 1e-14f) {
                    n = Normalize(c);
                    break;
                }
                tn = tn < 0.5f ? tn + 0.02f : tn - 0.02f;
            }
            if (options.flip)
                n = -n;
            Vec2 tc = uv(s, t);
            AddVertex(mesh, p, n, tc.x, tc.y, color(s, t));
        }
    }
    for (int j = 0; j < rows; j++) {
        for (int i = 0; i < cols; i++) {
            int a = base + j * (cols + 1) + i;
            int b = a + 1;
            int c = a + cols + 2;
            int d = a + cols + 1;
            if (options.flip)
                AddQuad(mesh, a, d, c, b);
            else
                AddQuad(mesh, a, b, c, d);
        }
    }
}

// A tube swept along path(t), t in [0, 1], with radius(t); s goes around it. Open ends.
template <typename PathF, typename RadiusF>
void AddSweep(MeshData& mesh, int segments, int sides, PathF path, RadiusF radius, const UvRect& uv, uint32_t color)
{
    AddSurface(
        mesh, sides, segments,
        [&](float s, float t) {
            constexpr float E = 1e-3f;
            Vec3 tangent = Normalize(path(Clamp(t + E, 0.0f, 1.0f)) - path(Clamp(t - E, 0.0f, 1.0f)));
            Vec3 ref = Abs(tangent.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
            Vec3 n = Normalize(Cross(ref, tangent));
            Vec3 b = Cross(tangent, n);
            float a = s * 2.0f * PI;
            return path(t) + (n * FastCos(a) + b * FastSin(a)) * radius(t);
        },
        [&](float s, float t) {
            return MapUv(uv, s, t);
        },
        [&](float, float) {
            return color;
        },
        SurfaceOptions{false, true});
}

// --- Primitives (local space; place them with Append and a matrix)

// Axis-aligned ellipsoid; s goes around Y (from +Z toward +X), t from the top pole to the bottom one.
void AddEllipsoid(MeshData& mesh, const Vec3& center, const Vec3& radii, int slices, int stacks, const UvRect& uv,
                  uint32_t color);
// Cylinder or cone from a to b with radii ra and rb; caps closes the ends.
void AddTube(MeshData& mesh, const Vec3& a, const Vec3& b, float ra, float rb, int segments, const UvRect& uv,
             uint32_t color, bool caps);
// Flat-shaded box.
void AddBox(MeshData& mesh, const Vec3& center, const Vec3& half, const UvRect& uv, uint32_t color);
// Surface of revolution around +Y. profile holds (radius, y) points from top to bottom (or bottom to top with flip).
void AddLathe(MeshData& mesh, const std::vector<Vec2>& profile, int segments, const UvRect& uv, uint32_t color,
              bool flip);
// Torus around the Z axis (ring in the XY plane): major radius R, tube radius r.
void AddTorus(MeshData& mesh, float R, float r, int segments, int sides, const UvRect& uv, uint32_t color);
// Flat-shaded prism: a simple polygon (counter-clockwise in XY) extruded from z = -depth/2 to +depth/2.
void AddPrism(MeshData& mesh, const std::vector<Vec2>& polygon, float depth, const UvRect& uv, uint32_t color);
// A quad card with corners p0..p3 (counter-clockwise from the front); twoSided adds the back face.
void AddCard(MeshData& mesh, const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, const UvRect& uv,
             uint32_t color, bool twoSided);

// --- Transforms and merging

// Transforms vertices from firstVertex (positions by m, normals by its inverse transpose). A mirroring matrix
// (negative determinant) also reverses the triangles from firstIndex so they stay counter-clockwise.
void TransformMesh(MeshData& mesh, const Mat4& m, int firstVertex = 0, int firstIndex = 0);
// Appends src to dst, transformed by m.
void Append(MeshData& dst, const MeshData& src, const Mat4& m);
void Append(MeshData& dst, const MeshData& src);
// Gives every triangle its own vertices with the face normal (faceted look; build such parts in their own mesh).
void Flatten(MeshData& mesh);
// Multiplies the vertex colours from firstVertex by c (0xAARRGGBB).
void TintMesh(MeshData& mesh, uint32_t c, int firstVertex = 0);

// Matrix mapping local +X to x, +Y to y (both unit, perpendicular), +Z to x cross y, and the origin to origin.
Mat4 Basis(const Vec3& x, const Vec3& y, const Vec3& origin);
// Model matrix: scale, rotate (Euler angles in radians: yaw around Y, pitch around X, roll around Z), translate.
Mat4 Place(const Vec3& pos, float yaw = 0.0f, float pitch = 0.0f, float roll = 0.0f, const Vec3& scale = Vec3(1, 1, 1));

} // namespace content
