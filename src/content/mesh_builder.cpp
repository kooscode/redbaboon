// Mesh building blocks: vertices, primitives, transforms and merging (docs/design.md §7).
#include "content/mesh_builder.h"

#include <math.h>

namespace content {

// --- Texture rectangles

UvRect AtlasUv(int x, int y, int w, int h, int size)
{
    float s = 1.0f / size;
    return UvRect{x * s, y * s, (x + w) * s, (y + h) * s};
}

UvRect AtlasSwatch(int x, int y, int w, int h, int size)
{
    float s = 1.0f / size;
    float u = (x + w * 0.5f) * s;
    float v = (y + h * 0.5f) * s;
    return UvRect{u, v, u, v};
}

Vec2 MapUv(const UvRect& r, float s, float t)
{
    return Vec2{r.u0 + (r.u1 - r.u0) * s, r.v0 + (r.v1 - r.v0) * t};
}

// --- Vertices and triangles

int AddVertex(MeshData& mesh, const Vec3& p, const Vec3& n, float u, float v, uint32_t color)
{
    Vertex vx;
    vx.px = p.x;
    vx.py = p.y;
    vx.pz = p.z;
    vx.nx = n.x;
    vx.ny = n.y;
    vx.nz = n.z;
    vx.u = u;
    vx.v = v;
    vx.color = color;
    mesh.vertices.push_back(vx);
    return static_cast<int>(mesh.vertices.size()) - 1;
}

void AddTriangle(MeshData& mesh, int a, int b, int c)
{
    mesh.indices.push_back(static_cast<uint16_t>(a));
    mesh.indices.push_back(static_cast<uint16_t>(b));
    mesh.indices.push_back(static_cast<uint16_t>(c));
}

void AddQuad(MeshData& mesh, int a, int b, int c, int d)
{
    AddTriangle(mesh, a, b, c);
    AddTriangle(mesh, a, c, d);
}

// --- Primitives

void AddEllipsoid(MeshData& mesh, const Vec3& center, const Vec3& radii, int slices, int stacks, const UvRect& uv,
                  uint32_t color)
{
    AddSurface(
        mesh, slices, stacks,
        [&](float s, float t) {
            float a = s * 2.0f * PI;
            float b = t * PI;
            return Vec3(center.x + radii.x * FastSin(b) * FastSin(a), center.y + radii.y * FastCos(b),
                        center.z + radii.z * FastSin(b) * FastCos(a));
        },
        [&](float s, float t) {
            return MapUv(uv, s, t);
        },
        [&](float, float) {
            return color;
        },
        SurfaceOptions{true, true});
}

void AddTube(MeshData& mesh, const Vec3& a, const Vec3& b, float ra, float rb, int segments, const UvRect& uv,
             uint32_t color, bool caps)
{
    Vec3 axis = Normalize(b - a);
    Vec3 side = Abs(axis.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    Vec3 e1 = Normalize(Cross(axis, side));
    Vec3 e2 = Cross(e1, axis);
    AddSurface(
        mesh, segments, 1,
        [&](float s, float t) {
            float ang = s * 2.0f * PI;
            Vec3 radial = e1 * FastCos(ang) + e2 * FastSin(ang);
            return Lerp(a, b, t) + radial * Lerp(ra, rb, t);
        },
        [&](float s, float t) {
            return MapUv(uv, s, t);
        },
        [&](float, float) {
            return color;
        },
        SurfaceOptions{true, true});
    if (!caps)
        return;
    for (int end = 0; end < 2; end++) {
        Vec3 c = end == 0 ? a : b;
        float r = end == 0 ? ra : rb;
        if (r <= 0.0f)
            continue;
        Vec3 n = end == 0 ? -axis : axis;
        int center = AddVertex(mesh, c, n, uv.u0, uv.v0, color);
        int first = static_cast<int>(mesh.vertices.size());
        for (int i = 0; i < segments; i++) {
            float ang = static_cast<float>(i) / segments * 2.0f * PI;
            AddVertex(mesh, c + (e1 * FastCos(ang) + e2 * FastSin(ang)) * r, n, uv.u0, uv.v0, color);
        }
        for (int i = 0; i < segments; i++) {
            int p = first + i;
            int q = first + (i + 1) % segments;
            if (end == 0)
                AddTriangle(mesh, center, p, q);
            else
                AddTriangle(mesh, center, q, p);
        }
    }
}

void AddBox(MeshData& mesh, const Vec3& center, const Vec3& half, const UvRect& uv, uint32_t color)
{
    static const Vec3 NORMALS[6] = {Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0),
                                    Vec3(0, -1, 0), Vec3(0, 0, 1), Vec3(0, 0, -1)};
    for (int f = 0; f < 6; f++) {
        Vec3 n = NORMALS[f];
        Vec3 u = Abs(n.y) > 0.5f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        Vec3 v = Cross(n, u);
        Vec3 c = center + Mul(n, half);
        Vec3 du = Mul(u, half);
        Vec3 dv = Mul(v, half);
        int i0 = AddVertex(mesh, c - du - dv, n, uv.u0, uv.v1, color);
        int i1 = AddVertex(mesh, c + du - dv, n, uv.u1, uv.v1, color);
        int i2 = AddVertex(mesh, c + du + dv, n, uv.u1, uv.v0, color);
        int i3 = AddVertex(mesh, c - du + dv, n, uv.u0, uv.v0, color);
        AddQuad(mesh, i0, i1, i2, i3);
    }
}

void AddLathe(MeshData& mesh, const std::vector<Vec2>& profile, int segments, const UvRect& uv, uint32_t color,
              bool flip)
{
    int last = static_cast<int>(profile.size()) - 1;
    AddSurface(
        mesh, segments, last,
        [&](float s, float t) {
            float f = Clamp(t, 0.0f, 1.0f) * last;
            int i = static_cast<int>(f);
            if (i >= last)
                i = last - 1;
            float k = f - i;
            Vec2 a = profile[i];
            Vec2 b = profile[i + 1];
            float r = a.x + (b.x - a.x) * k;
            float y = a.y + (b.y - a.y) * k;
            float ang = s * 2.0f * PI;
            return Vec3(r * FastSin(ang), y, r * FastCos(ang));
        },
        [&](float s, float t) {
            return MapUv(uv, s, t);
        },
        [&](float, float) {
            return color;
        },
        SurfaceOptions{!flip, true});
}

void AddTorus(MeshData& mesh, float R, float r, int segments, int sides, const UvRect& uv, uint32_t color)
{
    AddSurface(
        mesh, segments, sides,
        [&](float s, float t) {
            float a = s * 2.0f * PI;
            float b = t * 2.0f * PI;
            float d = R + r * FastCos(b);
            return Vec3(d * FastCos(a), d * FastSin(a), r * FastSin(b));
        },
        [&](float s, float t) {
            return MapUv(uv, s, t);
        },
        [&](float, float) {
            return color;
        },
        SurfaceOptions{false, true});
}

// Ear-clipping triangulation of a simple counter-clockwise polygon.
static std::vector<int> Triangulate(const std::vector<Vec2>& poly)
{
    std::vector<int> idx;
    std::vector<int> result;
    int n = static_cast<int>(poly.size());
    for (int i = 0; i < n; i++)
        idx.push_back(i);
    int guard = 0;
    while (idx.size() > 3 && guard < 10000) {
        guard++;
        int m = static_cast<int>(idx.size());
        bool clipped = false;
        for (int i = 0; i < m; i++) {
            Vec2 a = poly[idx[(i + m - 1) % m]];
            Vec2 b = poly[idx[i]];
            Vec2 c = poly[idx[(i + 1) % m]];
            float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (cross <= 0.0f)
                continue;
            bool inside = false;
            for (int j = 0; j < m && !inside; j++) {
                if (j == i || j == (i + 1) % m || j == (i + m - 1) % m)
                    continue;
                Vec2 p = poly[idx[j]];
                float d1 = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
                float d2 = (c.x - b.x) * (p.y - b.y) - (c.y - b.y) * (p.x - b.x);
                float d3 = (a.x - c.x) * (p.y - c.y) - (a.y - c.y) * (p.x - c.x);
                inside = d1 >= 0.0f && d2 >= 0.0f && d3 >= 0.0f;
            }
            if (inside)
                continue;
            result.push_back(idx[(i + m - 1) % m]);
            result.push_back(idx[i]);
            result.push_back(idx[(i + 1) % m]);
            idx.erase(idx.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped)
            break;
    }
    if (idx.size() == 3) {
        result.push_back(idx[0]);
        result.push_back(idx[1]);
        result.push_back(idx[2]);
    }
    return result;
}

void AddPrism(MeshData& mesh, const std::vector<Vec2>& polygon, float depth, const UvRect& uv, uint32_t color)
{
    float h = depth * 0.5f;
    int n = static_cast<int>(polygon.size());
    std::vector<int> tris = Triangulate(polygon);
    float minX = polygon[0].x;
    float maxX = polygon[0].x;
    float minY = polygon[0].y;
    float maxY = polygon[0].y;
    for (const Vec2& p : polygon) {
        minX = Min(minX, p.x);
        maxX = Max(maxX, p.x);
        minY = Min(minY, p.y);
        maxY = Max(maxY, p.y);
    }
    for (int face = 0; face < 2; face++) {
        float z = face == 0 ? h : -h;
        Vec3 nrm(0, 0, face == 0 ? 1.0f : -1.0f);
        int base = static_cast<int>(mesh.vertices.size());
        for (const Vec2& p : polygon) {
            Vec2 tc = MapUv(uv, (p.x - minX) / Max(maxX - minX, 1e-6f), (maxY - p.y) / Max(maxY - minY, 1e-6f));
            AddVertex(mesh, Vec3(p.x, p.y, z), nrm, tc.x, tc.y, color);
        }
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            if (face == 0)
                AddTriangle(mesh, base + tris[i], base + tris[i + 1], base + tris[i + 2]);
            else
                AddTriangle(mesh, base + tris[i], base + tris[i + 2], base + tris[i + 1]);
        }
    }
    for (int i = 0; i < n; i++) {
        Vec2 a = polygon[i];
        Vec2 b = polygon[(i + 1) % n];
        Vec3 nrm = Normalize(Vec3(b.y - a.y, a.x - b.x, 0));
        int i0 = AddVertex(mesh, Vec3(a.x, a.y, h), nrm, uv.u0, uv.v0, color);
        int i1 = AddVertex(mesh, Vec3(a.x, a.y, -h), nrm, uv.u0, uv.v1, color);
        int i2 = AddVertex(mesh, Vec3(b.x, b.y, -h), nrm, uv.u1, uv.v1, color);
        int i3 = AddVertex(mesh, Vec3(b.x, b.y, h), nrm, uv.u1, uv.v0, color);
        AddQuad(mesh, i0, i1, i2, i3);
    }
}

void AddCard(MeshData& mesh, const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, const UvRect& uv,
             uint32_t color, bool twoSided)
{
    Vec3 n = Normalize(Cross(p1 - p0, p3 - p0));
    int a = AddVertex(mesh, p0, n, uv.u0, uv.v1, color);
    int b = AddVertex(mesh, p1, n, uv.u1, uv.v1, color);
    int c = AddVertex(mesh, p2, n, uv.u1, uv.v0, color);
    int d = AddVertex(mesh, p3, n, uv.u0, uv.v0, color);
    AddQuad(mesh, a, b, c, d);
    if (!twoSided)
        return;
    a = AddVertex(mesh, p0, -n, uv.u0, uv.v1, color);
    b = AddVertex(mesh, p1, -n, uv.u1, uv.v1, color);
    c = AddVertex(mesh, p2, -n, uv.u1, uv.v0, color);
    d = AddVertex(mesh, p3, -n, uv.u0, uv.v0, color);
    AddQuad(mesh, a, d, c, b);
}

// --- Transforms and merging

void TransformMesh(MeshData& mesh, const Mat4& m, int firstVertex, int firstIndex)
{
    // Cofactors of the upper 3x3: proportional to the inverse transpose, the right matrix for normals.
    float a = m.m[0][0];
    float b = m.m[0][1];
    float c = m.m[0][2];
    float d = m.m[1][0];
    float e = m.m[1][1];
    float f = m.m[1][2];
    float g = m.m[2][0];
    float h = m.m[2][1];
    float k = m.m[2][2];
    Mat4 cof = MatIdentity();
    cof.m[0][0] = e * k - f * h;
    cof.m[0][1] = -(d * k - f * g);
    cof.m[0][2] = d * h - e * g;
    cof.m[1][0] = -(b * k - c * h);
    cof.m[1][1] = a * k - c * g;
    cof.m[1][2] = -(a * h - b * g);
    cof.m[2][0] = b * f - c * e;
    cof.m[2][1] = -(a * f - c * d);
    cof.m[2][2] = a * e - b * d;
    float det = a * cof.m[0][0] + b * cof.m[0][1] + c * cof.m[0][2];
    for (size_t i = firstVertex; i < mesh.vertices.size(); i++) {
        Vertex& v = mesh.vertices[i];
        Vec3 p = TransformPoint(m, Vec3(v.px, v.py, v.pz));
        Vec3 n(v.nx, v.ny, v.nz);
        Vec3 tn(n.x * cof.m[0][0] + n.y * cof.m[0][1] + n.z * cof.m[0][2],
                n.x * cof.m[1][0] + n.y * cof.m[1][1] + n.z * cof.m[1][2],
                n.x * cof.m[2][0] + n.y * cof.m[2][1] + n.z * cof.m[2][2]);
        tn = Normalize(det < 0.0f ? -tn : tn);
        v.px = p.x;
        v.py = p.y;
        v.pz = p.z;
        v.nx = tn.x;
        v.ny = tn.y;
        v.nz = tn.z;
    }
    if (det >= 0.0f)
        return;
    for (size_t i = firstIndex; i + 2 < mesh.indices.size(); i += 3) {
        uint16_t t = mesh.indices[i + 1];
        mesh.indices[i + 1] = mesh.indices[i + 2];
        mesh.indices[i + 2] = t;
    }
}

void Append(MeshData& dst, const MeshData& src, const Mat4& m)
{
    int firstVertex = static_cast<int>(dst.vertices.size());
    int firstIndex = static_cast<int>(dst.indices.size());
    Append(dst, src);
    TransformMesh(dst, m, firstVertex, firstIndex);
}

void Append(MeshData& dst, const MeshData& src)
{
    uint16_t base = static_cast<uint16_t>(dst.vertices.size());
    dst.vertices.insert(dst.vertices.end(), src.vertices.begin(), src.vertices.end());
    for (uint16_t i : src.indices)
        dst.indices.push_back(static_cast<uint16_t>(base + i));
}

void Flatten(MeshData& mesh)
{
    std::vector<Vertex> old = mesh.vertices;
    std::vector<uint16_t> oldIndices = mesh.indices;
    mesh.vertices.clear();
    mesh.indices.clear();
    for (size_t i = 0; i + 2 < oldIndices.size(); i += 3) {
        Vertex a = old[oldIndices[i]];
        Vertex b = old[oldIndices[i + 1]];
        Vertex c = old[oldIndices[i + 2]];
        Vec3 pa(a.px, a.py, a.pz);
        Vec3 n = Normalize(Cross(Vec3(b.px, b.py, b.pz) - pa, Vec3(c.px, c.py, c.pz) - pa));
        Vertex tri[3] = {a, b, c};
        for (Vertex& v : tri) {
            v.nx = n.x;
            v.ny = n.y;
            v.nz = n.z;
            mesh.vertices.push_back(v);
            mesh.indices.push_back(static_cast<uint16_t>(mesh.vertices.size() - 1));
        }
    }
}

void TintMesh(MeshData& mesh, uint32_t c, int firstVertex)
{
    for (size_t i = firstVertex; i < mesh.vertices.size(); i++) {
        uint32_t v = mesh.vertices[i].color;
        uint32_t r = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            uint32_t x = (v >> shift) & 0xFF;
            uint32_t y = (c >> shift) & 0xFF;
            r |= ((x * y + 127) / 255) << shift;
        }
        mesh.vertices[i].color = r;
    }
}

Mat4 Basis(const Vec3& x, const Vec3& y, const Vec3& origin)
{
    Vec3 z = Cross(x, y);
    Mat4 m = MatIdentity();
    m.m[0][0] = x.x;
    m.m[0][1] = x.y;
    m.m[0][2] = x.z;
    m.m[1][0] = y.x;
    m.m[1][1] = y.y;
    m.m[1][2] = y.z;
    m.m[2][0] = z.x;
    m.m[2][1] = z.y;
    m.m[2][2] = z.z;
    m.m[3][0] = origin.x;
    m.m[3][1] = origin.y;
    m.m[3][2] = origin.z;
    return m;
}

Mat4 Place(const Vec3& pos, float yaw, float pitch, float roll, const Vec3& scale)
{
    Quat q = QuatAxisAngle(Vec3(0, 1, 0), yaw) * QuatAxisAngle(Vec3(1, 0, 0), pitch) *
             QuatAxisAngle(Vec3(0, 0, 1), roll);
    return MatTRS(pos, q, scale);
}

} // namespace content
