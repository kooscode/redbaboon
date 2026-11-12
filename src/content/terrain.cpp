// The island of the level "Raid on Isla Platano": the height field (volcano with a lava crater, an east-west canyon
// with a river, beaches, the harbour bay, the pirates' rock island, islets, the sea floor), its tiles with vertex
// colours, the sea with depth colours, the canyon rings and the landmarks. docs/design.md §2, §7; creative brief §1.
#include "content/content.h"
#include "content/mesh_builder.h"
#include "content/noise.h"
#include "content/terrain_shape.h"

#include <math.h>

namespace content {

// Map extent and samples.
constexpr float MAP_SIZE = 8000.0f;
constexpr int RESOLUTION = 257;
constexpr float SPACING = MAP_SIZE / (RESOLUTION - 1);
// Tiles: 8 x 8, each 32 x 32 cells.
constexpr int TILES = 8;
constexpr int TILE_CELLS = (RESOLUTION - 1) / TILES;
// Texture repeat of the terrain UVs (metres per tile of the macro texture; the detail is 25 times finer: 16 m).
constexpr float UV_METRES = 400.0f;

// Landmarks.
constexpr float CRATER_RADIUS = 190.0f;
constexpr float LAVA_LEVEL = 520.0f;
constexpr float CANYON_FLOOR = -8.0f;
// Ring count along the canyon.
constexpr int RINGS = 8;

// Islets: x, z, radius, height.
static const Islet ISLETS[] = {
    {-3300.0f, 1500.0f, 280.0f, 38.0f}, {-2650.0f, -3000.0f, 320.0f, 55.0f}, {2700.0f, 2950.0f, 230.0f, 26.0f},
    {3500.0f, 700.0f, 210.0f, 32.0f},   {-1200.0f, -3350.0f, 190.0f, 22.0f}, {-600.0f, 3350.0f, 150.0f, 14.0f},
};

// The terrain, built on first use.
static Terrain g_terrain;
static bool g_built = false;

// --- Shape

float CanyonCenterZ(float x)
{
    return 620.0f + 170.0f * FastSin(x / 650.0f + 0.6f) + 60.0f * FastSin(x / 260.0f);
}

float CanyonHalfWidth(float x)
{
    return 100.0f + 20.0f * FastSin(x / 400.0f);
}

int IsletCount()
{
    return static_cast<int>(sizeof(ISLETS) / sizeof(ISLETS[0]));
}

const Islet& IsletAt(int index)
{
    return ISLETS[index];
}

static float SmoothMax(float a, float b, float k)
{
    float h = Clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return Lerp(a, b, h) + k * h * (1.0f - h);
}

// Normalised distance from the island centre (1 at the coast), with a noisy coastline.
static float CoastDistance(float x, float z)
{
    float n = Fbm(x / 1400.0f + 10.0f, z / 1400.0f + 10.0f, 4, 17u);
    float r = Sqrt(x * x + z * z * 1.17f);
    return r / (2500.0f + 1000.0f * (n - 0.5f));
}

static float IslandHeight(float x, float z)
{
    float d = CoastDistance(x, z);
    float h;
    if (d > 1.0f) {
        h = -4.0f - 50.0f * SmoothStep(1.0f, 1.2f, d) - 150.0f * SmoothStep(1.2f, 1.9f, d);
    } else {
        float coastal = Lerp(-4.0f, 8.0f, SmoothStep(1.0f, 0.93f, d));
        float hills = 150.0f * SmoothStep(0.93f, 0.45f, d);
        float rolling = 110.0f * (Fbm(x / 800.0f, z / 800.0f, 4, 23u) - 0.45f) * SmoothStep(0.97f, 0.7f, d);
        float ridges = 70.0f * RidgedFbm(x / 600.0f + 3.0f, z / 600.0f, 4, 29u) * SmoothStep(0.9f, 0.6f, d);
        h = coastal + hills + rolling + ridges;
        // The plateau rises along the canyon so its walls stand 200-400 m high.
        float dc = Abs(z - CanyonCenterZ(x));
        h += 230.0f * SmoothStep(650.0f, 180.0f, dc) * SmoothStep(0.98f, 0.72f, d);
    }
    // Volcano: a cone with gullies, a crater with a lava lake.
    float dv = Sqrt((x - VOLCANO_X) * (x - VOLCANO_X) + (z - VOLCANO_Z) * (z - VOLCANO_Z));
    if (dv < 1700.0f) {
        float cone = 760.0f * FastPow(Max(1.0f - dv / 1650.0f, 0.0f), 1.5f);
        cone += 35.0f * RidgedFbm(FastAtan2(z - VOLCANO_Z, x - VOLCANO_X) * 4.0f, dv / 160.0f, 3, 31u) *
                SmoothStep(80.0f, 400.0f, cone);
        h = SmoothMax(h, cone, 60.0f);
        float rim = h;
        float inside = SmoothStep(CRATER_RADIUS + 40.0f, CRATER_RADIUS - 60.0f, dv);
        float bowl = LAVA_LEVEL + (rim - LAVA_LEVEL) * FastPow(Clamp(dv / CRATER_RADIUS, 0.0f, 1.0f), 3.0f);
        h = Lerp(h, Min(h, bowl), inside);
        if (dv < CRATER_RADIUS * 0.55f)
            h = Max(LAVA_LEVEL, Min(h, LAVA_LEVEL + 2.0f));
    }
    // Canyon: steep stepped walls down to a river at sea level, open to the sea at both ends.
    if (Abs(x) < 3600.0f) {
        float dc = Abs(z - CanyonCenterZ(x));
        // The wall line wanders +-6 m on a 40 m scale, so the walls are not ruler-straight.
        float hw = CanyonHalfWidth(x) + 12.0f * (ValueNoise(x / 40.0f, z / 40.0f, 39u) - 0.5f);
        if (dc < hw + 90.0f) {
            float t = Clamp((dc - hw) / 90.0f, 0.0f, 1.0f);
            float steps = t + 0.08f * FastSin(t * PI * 6.0f);
            float wall = FastPow(Clamp(steps, 0.0f, 1.0f), 0.55f);
            h = Min(h, Lerp(CANYON_FLOOR + 6.0f * (Fbm(x / 90.0f, z / 90.0f, 2, 37u) - 0.5f), h, wall));
        }
    }
    // Harbour bay on the south shore, with flat ground for the village around it.
    float bx = (x - BAY_X) / BAY_RADIUS_X;
    float bz = (z - BAY_Z) / BAY_RADIUS_Z;
    float e = Sqrt(bx * bx + bz * bz);
    if (e < 2.2f) {
        float village = 3.0f + 40.0f * Max(e - 1.1f, 0.0f) * Max(e - 1.1f, 0.0f) + 12.0f * Max(e - 1.1f, 0.0f);
        float flat = SmoothStep(2.4f, 1.3f, e);
        h = Lerp(h, Min(h, village), flat);
        h = Lerp(Min(h, -14.0f), h, SmoothStep(0.8f, 1.12f, e));
    }
    // The pirates' rock island to the north-east: cliffs around a rocky top.
    float dp = Sqrt((x - PIRATE_X) * (x - PIRATE_X) + (z - PIRATE_Z) * (z - PIRATE_Z));
    if (dp < 700.0f) {
        float rock = -45.0f + 70.0f * SmoothStep(300.0f, 210.0f, dp) +
                     20.0f * SmoothStep(200.0f, 60.0f, dp) + 24.0f * (Fbm(x / 70.0f, z / 70.0f, 3, 41u) - 0.5f);
        h = Max(h, rock);
    }
    for (const Islet& is : ISLETS) {
        float di = Sqrt((x - is.x) * (x - is.x) + (z - is.z) * (z - is.z));
        if (di > is.radius * 1.6f)
            continue;
        float k = SmoothStep(is.radius, is.radius * 0.25f, di);
        float islet = -40.0f + (is.height + 40.0f) * k + 12.0f * (Fbm(x / 60.0f, z / 60.0f, 2, 43u) - 0.5f) * k;
        h = Max(h, islet);
    }
    return h;
}

// --- Terrain sampling

float Terrain::HeightAt(float x, float z) const
{
    float fx = Clamp((x / size + 0.5f) * (resolution - 1), 0.0f, resolution - 1.001f);
    float fz = Clamp((z / size + 0.5f) * (resolution - 1), 0.0f, resolution - 1.001f);
    int ix = static_cast<int>(fx);
    int iz = static_cast<int>(fz);
    float tx = fx - ix;
    float tz = fz - iz;
    const float* row0 = &heights[static_cast<size_t>(iz) * resolution];
    const float* row1 = row0 + resolution;
    float a = row0[ix] + (row0[ix + 1] - row0[ix]) * tx;
    float b = row1[ix] + (row1[ix + 1] - row1[ix]) * tx;
    return a + (b - a) * tz;
}

Vec3 Terrain::NormalAt(float x, float z) const
{
    float s = size / (resolution - 1);
    float dx = HeightAt(x + s, z) - HeightAt(x - s, z);
    float dz = HeightAt(x, z + s) - HeightAt(x, z - s);
    return Normalize(Vec3(-dx, 2.0f * s, -dz));
}

const Terrain& BuildTerrain()
{
    if (g_built)
        return g_terrain;
    g_terrain.size = MAP_SIZE;
    g_terrain.resolution = RESOLUTION;
    g_terrain.heights.resize(static_cast<size_t>(RESOLUTION) * RESOLUTION);
    for (int j = 0; j < RESOLUTION; j++) {
        float z = -MAP_SIZE * 0.5f + j * SPACING;
        for (int i = 0; i < RESOLUTION; i++) {
            float x = -MAP_SIZE * 0.5f + i * SPACING;
            g_terrain.heights[static_cast<size_t>(j) * RESOLUTION + i] = IslandHeight(x, z);
        }
    }
    g_built = true;
    return g_terrain;
}

static float Sample(int i, int j)
{
    i = i < 0 ? 0 : (i >= RESOLUTION ? RESOLUTION - 1 : i);
    j = j < 0 ? 0 : (j >= RESOLUTION ? RESOLUTION - 1 : j);
    return g_terrain.heights[static_cast<size_t>(j) * RESOLUTION + i];
}

static Vec3 SampleNormal(int i, int j)
{
    float dx = Sample(i + 1, j) - Sample(i - 1, j);
    float dz = Sample(i, j + 1) - Sample(i, j - 1);
    return Normalize(Vec3(-dx, 2.0f * SPACING, -dz));
}

// Shading normals of a block of samples starting at (i0, j0), count x count, each the 3 x 3 neighbourhood's sample
// normals weighted 1-2-1 (smooth hills, no facets).
static std::vector<Vec3> SmoothNormals(int i0, int j0, int count, int step)
{
    int m = count + 2;
    std::vector<Vec3> raw(static_cast<size_t>(m) * m);
    for (int j = 0; j < m; j++) {
        for (int i = 0; i < m; i++)
            raw[static_cast<size_t>(j) * m + i] = SampleNormal(i0 + (i - 1) * step, j0 + (j - 1) * step);
    }
    std::vector<Vec3> out(static_cast<size_t>(count) * count);
    for (int j = 0; j < count; j++) {
        for (int i = 0; i < count; i++) {
            Vec3 sum;
            for (int dj = 0; dj < 3; dj++) {
                for (int di = 0; di < 3; di++) {
                    float w = (di == 1 ? 2.0f : 1.0f) * (dj == 1 ? 2.0f : 1.0f);
                    sum += raw[static_cast<size_t>(j + dj) * m + i + di] * w;
                }
            }
            out[static_cast<size_t>(j) * count + i] = Normalize(sum);
        }
    }
    return out;
}

// --- Colours

static Vec3 Col(uint32_t rgb)
{
    return Vec3(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f);
}

// The game bakes this sun and sky light into the terrain's vertex colours (render::Environment's defaults):
// toward the sun, sun colour, sky fill from above, bounce from below.
constexpr Vec3 BAKE_SUN = Vec3(-0.545f, 0.530f, 0.650f);
constexpr Vec3 BAKE_SUN_COLOR = Vec3(1.0f, 0.92f, 0.78f);
constexpr Vec3 BAKE_SKY = Vec3(0.36f, 0.42f, 0.52f);
constexpr Vec3 BAKE_GROUND = Vec3(0.26f, 0.23f, 0.18f);
// Shadowed ground keeps at least this share of the light flat ground gets (a sky fill on cliffs facing away).
constexpr float SHADOW_FLOOR = 0.5f;

// Brightness of the baked light on a surface with normal n (mean of the channels).
static float BakedLight(const Vec3& n)
{
    Vec3 ambient = Lerp(BAKE_GROUND, BAKE_SKY, n.y * 0.5f + 0.5f);
    Vec3 l = ambient + BAKE_SUN_COLOR * Max(Dot(n, BAKE_SUN), 0.0f);
    return (l.x + l.y + l.z) / 3.0f;
}

// Vertex colour of the ground (creative brief §1, review 1 §3): wet and dry sand at the waterline, grass on low
// flats, jungle elsewhere, rock strata on steep slopes, ash and basalt high on the volcano, lava in the crater, dark
// rock on the pirates' island. Noise per vertex and a broad macro variation keep the hills from looking alike. The
// alpha is the jungle weight (1 = jungle) for the renderer's canopy stage; the terrain is drawn opaque.
static uint32_t GroundColor(float x, float z, float h, const Vec3& n)
{
    float noise = Fbm(x / 140.0f, z / 140.0f, 2, 53u);
    float fine = ValueNoise(x / 35.0f, z / 35.0f, 59u);
    float macro = Fbm(x / 600.0f + 5.0f, z / 600.0f, 2, 61u);
    float value50 = ValueNoise(x / 50.0f + 3.0f, z / 50.0f, 67u);
    float value15 = ValueNoise(x / 15.0f, z / 15.0f + 7.0f, 71u);
    Vec3 sand = Col(0xE9D29A);
    Vec3 wet = Col(0xC2A56E);
    Vec3 c;
    // How much of this vertex is jungle (for the canopy stage), carried in the colour's alpha.
    float jungleW = 0.0f;
    if (h < -0.6f) {
        // Below the sea: sand going greener and darker with depth.
        c = Lerp(wet, Lerp(Col(0x8A9A78), Col(0x3A5A5A), SmoothStep(8.0f, 60.0f, -h)), SmoothStep(-0.6f, -12.0f, h));
    } else {
        // Vegetation (review 2 §1): jungle 2F7A2E with value noise of +-12 % at 50 m and +-6 % at 15 m and darker
        // hollows; bright grass only on flats below 80 m near the coast.
        Vec3 jungle = Lerp(Col(0x2F7A2E), Col(0x245F27), SmoothStep(0.5f, 0.8f, noise));
        jungle = jungle * (1.0f + 0.24f * (value50 - 0.5f) + 0.12f * (value15 - 0.5f));
        Vec3 grass = Lerp(Col(0x6FA23C), Col(0x7BB043), SmoothStep(0.3f, 0.7f, macro));
        float coast = SmoothStep(0.72f, 0.85f, CoastDistance(x, z));
        float grassK = SmoothStep(0.88f, 0.95f, n.y) * SmoothStep(80.0f, 40.0f, h) * coast *
                       SmoothStep(0.65f, 0.45f, noise);
        c = Lerp(jungle, grass, grassK);
        jungleW = 1.0f - grassK;
        // Darker green under palm clusters (they are not drawn beyond 600 m; the ground keeps their mark).
        c = Lerp(c, Col(0x2A6B2C), 0.75f * PalmGroundShade(x, z));
        // Beach: dry sand, then the wet band right at the water.
        float beach = SmoothStep(4.5f + 3.0f * noise, 2.0f, h);
        c = Lerp(c, Lerp(sand, wet, SmoothStep(1.4f, 0.4f, h)), beach);
        jungleW *= 1.0f - beach;
        // Rock from 32 to 42 degrees of slope, jittered by +-6 degrees of noise so the edge is not the 31 m grid
        // (the canyon walls also carry a strata skin, props.cpp).
        float jitter = 0.07f * (value50 - 0.5f) * 2.0f + 0.035f * (value15 - 0.5f) * 2.0f;
        float rockK = SmoothStep(0.848f, 0.743f, n.y + jitter);
        int band = static_cast<int>(Floor((h + 4.0f * fine) / 10.0f));
        static const uint32_t STRATA[3] = {0x7A5E4E, 0x6A4F42, 0x8A6E58};
        // Strata bands only on slopes over 45 degrees (faded in over 5), plain rock on gentler ground.
        Vec3 rock = Lerp(Col(0x7A5E4E), Col(STRATA[(band % 3 + 3) % 3]), SmoothStep(0.766f, 0.707f, n.y));
        rock = Lerp(rock, Col(0x8E6A52), 0.25f * fine);
        c = Lerp(c, rock, rockK);
        jungleW *= 1.0f - rockK;
        // Volcano: ash above 380 m darkening to basalt at the rim, glowing lava in the crater.
        float dv = Sqrt((x - VOLCANO_X) * (x - VOLCANO_X) + (z - VOLCANO_Z) * (z - VOLCANO_Z));
        float ashK = SmoothStep(340.0f + 50.0f * noise, 400.0f + 50.0f * noise, h) * SmoothStep(1500.0f, 1100.0f, dv);
        Vec3 ash = Lerp(Col(0x5A5552), Col(0x3B3330), SmoothStep(470.0f, 600.0f, h));
        // Break the ash up so the detail texture's tiling does not show (review 4 §10): broad and fine value noise,
        // and dark lava-rock streaks running down from the rim.
        float ashNoise =
            ValueNoise(x / 60.0f, z / 60.0f + 11.0f, 73u) * 0.6f + ValueNoise(x / 22.0f, z / 22.0f, 79u) * 0.4f;
        ash = ash * (0.86f + 0.28f * ashNoise);
        float around = FastAtan2(z - VOLCANO_Z, x - VOLCANO_X);
        float streak = ValueNoise(around * 9.0f, dv / 260.0f, 83u);
        ash = Lerp(ash, Col(0x3B3330), SmoothStep(0.62f, 0.78f, streak) * SmoothStep(1200.0f, 300.0f, dv));
        ash = Lerp(ash, Col(0x6E6660), 0.25f * fine);
        c = Lerp(c, ash, ashK);
        jungleW *= 1.0f - ashK;
        if (dv < CRATER_RADIUS * 0.6f && h < LAVA_LEVEL + 6.0f) {
            c = Lerp(Col(0xFF6A1A), Col(0xFFB23F), fine);
            jungleW = 0.0f;
        }
        // Pirate rock: dark basalt.
        float dp = Sqrt((x - PIRATE_X) * (x - PIRATE_X) + (z - PIRATE_Z) * (z - PIRATE_Z));
        if (dp < 330.0f && h > 2.0f) {
            c = Lerp(c, Lerp(Col(0x3B3330), Col(0x5A4A40), fine), SmoothStep(330.0f, 260.0f, dp));
            jungleW *= 1.0f - SmoothStep(330.0f, 260.0f, dp);
        }
    }
    // The 2x detail path shows the colour as it is (neutral textures); 0.9 keeps the jungle deep, not neon.
    c = c * (0.9f * (0.92f + 0.16f * macro));
    // Pre-brighten ground facing away from the sun so baked shadows keep a sky fill (canyon walls stay readable).
    float flat = BakedLight(Vec3(0, 1, 0));
    float lit = BakedLight(n);
    if (lit < SHADOW_FLOOR * flat)
        c = c * (SHADOW_FLOOR * flat / Max(lit, 0.05f));
    return PackARGB(c.x, c.y, c.z, jungleW);
}

// Texture coordinates (macro tile per 400 m, the detail 25 times finer): from above on gentle ground, along the
// wall on steep ground (so cliffs are not smeared into vertical streaks): u runs along the wall, v with the height.
static Vec2 GroundUv(float x, float z, float h, const Vec3& n)
{
    if (n.y > 0.72f)
        return Vec2{x / UV_METRES, z / UV_METRES};
    float along = Abs(n.z) > Abs(n.x) ? x : z;
    return Vec2{along / UV_METRES, -h / UV_METRES};
}

// --- Tiles

int TerrainTileCount()
{
    return TILES * TILES;
}

// A tile far under the sea is drawn with a quarter of the samples (it is only seen through the water).
static bool TileIsDeep(int tx, int tz)
{
    float maxH = -1e9f;
    for (int j = tz * TILE_CELLS; j <= (tz + 1) * TILE_CELLS; j++) {
        for (int i = tx * TILE_CELLS; i <= (tx + 1) * TILE_CELLS; i++)
            maxH = Max(maxH, Sample(i, j));
    }
    return maxH < -25.0f;
}

MeshData MakeTerrainTile(int index)
{
    BuildTerrain();
    int tx = index % TILES;
    int tz = index / TILES;
    bool deep = TileIsDeep(tx, tz);
    int step = deep ? 4 : 1;
    int n = TILE_CELLS / step;
    MeshData mesh;
    mesh.vertices.reserve(static_cast<size_t>(n + 1) * (n + 1) + 8 * n);
    mesh.indices.reserve(static_cast<size_t>(n) * n * 6 + 48 * n);
    std::vector<Vec3> normals = SmoothNormals(tx * TILE_CELLS, tz * TILE_CELLS, n + 1, step);
    for (int j = 0; j <= n; j++) {
        for (int i = 0; i <= n; i++) {
            int si = tx * TILE_CELLS + i * step;
            int sj = tz * TILE_CELLS + j * step;
            float x = -MAP_SIZE * 0.5f + si * SPACING;
            float z = -MAP_SIZE * 0.5f + sj * SPACING;
            float h = Sample(si, sj);
            Vec3 nrm = normals[static_cast<size_t>(j) * (n + 1) + i];
            Vec2 uv = GroundUv(x, z, h, nrm);
            AddVertex(mesh, Vec3(x, h, z), nrm, uv.x, uv.y, GroundColor(x, z, h, nrm));
        }
    }
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            int a = j * (n + 1) + i;
            int b = a + 1;
            int c = a + n + 2;
            int d = a + n + 1;
            // Counter-clockwise seen from above (+Y): a(x,z) -> d(x,z+1) -> c -> b.
            if ((i + j) % 2 == 0) {
                AddTriangle(mesh, a, d, c);
                AddTriangle(mesh, a, c, b);
            } else {
                AddTriangle(mesh, a, d, b);
                AddTriangle(mesh, b, d, c);
            }
        }
    }
    // Skirts on the edges hide cracks between full and reduced tiles: a 30 m curtain hanging from each edge that
    // borders a reduced tile (or every edge of a reduced tile), with both faces.
    bool neighbourDeep[4] = {tz > 0 && TileIsDeep(tx, tz - 1), tx < TILES - 1 && TileIsDeep(tx + 1, tz),
                             tz < TILES - 1 && TileIsDeep(tx, tz + 1), tx > 0 && TileIsDeep(tx - 1, tz)};
    for (int edge = 0; edge < 4; edge++) {
        if (!deep && !neighbourDeep[edge])
            continue;
        for (int k = 0; k < n; k++) {
            int i0 = edge == 1 ? n : (edge == 3 ? 0 : k);
            int j0 = edge == 0 ? 0 : (edge == 2 ? n : k);
            int i1 = edge == 0 || edge == 2 ? k + 1 : i0;
            int j1 = edge == 1 || edge == 3 ? k + 1 : j0;
            Vertex va = mesh.vertices[j0 * (n + 1) + i0];
            Vertex vb = mesh.vertices[j1 * (n + 1) + i1];
            Vec3 pa(va.px, va.py, va.pz);
            Vec3 pb(vb.px, vb.py, vb.pz);
            Vec3 down(0.0f, -30.0f, 0.0f);
            AddCard(mesh, pa + down, pb + down, pb, pa, UvRect{va.u, va.v, vb.u, vb.v}, va.color, true);
        }
    }
    return mesh;
}

Vec3 TerrainTileCenter(int index)
{
    BuildTerrain();
    int tx = index % TILES;
    int tz = index / TILES;
    float lo = 1e9f;
    float hi = -1e9f;
    for (int j = tz * TILE_CELLS; j <= (tz + 1) * TILE_CELLS; j++) {
        for (int i = tx * TILE_CELLS; i <= (tx + 1) * TILE_CELLS; i++) {
            lo = Min(lo, Sample(i, j));
            hi = Max(hi, Sample(i, j));
        }
    }
    float tile = MAP_SIZE / TILES;
    return Vec3(-MAP_SIZE * 0.5f + (tx + 0.5f) * tile, (lo + hi) * 0.5f - 15.0f, -MAP_SIZE * 0.5f + (tz + 0.5f) * tile);
}

float TerrainTileRadius(int index)
{
    Vec3 c = TerrainTileCenter(index);
    int tx = index % TILES;
    int tz = index / TILES;
    float hi = -1e9f;
    for (int j = tz * TILE_CELLS; j <= (tz + 1) * TILE_CELLS; j++) {
        for (int i = tx * TILE_CELLS; i <= (tx + 1) * TILE_CELLS; i++)
            hi = Max(hi, Sample(i, j));
    }
    float half = MAP_SIZE / TILES * 0.5f;
    float dy = Max(hi - c.y, 30.0f);
    return Sqrt(2.0f * half * half + dy * dy);
}

// --- Sea

// Finest sea cell, the largest cell allowed inside the map, and the sea's extent.
constexpr float SEA_FINE = 62.5f;
constexpr float SEA_MAP_MAX = 250.0f;
constexpr float SEA_EXTENT = 16000.0f;
// Water repeat (metres per texture tile).
constexpr float WATER_METRES = 32.0f;

struct SeaBuilder {
    MeshData mesh;
    std::vector<int> index; // vertex of each finest-grid point, or -1
    int gridSize;
};

static uint32_t SeaColor(float depth)
{
    Vec3 lagoon = Col(0x35C6C0);
    Vec3 mid = Col(0x1B78A8);
    Vec3 deepC = Col(0x0E4C7A);
    Vec3 c;
    float a;
    if (depth < 2.5f) {
        c = lagoon;
        a = Lerp(0.5f, 0.6f, SmoothStep(-1.0f, 2.5f, depth));
    } else if (depth < 18.0f) {
        float k = SmoothStep(2.5f, 18.0f, depth);
        c = Lerp(lagoon, mid, k);
        a = Lerp(0.6f, 0.82f, k);
    } else {
        float k = SmoothStep(18.0f, 90.0f, depth);
        c = Lerp(mid, deepC, k);
        a = Lerp(0.82f, 1.0f, SmoothStep(18.0f, 140.0f, depth));
    }
    return PackARGB(c.x, c.y, c.z, a);
}

static float TerrainOrSea(float x, float z)
{
    if (Abs(x) > MAP_SIZE * 0.5f || Abs(z) > MAP_SIZE * 0.5f)
        return -220.0f;
    return g_terrain.HeightAt(x, z);
}

static int SeaVertex(SeaBuilder& sb, int gx, int gz)
{
    int& slot = sb.index[static_cast<size_t>(gz) * (sb.gridSize + 1) + gx];
    if (slot >= 0)
        return slot;
    float x = -SEA_EXTENT + gx * SEA_FINE;
    float z = -SEA_EXTENT + gz * SEA_FINE;
    // Depth averaged over 90 m, so the lagoon colours follow the coast as soft curves, not grid steps.
    float depth = -(TerrainOrSea(x, z) * 2.0f + TerrainOrSea(x + 45.0f, z) + TerrainOrSea(x - 45.0f, z) +
                    TerrainOrSea(x, z + 45.0f) + TerrainOrSea(x, z - 45.0f)) / 6.0f;
    slot = AddVertex(sb.mesh, Vec3(x, 0.0f, z), Vec3(0, 1, 0), x / WATER_METRES, z / WATER_METRES, SeaColor(depth));
    return slot;
}

// Lowest and highest terrain over a cell (sampled at the terrain's spacing).
static void CellRange(float x0, float z0, float size, float& lo, float& hi)
{
    lo = 1e9f;
    hi = -1e9f;
    float step = Min(size, SPACING);
    float half = MAP_SIZE * 0.5f;
    // Outside the map the sea floor is deep; only the part over the map needs sampling.
    if (x0 < -half || x0 + size > half || z0 < -half || z0 + size > half)
        lo = -220.0f;
    float xa = Max(x0, -half);
    float xb = Min(x0 + size, half);
    float za = Max(z0, -half);
    float zb = Min(z0 + size, half);
    for (float z = za; z <= zb + 0.1f; z += step) {
        for (float x = xa; x <= xb + 0.1f; x += step) {
            float h = TerrainOrSea(x, z);
            lo = Min(lo, h);
            hi = Max(hi, h);
        }
    }
    if (hi < lo)
        hi = lo;
}

static void SeaCell(SeaBuilder& sb, int gx, int gz, int cells)
{
    float size = cells * SEA_FINE;
    float x0 = -SEA_EXTENT + gx * SEA_FINE;
    float z0 = -SEA_EXTENT + gz * SEA_FINE;
    bool inMap = x0 < MAP_SIZE * 0.5f + 600.0f && x0 + size > -MAP_SIZE * 0.5f - 600.0f &&
                 z0 < MAP_SIZE * 0.5f + 600.0f && z0 + size > -MAP_SIZE * 0.5f - 600.0f;
    float lo = -220.0f;
    float hi = -220.0f;
    if (inMap)
        CellRange(x0, z0, size, lo, hi);
    // Dry land: no water drawn.
    if (lo > 3.0f)
        return;
    bool near = Abs(x0 + size * 0.5f) < 12000.0f && Abs(z0 + size * 0.5f) < 12000.0f;
    bool split = cells > 1 && ((inMap && size > SEA_MAP_MAX) || (inMap && hi > -40.0f) || (near && size > 1000.0f) ||
                               size > 4000.0f);
    if (split) {
        int h = cells / 2;
        SeaCell(sb, gx, gz, h);
        SeaCell(sb, gx + h, gz, h);
        SeaCell(sb, gx, gz + h, h);
        SeaCell(sb, gx + h, gz + h, h);
        return;
    }
    int a = SeaVertex(sb, gx, gz);
    int b = SeaVertex(sb, gx + cells, gz);
    int c = SeaVertex(sb, gx + cells, gz + cells);
    int d = SeaVertex(sb, gx, gz + cells);
    AddTriangle(sb.mesh, a, d, c);
    AddTriangle(sb.mesh, a, c, b);
}

// --- Shore foam

// Height of the coastline contour, and the foam strip's width on the land and sea sides of it.
constexpr float SHORE_LEVEL = 0.2f;
constexpr float FOAM_LAND = 2.5f;
constexpr float FOAM_SEA = 9.0f;
constexpr float FOAM_Y = 0.12f;
// Points of a smoothed coastline closer than this are merged.
constexpr float FOAM_SPACING = 10.0f;

// Crossing of the contour with the sample edge e (bottom edges 2k, left edges 2k + 1 of sample k).
static Vec2 EdgePoint(int e)
{
    int k = e / 2;
    int i = k % RESOLUTION;
    int j = k / RESOLUTION;
    int i2 = (e & 1) ? i : i + 1;
    int j2 = (e & 1) ? j + 1 : j;
    float a = Sample(i, j) - SHORE_LEVEL;
    float b = Sample(i2, j2) - SHORE_LEVEL;
    float t = Clamp(a / (a - b), 0.0f, 1.0f);
    float x = -MAP_SIZE * 0.5f + Lerp(static_cast<float>(i), static_cast<float>(i2), t) * SPACING;
    float z = -MAP_SIZE * 0.5f + Lerp(static_cast<float>(j), static_cast<float>(j2), t) * SPACING;
    return Vec2{x, z};
}

static void Link(std::vector<int>& links, int a, int b)
{
    for (int k = 0; k < 2; k++) {
        if (links[a * 2 + k] < 0) {
            links[a * 2 + k] = b;
            break;
        }
    }
    for (int k = 0; k < 2; k++) {
        if (links[b * 2 + k] < 0) {
            links[b * 2 + k] = a;
            break;
        }
    }
}

// The coastline as chains of points: marching squares on the height field at SHORE_LEVEL.
static std::vector<std::vector<Vec2>> CoastChains()
{
    int edges = RESOLUTION * RESOLUTION * 2;
    std::vector<int> links(static_cast<size_t>(edges) * 2, -1);
    for (int j = 0; j + 1 < RESOLUTION; j++) {
        for (int i = 0; i + 1 < RESOLUTION; i++) {
            bool c00 = Sample(i, j) > SHORE_LEVEL;
            bool c10 = Sample(i + 1, j) > SHORE_LEVEL;
            bool c11 = Sample(i + 1, j + 1) > SHORE_LEVEL;
            bool c01 = Sample(i, j + 1) > SHORE_LEVEL;
            int bottom = (j * RESOLUTION + i) * 2;
            int left = bottom + 1;
            int top = ((j + 1) * RESOLUTION + i) * 2;
            int right = (j * RESOLUTION + i + 1) * 2 + 1;
            int crossing[4];
            int n = 0;
            if (c00 != c10)
                crossing[n++] = bottom;
            if (c10 != c11)
                crossing[n++] = right;
            if (c11 != c01)
                crossing[n++] = top;
            if (c01 != c00)
                crossing[n++] = left;
            if (n == 2) {
                Link(links, crossing[0], crossing[1]);
            } else if (n == 4) {
                // Saddle: join by the cell centre's side.
                float centre = (Sample(i, j) + Sample(i + 1, j) + Sample(i + 1, j + 1) + Sample(i, j + 1)) * 0.25f;
                if ((centre > SHORE_LEVEL) == c00) {
                    Link(links, bottom, right);
                    Link(links, top, left);
                } else {
                    Link(links, bottom, left);
                    Link(links, top, right);
                }
            }
        }
    }
    std::vector<std::vector<Vec2>> chains;
    std::vector<uint8_t> used(static_cast<size_t>(edges), 0);
    // Open chains first (they start at an edge with one neighbour), then closed loops.
    for (int pass = 0; pass < 2; pass++) {
        for (int e = 0; e < edges; e++) {
            int degree = (links[e * 2] >= 0) + (links[e * 2 + 1] >= 0);
            if (used[e] || degree == 0 || (pass == 0 && degree != 1))
                continue;
            std::vector<Vec2> chain;
            int prev = -1;
            int cur = e;
            while (cur >= 0 && !used[cur]) {
                used[cur] = 1;
                chain.push_back(EdgePoint(cur));
                int next = links[cur * 2] != prev ? links[cur * 2] : links[cur * 2 + 1];
                prev = cur;
                cur = next;
            }
            if (chain.size() >= 4)
                chains.push_back(chain);
        }
    }
    return chains;
}

// Chaikin corner cutting (closed: the chain is a loop), then merges points closer than FOAM_SPACING.
static std::vector<Vec2> SmoothChain(const std::vector<Vec2>& in, bool closed)
{
    std::vector<Vec2> pts = in;
    for (int round = 0; round < 2; round++) {
        std::vector<Vec2> out;
        int n = static_cast<int>(pts.size());
        if (!closed)
            out.push_back(pts[0]);
        for (int i = 0; i < (closed ? n : n - 1); i++) {
            Vec2 a = pts[i];
            Vec2 b = pts[(i + 1) % n];
            out.push_back(Vec2{a.x * 0.75f + b.x * 0.25f, a.y * 0.75f + b.y * 0.25f});
            out.push_back(Vec2{a.x * 0.25f + b.x * 0.75f, a.y * 0.25f + b.y * 0.75f});
        }
        if (!closed)
            out.push_back(pts[n - 1]);
        pts = out;
    }
    std::vector<Vec2> sparse;
    for (const Vec2& p : pts) {
        if (!sparse.empty()) {
            Vec2 q = sparse.back();
            if ((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) < FOAM_SPACING * FOAM_SPACING)
                continue;
        }
        sparse.push_back(p);
    }
    return sparse;
}

// Appends the foam strip along every coastline to the sea mesh.
static void AddShoreFoam(MeshData& mesh)
{
    std::vector<std::vector<Vec2>> chains = CoastChains();
    uint32_t inner = PackARGB(0.96f, 0.97f, 0.95f, 0.9f);
    uint32_t outer = PackARGB(0.96f, 0.97f, 0.95f, 0.0f);
    for (const std::vector<Vec2>& raw : chains) {
        Vec2 first = raw.front();
        Vec2 last = raw.back();
        bool closed = (first.x - last.x) * (first.x - last.x) + (first.y - last.y) * (first.y - last.y) <
                      SPACING * SPACING * 2.0f;
        std::vector<Vec2> pts = SmoothChain(raw, closed);
        int n = static_cast<int>(pts.size());
        if (n < 3 || mesh.vertices.size() + n * 2 + 2 > 65000)
            continue;
        int base = static_cast<int>(mesh.vertices.size());
        int count = closed ? n + 1 : n;
        for (int k = 0; k < count; k++) {
            int i = k % n;
            Vec2 a = pts[i == 0 ? (closed ? n - 1 : 0) : i - 1];
            Vec2 b = pts[i == n - 1 ? (closed ? 0 : n - 1) : i + 1];
            Vec2 t = {b.x - a.x, b.y - a.y};
            float len = Sqrt(t.x * t.x + t.y * t.y);
            Vec2 side = {-t.y / Max(len, 1e-3f), t.x / Max(len, 1e-3f)};
            Vec2 p = pts[i];
            // Point the side vector out to sea (downhill).
            if (g_terrain.HeightAt(p.x + side.x * 6.0f, p.y + side.y * 6.0f) >
                g_terrain.HeightAt(p.x - side.x * 6.0f, p.y - side.y * 6.0f)) {
                side.x = -side.x;
                side.y = -side.y;
            }
            Vec3 land(p.x - side.x * FOAM_LAND, FOAM_Y, p.y - side.y * FOAM_LAND);
            Vec3 sea(p.x + side.x * FOAM_SEA, FOAM_Y, p.y + side.y * FOAM_SEA);
            AddVertex(mesh, land, Vec3(0, 1, 0), land.x / WATER_METRES, land.z / WATER_METRES, inner);
            AddVertex(mesh, sea, Vec3(0, 1, 0), sea.x / WATER_METRES, sea.z / WATER_METRES, outer);
        }
        for (int k = 0; k + 1 < count; k++) {
            int l0 = base + k * 2;
            int s0 = l0 + 1;
            int l1 = l0 + 2;
            int s1 = l0 + 3;
            const Vertex& vl0 = mesh.vertices[l0];
            const Vertex& vs0 = mesh.vertices[s0];
            const Vertex& vl1 = mesh.vertices[l1];
            // Counter-clockwise seen from above: the cross product of the two edges must point up.
            float cy = (vs0.pz - vl0.pz) * (vl1.px - vl0.px) - (vs0.px - vl0.px) * (vl1.pz - vl0.pz);
            if (cy > 0.0f) {
                AddTriangle(mesh, l0, s0, s1);
                AddTriangle(mesh, l0, s1, l1);
            } else {
                AddTriangle(mesh, l0, s1, s0);
                AddTriangle(mesh, l0, l1, s1);
            }
        }
    }
}

// --- Sea mesh

MeshData MakeSea()
{
    BuildTerrain();
    SeaBuilder sb;
    sb.gridSize = static_cast<int>(2.0f * SEA_EXTENT / SEA_FINE);
    sb.index.assign(static_cast<size_t>(sb.gridSize + 1) * (sb.gridSize + 1), -1);
    SeaCell(sb, 0, 0, sb.gridSize);
    AddShoreFoam(sb.mesh);
    return sb.mesh;
}

// --- Rings and landmarks

int RingCount()
{
    return RINGS;
}

Vec3 RingPosition(int index)
{
    float x = -2100.0f + index * 600.0f;
    return Vec3(x, 48.0f + 14.0f * FastSin(index * 1.7f), CanyonCenterZ(x));
}

Quat RingRotation(int index)
{
    float x = -2100.0f + index * 600.0f;
    float dzdx = (CanyonCenterZ(x + 1.0f) - CanyonCenterZ(x - 1.0f)) * 0.5f;
    Vec3 along = Normalize(Vec3(1.0f, 0.0f, dzdx));
    // The ring's axis (local Z) runs along the canyon toward the east: forward (-Z) points west.
    return QuatLook(-along, Vec3(0, 1, 0));
}

Vec3 HarbourPosition()
{
    return Vec3(BAY_X, 0.0f, BAY_Z);
}

Vec3 PirateBasePosition()
{
    BuildTerrain();
    return Vec3(PIRATE_X, g_terrain.HeightAt(PIRATE_X, PIRATE_Z) + PIRATE_TOWER, PIRATE_Z);
}

Vec3 VolcanoCraterPosition()
{
    return Vec3(VOLCANO_X, LAVA_LEVEL, VOLCANO_Z);
}

} // namespace content
