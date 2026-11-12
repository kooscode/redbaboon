// Render demo: a standalone title that exercises everything in render/render.h (docs/design.md §5) so the renderer
// can be checked in xemu and on a console without the game: sky, a rolling procedural island with vertex colours
// and a detail texture, a translucent scrolling sea with sun glint, spinning lit objects with specular and rim,
// particle bursts (alpha and additive), tracer streaks, a decal with depth bias, and the 2D overlay with FPS.
#include "render/render.h"

#include <stdio.h>
#include <xboxkrnl/xboxkrnl.h>

#include <algorithm>
#include <vector>

namespace {

// --- Constants

// Terrain: TILES x TILES meshes of TILE_QUADS x TILE_QUADS quads over TERRAIN_SIZE metres, centred on the origin.
constexpr int TILES = 8;
constexpr int TILE_QUADS = 24;
constexpr float TERRAIN_SIZE = 4096.0f;
constexpr float MACRO_REPEAT = 600.0f; // metres per tile of the large-scale variation texture
constexpr float DETAIL_REPEAT = 24.0f; // metres per tile of the detail texture

// Sea: one grid of SEA_QUADS x SEA_QUADS over SEA_SIZE metres.
constexpr int SEA_QUADS = 80;
constexpr float SEA_SIZE = 20000.0f;
constexpr float WATER_REPEAT = 48.0f; // metres per water texture tile

// The objects float in a ring around this point, south of the island over the sea; the camera circles it.
constexpr Vec3 SHOW_CENTER = Vec3(0.0f, 140.0f, 1500.0f);
constexpr int OBJECT_COUNT = 6;

// Small objects scattered over the island: a draw-call load like the game's props (about 300 draws in all).
constexpr int PROPS_PER_SIDE = 16;
constexpr float PROP_LOD_DISTANCE = 2400.0f; // boxes nearer, flat impostors farther

constexpr int PARTICLES = 48;

// The sectioned hull: this many separate meshes along HULL_LENGTH metres.
constexpr int HULL_SECTIONS = 12;
constexpr float HULL_LENGTH = 60.0f;

// HUD colours (docs/creative/brief.md §1).
constexpr uint32_t CREAM = 0xFFF4E4C1;
constexpr uint32_t BRASS = 0xFFC9A227;
constexpr uint32_t CRIMSON = 0xFFB3202A;
constexpr int TRACERS = 24;

// --- Procedural content

uint32_t Hash(int x, int y)
{
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// Smooth value noise in [0, 1].
float Noise(float x, float y)
{
    int ix = static_cast<int>(floorf(x));
    int iy = static_cast<int>(floorf(y));
    float fx = x - ix;
    float fy = y - iy;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    auto corner = [&](int dx, int dy) {
        return (Hash(ix + dx, iy + dy) & 0xFFFF) / 65535.0f;
    };
    float top = Lerp(corner(0, 0), corner(1, 0), fx);
    float bottom = Lerp(corner(0, 1), corner(1, 1), fx);
    return Lerp(top, bottom, fy);
}

float Fractal(float x, float y, int octaves)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    for (int i = 0; i < octaves; i++) {
        sum += Noise(x, y) * amplitude;
        x *= 2.03f;
        y *= 2.03f;
        amplitude *= 0.5f;
    }
    return sum;
}

// A volcanic island: a cone with rolling noise, falling under the sea toward the edges.
float TerrainHeight(float x, float z)
{
    float r = sqrtf(x * x + z * z) / (TERRAIN_SIZE * 0.42f);
    float island = Clamp(1.0f - r, -0.4f, 1.0f);
    float cone = island > 0.0f ? island * island * 520.0f : island * 120.0f;
    float hills = (Fractal(x * 0.0021f, z * 0.0021f, 5) - 0.45f) * 260.0f * Clamp(island + 0.3f, 0.0f, 1.0f);
    return cone + hills - 25.0f;
}

Vec3 TerrainNormal(float x, float z)
{
    constexpr float STEP = 4.0f;
    float dx = TerrainHeight(x + STEP, z) - TerrainHeight(x - STEP, z);
    float dz = TerrainHeight(x, z + STEP) - TerrainHeight(x, z - STEP);
    return Normalize(Vec3(-dx, 2.0f * STEP, -dz));
}

Vec3 Hex(uint32_t rgb)
{
    return Vec3(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f);
}

// Sun and sky light baked into the terrain's vertex colours: the terrain never moves and the sun is fixed, so it
// draws with the short unlit vertex program.
Vec3 BakedLight(const Vec3& normal)
{
    render::Environment e;
    Vec3 ambient = Lerp(e.groundAmbient, e.ambient, normal.y * 0.5f + 0.5f);
    return ambient + e.sunColor * std::max(Dot(normal, e.sunDirection), 0.0f);
}

uint32_t TerrainColor(float height, float slope, float variation, const Vec3& light)
{
    Vec3 sand = Hex(0xE9D29A);
    Vec3 wetSand = Hex(0xC2A56E);
    Vec3 grass = Hex(0x7BB043);
    Vec3 jungle = Lerp(Hex(0x2F7A2E), Hex(0x1F5A26), variation);
    Vec3 rock = Hex(0x7A5E4E);
    Vec3 basalt = Hex(0x3B3330);
    Vec3 c;
    if (height < 1.5f)
        c = Lerp(wetSand, sand, Clamp((height + 6.0f) / 7.5f, 0.0f, 1.0f));
    else if (height < 10.0f)
        c = Lerp(sand, grass, (height - 1.5f) / 8.5f);
    else if (height < 60.0f)
        c = Lerp(grass, jungle, (height - 10.0f) / 50.0f);
    else
        c = Lerp(jungle, rock, Clamp((height - 260.0f) / 120.0f, 0.0f, 1.0f));
    c = Lerp(c, Lerp(rock, basalt, variation), Clamp((slope - 0.35f) * 2.5f, 0.0f, 1.0f));
    c = Mul(c, light);
    return PackARGB(c.x, c.y, c.z);
}

MeshData TerrainTile(int tileX, int tileZ)
{
    MeshData mesh;
    float tileSize = TERRAIN_SIZE / TILES;
    float step = tileSize / TILE_QUADS;
    float x0 = -TERRAIN_SIZE * 0.5f + tileX * tileSize;
    float z0 = -TERRAIN_SIZE * 0.5f + tileZ * tileSize;
    for (int j = 0; j <= TILE_QUADS; j++) {
        for (int i = 0; i <= TILE_QUADS; i++) {
            float x = x0 + i * step;
            float z = z0 + j * step;
            float h = TerrainHeight(x, z);
            Vec3 n = TerrainNormal(x, z);
            float variation = Noise(x * 0.02f, z * 0.02f);
            mesh.vertices.push_back({x, h, z, n.x, n.y, n.z, x / MACRO_REPEAT, z / MACRO_REPEAT,
                                     TerrainColor(h, 1.0f - n.y, variation, BakedLight(n))});
        }
    }
    for (int j = 0; j < TILE_QUADS; j++) {
        for (int i = 0; i < TILE_QUADS; i++) {
            uint16_t a = static_cast<uint16_t>(j * (TILE_QUADS + 1) + i);
            uint16_t b = static_cast<uint16_t>(a + 1);
            uint16_t c = static_cast<uint16_t>(a + TILE_QUADS + 1);
            uint16_t d = static_cast<uint16_t>(c + 1);
            // Counter-clockwise seen from above (+Y).
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }
    return mesh;
}

// Sea grid with colours from the depth of the terrain below it: lagoon turquoise in the shallows.
MeshData SeaMesh()
{
    MeshData mesh;
    float step = SEA_SIZE / SEA_QUADS;
    for (int j = 0; j <= SEA_QUADS; j++) {
        for (int i = 0; i <= SEA_QUADS; i++) {
            float x = -SEA_SIZE * 0.5f + i * step;
            float z = -SEA_SIZE * 0.5f + j * step;
            float depth = -TerrainHeight(x, z);
            Vec3 c = Lerp(Hex(0x35C6C0), Hex(0x1B78A8), Clamp(depth / 40.0f, 0.0f, 1.0f));
            c = Lerp(c, Hex(0x0E4C7A), Clamp((depth - 40.0f) / 150.0f, 0.0f, 1.0f));
            float alpha = Lerp(0.55f, 0.85f, Clamp(depth / 60.0f, 0.0f, 1.0f));
            uint32_t color = PackARGB(c.x, c.y, c.z, alpha);
            mesh.vertices.push_back({x, 0.0f, z, 0.0f, 1.0f, 0.0f, x / WATER_REPEAT, z / WATER_REPEAT, color});
        }
    }
    for (int j = 0; j < SEA_QUADS; j++) {
        for (int i = 0; i < SEA_QUADS; i++) {
            uint16_t a = static_cast<uint16_t>(j * (SEA_QUADS + 1) + i);
            uint16_t b = static_cast<uint16_t>(a + 1);
            uint16_t c = static_cast<uint16_t>(a + SEA_QUADS + 1);
            uint16_t d = static_cast<uint16_t>(c + 1);
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }
    return mesh;
}

MeshData Sphere(float radius, int rings, int segments, uint32_t color)
{
    MeshData mesh;
    for (int r = 0; r <= rings; r++) {
        float phi = PI * r / rings;
        for (int s = 0; s <= segments; s++) {
            float theta = 2.0f * PI * s / segments;
            Vec3 n(sinf(phi) * cosf(theta), cosf(phi), sinf(phi) * sinf(theta));
            mesh.vertices.push_back({n.x * radius, n.y * radius, n.z * radius, n.x, n.y, n.z,
                                     static_cast<float>(s) / segments * 4.0f, static_cast<float>(r) / rings * 2.0f,
                                     color});
        }
    }
    for (int r = 0; r < rings; r++) {
        for (int s = 0; s < segments; s++) {
            uint16_t a = static_cast<uint16_t>(r * (segments + 1) + s);
            uint16_t b = static_cast<uint16_t>(a + 1);
            uint16_t c = static_cast<uint16_t>(a + segments + 1);
            uint16_t d = static_cast<uint16_t>(c + 1);
            mesh.indices.insert(mesh.indices.end(), {a, b, c, b, d, c});
        }
    }
    return mesh;
}

MeshData Torus(float major, float minor, int rings, int segments, uint32_t color)
{
    MeshData mesh;
    for (int r = 0; r <= rings; r++) {
        float u = 2.0f * PI * r / rings;
        Vec3 center(cosf(u) * major, 0.0f, sinf(u) * major);
        for (int s = 0; s <= segments; s++) {
            float v = 2.0f * PI * s / segments;
            Vec3 n(cosf(u) * cosf(v), sinf(v), sinf(u) * cosf(v));
            Vec3 p = center + n * minor;
            mesh.vertices.push_back({p.x, p.y, p.z, n.x, n.y, n.z, static_cast<float>(r) / rings * 8.0f,
                                     static_cast<float>(s) / segments, color});
        }
    }
    for (int r = 0; r < rings; r++) {
        for (int s = 0; s < segments; s++) {
            uint16_t a = static_cast<uint16_t>(r * (segments + 1) + s);
            uint16_t b = static_cast<uint16_t>(a + 1);
            uint16_t c = static_cast<uint16_t>(a + segments + 1);
            uint16_t d = static_cast<uint16_t>(c + 1);
            mesh.indices.insert(mesh.indices.end(), {a, b, c, b, d, c});
        }
    }
    return mesh;
}

// One section of a cylinder along Z (from z0 to z1): many small meshes side by side, like the zeppelin's hull.
MeshData CylinderSection(float radius, float z0, float z1, int around, uint32_t color)
{
    MeshData mesh;
    for (int r = 0; r <= 1; r++) {
        float z = r == 0 ? z0 : z1;
        for (int a = 0; a <= around; a++) {
            float angle = 2.0f * PI * a / around;
            Vec3 n(cosf(angle), sinf(angle), 0.0f);
            mesh.vertices.push_back({n.x * radius, n.y * radius, z, n.x, n.y, n.z,
                                     static_cast<float>(a) / around, static_cast<float>(r), color});
        }
    }
    for (int a = 0; a < around; a++) {
        uint16_t i0 = static_cast<uint16_t>(a);
        uint16_t i1 = static_cast<uint16_t>(a + 1);
        uint16_t i2 = static_cast<uint16_t>(a + around + 1);
        uint16_t i3 = static_cast<uint16_t>(a + around + 2);
        // Counter-clockwise seen from outside.
        mesh.indices.insert(mesh.indices.end(), {i0, i1, i2, i1, i3, i2});
    }
    return mesh;
}

MeshData Box(Vec3 half, uint32_t color)
{
    MeshData mesh;
    const Vec3 normals[6] = {Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, -1, 0), Vec3(0, 0, 1),
                             Vec3(0, 0, -1)};
    for (const Vec3& n : normals) {
        Vec3 u = fabsf(n.y) > 0.5f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        Vec3 v = Cross(n, u);
        uint16_t base = static_cast<uint16_t>(mesh.vertices.size());
        const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& c : corners) {
            Vec3 p = n + u * c[0] + v * c[1];
            p = Vec3(p.x * half.x, p.y * half.y, p.z * half.z);
            mesh.vertices.push_back({p.x, p.y, p.z, n.x, n.y, n.z, (c[0] + 1.0f) * 0.5f, (c[1] + 1.0f) * 0.5f, color});
        }
        // u x v = n, so corners 0-1-2 run counter-clockwise seen from outside.
        mesh.indices.insert(mesh.indices.end(), {base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2),
                                                 base, static_cast<uint16_t>(base + 2),
                                                 static_cast<uint16_t>(base + 3)});
    }
    return mesh;
}

// A flat square lying on the XZ plane, facing up (decal, shadow).
MeshData Quad(float half, uint32_t color)
{
    MeshData mesh;
    mesh.vertices.push_back({-half, 0.0f, -half, 0, 1, 0, 0, 0, color});
    mesh.vertices.push_back({half, 0.0f, -half, 0, 1, 0, 1, 0, color});
    mesh.vertices.push_back({half, 0.0f, half, 0, 1, 0, 1, 1, color});
    mesh.vertices.push_back({-half, 0.0f, half, 0, 1, 0, 0, 1, color});
    mesh.indices = {0, 2, 1, 0, 3, 2};
    return mesh;
}

ImageData MakeImage(int width, int height)
{
    ImageData image;
    image.width = width;
    image.height = height;
    image.pixels.resize(width * height);
    return image;
}

// Tileable grey noise around mid grey (neutral for a detail draw), +-contrast; seed picks the pattern.
ImageData NoiseTexture(int seed, float contrast)
{
    constexpr int SIZE = 256;
    ImageData image = MakeImage(SIZE, SIZE);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            // Tileable: the noise wraps with a period of 16 cells.
            float n = 0.0f;
            float amplitude = 0.5f;
            int period = 16;
            for (int o = 0; o < 4; o++) {
                float fx = static_cast<float>(x * period) / SIZE;
                float fy = static_cast<float>(y * period) / SIZE;
                int ix = static_cast<int>(fx);
                int iy = static_cast<int>(fy);
                float tx = fx - ix;
                float ty = fy - iy;
                tx = tx * tx * (3.0f - 2.0f * tx);
                ty = ty * ty * (3.0f - 2.0f * ty);
                auto corner = [&](int dx, int dy) {
                    return (Hash((ix + dx) % period + seed, (iy + dy) % period + o * 1000) & 0xFFFF) / 65535.0f;
                };
                n += Lerp(Lerp(corner(0, 0), corner(1, 0), tx), Lerp(corner(0, 1), corner(1, 1), tx), ty) * amplitude;
                amplitude *= 0.5f;
                period *= 2;
            }
            float v = 0.5f + (n - 0.47f) * 2.0f * contrast;
            image.pixels[y * SIZE + x] = PackARGB(v, v, v);
        }
    }
    return image;
}

// Ripple pattern for the sea around mid grey (tileable sines).
ImageData WaterTexture()
{
    constexpr int SIZE = 128;
    ImageData image = MakeImage(SIZE, SIZE);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            float u = static_cast<float>(x) / SIZE * 2.0f * PI;
            float v = static_cast<float>(y) / SIZE * 2.0f * PI;
            float w = sinf(u * 3.0f + sinf(v * 2.0f) * 1.5f) * 0.5f + sinf(v * 5.0f + u * 2.0f) * 0.3f +
                      sinf((u + v) * 7.0f) * 0.2f;
            float c = 0.5f + w * 0.07f;
            image.pixels[y * SIZE + x] = PackARGB(c, c, c);
        }
    }
    return image;
}

// Crimson livery with cream stripes and dark panel lines.
ImageData LiveryTexture()
{
    constexpr int SIZE = 256;
    constexpr int PANEL = 64;  // panel line spacing
    constexpr int STRIPE = 32; // stripe height
    ImageData image = MakeImage(SIZE, SIZE);
    Vec3 crimson = Hex(0xC1121F);
    Vec3 cream = Hex(0xF2E6C9);
    Vec3 panel = Hex(0x7A0A12);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            Vec3 c = ((y / STRIPE) % 4 == 1) ? cream : crimson;
            if (x % PANEL == 0 || y % PANEL == 0)
                c = panel;
            image.pixels[y * SIZE + x] = PackARGB(c.x, c.y, c.z);
        }
    }
    return image;
}

// 2x2 atlas of soft puffs (white, alpha falls off from the centre, each frame a little different).
ImageData PuffAtlas()
{
    constexpr int FRAME = 64; // frame size in pixels; 2 x 2 frames
    ImageData image = MakeImage(FRAME * 2, FRAME * 2);
    for (int y = 0; y < FRAME * 2; y++) {
        for (int x = 0; x < FRAME * 2; x++) {
            int frame = (y / FRAME) * 2 + x / FRAME;
            float dx = ((x % FRAME) + 0.5f) / (FRAME * 0.5f) - 1.0f;
            float dy = ((y % FRAME) + 0.5f) / (FRAME * 0.5f) - 1.0f;
            float r = sqrtf(dx * dx + dy * dy);
            float n = Fractal(x * 0.15f + frame * 10.0f, y * 0.15f, 3);
            float a = Clamp((1.0f - r) * 1.6f, 0.0f, 1.0f) * Clamp(0.4f + n * 1.2f, 0.0f, 1.0f);
            image.pixels[y * FRAME * 2 + x] = PackARGB(1.0f, 1.0f, 1.0f, a * a);
        }
    }
    return image;
}

// Soft round blob (shadow decal).
ImageData BlobTexture()
{
    constexpr int SIZE = 64;
    ImageData image = MakeImage(SIZE, SIZE);
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            float dx = (x + 0.5f) / (SIZE * 0.5f) - 1.0f;
            float dy = (y + 0.5f) / (SIZE * 0.5f) - 1.0f;
            float a = Clamp(1.0f - sqrtf(dx * dx + dy * dy), 0.0f, 1.0f);
            image.pixels[y * SIZE + x] = PackARGB(1.0f, 1.0f, 1.0f, a * 0.8f);
        }
    }
    return image;
}

double Seconds()
{
    static ULONGLONG frequency = KeQueryPerformanceFrequency();
    return static_cast<double>(KeQueryPerformanceCounter()) / static_cast<double>(frequency);
}

} // namespace

int main()
{
    if (!render::Init()) {
        DbgPrint("render demo: render::Init failed\n");
        return 1;
    }
    DbgPrint("render demo: started\n");

    render::TextureId macro = render::CreateTexture(NoiseTexture(0, 0.10f), true, true);
    render::TextureId detail = render::CreateTexture(NoiseTexture(77, 0.18f), true, true);
    render::TextureId water = render::CreateTexture(WaterTexture(), true, true);
    render::TextureId livery = render::CreateTexture(LiveryTexture(), true, true);
    render::TextureId puff = render::CreateTexture(PuffAtlas(), true, false);
    render::TextureId blob = render::CreateTexture(BlobTexture(), true, false);

    std::vector<render::MeshId> tiles;
    for (int z = 0; z < TILES; z++) {
        for (int x = 0; x < TILES; x++)
            tiles.push_back(render::CreateMesh(TerrainTile(x, z)));
    }
    render::MeshId sea = render::CreateMesh(SeaMesh());
    render::MeshId sphere = render::CreateMesh(Sphere(9.0f, 24, 32, 0xFFFFFFFF));
    render::MeshId torus = render::CreateMesh(Torus(10.0f, 3.5f, 48, 16, 0xFFFFFFFF));
    render::MeshId box = render::CreateMesh(Box(Vec3(8.0f, 5.0f, 12.0f), 0xFFFFFFFF));
    render::MeshId decal = render::CreateMesh(Quad(1.0f, 0xFF1A1410));
    render::MeshId whiteQuad = render::CreateMesh(Quad(1.0f, 0xFFFFFFFF));

    // A hull made of small section meshes (each its own CreateMesh), alternating colours.
    std::vector<render::MeshId> sections;
    for (int i = 0; i < HULL_SECTIONS; i++) {
        float z0 = -HULL_LENGTH * 0.5f + HULL_LENGTH * i / HULL_SECTIONS;
        float z1 = z0 + HULL_LENGTH / HULL_SECTIONS;
        sections.push_back(render::CreateMesh(CylinderSection(6.0f, z0, z1, 16, (i & 1) ? 0xFFA9AEB3 : 0xFF9AA0A6)));
    }
    render::MeshId objects[OBJECT_COUNT] = {sphere, torus, box, sphere, torus, box};
    DbgPrint("render demo: content uploaded\n");

    std::vector<Mat4> props;
    Rng propRng(99);
    for (int z = 0; z < PROPS_PER_SIDE; z++) {
        for (int x = 0; x < PROPS_PER_SIDE; x++) {
            float px = (x - PROPS_PER_SIDE / 2 + propRng.Range(0.0f, 1.0f)) * 120.0f;
            float pz = (z - PROPS_PER_SIDE / 2 + propRng.Range(0.0f, 1.0f)) * 120.0f;
            float h = TerrainHeight(px, pz);
            if (h < 3.0f)
                continue;
            Quat turn = QuatAxisAngle(Vec3(0.0f, 1.0f, 0.0f), propRng.Range(0.0f, 6.28f));
            props.push_back(MatTRS(Vec3(px, h + 2.0f, pz), turn, Vec3(0.5f, 0.6f, 0.5f)));
        }
    }

    render::Environment environment;
    std::vector<render::Billboard> smoke(PARTICLES);
    std::vector<render::Billboard> fire(PARTICLES);
    std::vector<render::Streak> tracers(TRACERS);

    double start = Seconds();
    double fpsTime = start;
    int fpsFrames = 0;
    int fps = 0;
    float cpuMs = 0.0f;
    char line[160];
    for (;;) {
        double frameStart = Seconds();
        float t = static_cast<float>(frameStart - start);

        // Two camera moves, 12 s each: circling the show point close up (objects, particles, tracers), then
        // circling the island from afar (terrain, sea, fog against the sky, the sun's glint).
        render::Camera camera;
        camera.fovY = 58.0f * DEG;
        if (fmodf(t, 24.0f) < 12.0f) {
            float orbit = t * 0.12f;
            camera.position =
                SHOW_CENTER + Vec3(sinf(orbit) * 75.0f, 18.0f + sinf(t * 0.3f) * 10.0f, cosf(orbit) * 75.0f);
            camera.rotation = QuatLook(SHOW_CENTER - camera.position, Vec3(0.0f, 1.0f, 0.0f));
        } else {
            float orbit = t * 0.05f;
            camera.position = Vec3(sinf(orbit) * 2600.0f, 380.0f, cosf(orbit) * 2600.0f);
            camera.rotation = QuatLook(Vec3(0.0f, 60.0f, 0.0f) - camera.position, Vec3(0.0f, 1.0f, 0.0f));
        }

        render::BeginFrame(camera, environment);
        render::DrawSky();

        render::Material ground;
        ground.texture = macro;
        ground.detail = detail;
        ground.detailScale = {MACRO_REPEAT / DETAIL_REPEAT, MACRO_REPEAT / DETAIL_REPEAT};
        ground.unlit = true;
        for (render::MeshId tile : tiles)
            render::DrawMesh(tile, MatIdentity(), ground);

        // Distance LOD: boxes up to PROP_LOD_DISTANCE, flat white impostor quads beyond it.
        render::Material hut;
        hut.texture = livery;
        hut.unlit = true;
        hut.blend = render::Blend::ALPHA_TEST;
        hut.maxDistance = PROP_LOD_DISTANCE;
        render::Material impostor;
        impostor.unlit = true;
        impostor.blend = render::Blend::ALPHA_TEST;
        impostor.cullBackFaces = false;
        impostor.minDistance = PROP_LOD_DISTANCE;
        for (const Mat4& prop : props) {
            render::DrawMesh(box, prop, hut);
            render::DrawMesh(whiteQuad, MatScale(Vec3(6.0f, 6.0f, 6.0f)) * prop, impostor);
        }

        for (int i = 0; i < OBJECT_COUNT; i++) {
            float angle = 2.0f * PI * i / OBJECT_COUNT;
            Vec3 position = SHOW_CENTER + Vec3(cosf(angle) * 34.0f, sinf(t + i) * 4.0f, sinf(angle) * 34.0f);
            Quat spin = QuatAxisAngle(Vec3(0.3f, 1.0f, 0.2f * i), t * (0.6f + 0.15f * i));
            render::Material paint;
            paint.texture = livery;
            paint.specular = 0.6f;
            paint.shininess = 32.0f;
            paint.rim = 0.25f;
            if (i == 3 && fmodf(t, 2.0f) < 0.1f)
                paint.emissive = Vec3(0.8f, 0.8f, 0.8f);
            render::DrawMesh(objects[i], MatTRS(position, spin, Vec3(1.0f, 1.0f, 1.0f)), paint);
        }

        render::Material hullPaint;
        hullPaint.specular = 0.3f;
        hullPaint.hemisphere = true;
        Mat4 hullModel = MatTRS(SHOW_CENTER + Vec3(0.0f, 30.0f, 0.0f), QuatAxisAngle(Vec3(0, 1, 0), t * 0.2f),
                                Vec3(1.0f, 1.0f, 1.0f));
        for (render::MeshId section : sections)
            render::DrawMesh(section, hullModel, hullPaint);

        // Sea last among the solids: translucent, scrolling, strong sun glint.
        render::Material water1;
        water1.texture = water;
        water1.blend = render::Blend::ALPHA;
        water1.specular = 1.0f;
        water1.shininess = 90.0f;
        water1.uvOffset = {t * 0.01f, t * 0.006f};
        water1.detail = water;
        water1.detailScale = {3.7f, 3.7f};
        water1.detailOffset = {-t * 0.023f, t * 0.017f};
        water1.fogDensity = 1.6f;
        render::DrawMesh(sea, MatIdentity(), water1);
        render::DrawSeaGlints(t);

        // A blob shadow on the sea right under the show point, with depth bias.
        render::Material shadow;
        shadow.texture = blob;
        shadow.blend = render::Blend::ALPHA;
        shadow.unlit = true;
        shadow.tint = 0x80FFFFFF;
        shadow.depthBias = 0.3f;
        render::DrawMesh(decal, MatTRS(Vec3(SHOW_CENTER.x, 0.0f, SHOW_CENTER.z), Quat(), Vec3(30, 1, 30)), shadow);

        // Particle burst every 3 s: smoke (alpha) and fire (additive) expanding from the centre.
        float age = fmodf(t, 3.0f);
        Rng rng(1234);
        for (int i = 0; i < PARTICLES; i++) {
            Vec3 direction = Normalize(Vec3(rng.Range(-1, 1), rng.Range(-0.4f, 1), rng.Range(-1, 1)));
            float speed = rng.Range(6.0f, 14.0f);
            float fade = Clamp(1.0f - age / 3.0f, 0.0f, 1.0f);
            smoke[i].position = SHOW_CENTER + direction * (speed * age) + Vec3(0, age * 3.0f, 0);
            smoke[i].size = 4.0f + age * 6.0f;
            smoke[i].rotation = rng.Range(0, 6.28f) + age * 0.5f;
            smoke[i].color = PackARGB(0.23f, 0.2f, 0.18f, fade * 0.8f);
            smoke[i].frame = i & 3;
            float fireFade = Clamp(1.0f - age / 0.8f, 0.0f, 1.0f);
            fire[i].position = SHOW_CENTER + direction * (speed * 0.6f * age);
            fire[i].size = 3.0f + age * 8.0f;
            fire[i].rotation = rng.Range(0, 6.28f);
            fire[i].color = PackARGB(1.0f, 0.7f, 0.25f, fireFade);
            fire[i].frame = (i + 1) & 3;
        }
        render::DrawBillboards(puff, 2, 2, smoke.data(), PARTICLES, render::Blend::ALPHA);
        render::DrawBillboards(puff, 2, 2, fire.data(), PARTICLES, render::Blend::ADDITIVE);

        // Tracers crossing the show point.
        for (int i = 0; i < TRACERS; i++) {
            float phase = fmodf(t * 0.8f + i * 0.13f, 1.0f);
            float lane = (i - TRACERS / 2) * 1.5f;
            Vec3 from = SHOW_CENTER + Vec3(-120.0f + phase * 240.0f, lane * 0.5f + 6.0f, lane);
            tracers[i].from = from;
            tracers[i].to = from + Vec3(10.0f, 0.0f, 0.0f);
            tracers[i].width = 0.5f;
            tracers[i].color = (i & 1) ? 0xFFFFD23F : 0xFFFF5A3C;
        }
        render::DrawStreaks(render::NO_TEXTURE, tracers.data(), TRACERS, render::Blend::ADDITIVE);

        // HUD: a panel inside the title-safe area, shadowed text, the charset for inspection.
        render::FrameStats stats = render::LastFrameStats();
        render::DrawRect(32, 24, 264, 72, 0xA01A1410);
        render::DrawRect(32, 24, 264, 2, BRASS);
        render::DrawRect(32, 94, 264, 2, BRASS);
        render::DrawTextShadow(40, 32, 2, CREAM, "RENDER DEMO");
        int cpuTenths = static_cast<int>(cpuMs * 10.0f);
        int waitTenths = static_cast<int>(stats.gpuWaitMs * 10.0f);
        snprintf(line, sizeof(line), "FPS %d  CPU %d.%d MS  GPU WAIT %d.%d MS", fps, cpuTenths / 10, cpuTenths % 10,
                 waitTenths / 10, waitTenths % 10);
        render::DrawTextShadow(40, 56, 1, CREAM, line);
        snprintf(line, sizeof(line), "DRAWS %d  TRIS %d  CULLED %d", stats.drawCalls, stats.triangles, stats.culled);
        render::DrawTextShadow(40, 72, 1, CREAM, line);
        const char* charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789\n.,:;!?'\"-+=*/\\%#()[]<>_|&$@";
        render::DrawTextShadow(32, 400, 1, CREAM, charset);
        const char* motto = "PERFECTION PERFECTED";
        render::DrawTextShadow(608 - render::TextWidth(3, motto), 424, 3, CRIMSON, motto);

        float sx = 0.0f;
        float sy = 0.0f;
        if (render::Project(SHOW_CENTER, sx, sy)) {
            render::DrawRect(sx - 12, sy - 1, 24, 2, 0xC0F4E4C1);
            render::DrawRect(sx - 1, sy - 12, 2, 24, 0xC0F4E4C1);
        }

        cpuMs = static_cast<float>((Seconds() - frameStart) * 1000.0);
        render::EndFrame();

        fpsFrames++;
        double now = Seconds();
        if (now - fpsTime >= 1.0) {
            fps = static_cast<int>(fpsFrames / (now - fpsTime) + 0.5);
            fpsFrames = 0;
            fpsTime = now;
            DbgPrint("render demo: %d fps, cpu %d.%d ms (gpu wait %d.%d, flip wait %d.%d), %d draws, %d tris, "
                     "%d culled\n",
                     fps, static_cast<int>(cpuMs), static_cast<int>(cpuMs * 10) % 10, static_cast<int>(stats.gpuWaitMs),
                     static_cast<int>(stats.gpuWaitMs * 10) % 10, static_cast<int>(stats.flipWaitMs),
                     static_cast<int>(stats.flipWaitMs * 10) % 10, stats.drawCalls, stats.triangles, stats.culled);
        }
    }
    return 0;
}
