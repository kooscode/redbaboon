// Level props on the island: palm clusters, a banana plantation, the harbour village (huts with terracotta roofs,
// a warehouse, piers, boats), the lighthouse, rocks at the canyon and beaches, and the pirates' rock tower base.
// Placement is computed once (positions only); geometry is built per culling group on request.
// docs/design.md §2, §7; creative brief §1 ("from 50 m").
#include "content/content.h"
#include "content/mesh_builder.h"
#include "content/noise.h"
#include "content/props_layout.h"
#include "content/terrain_shape.h"

#include <math.h>

namespace content {

enum class PropKind {
    PALM,
    BANANA_PLANT,
    HUT,
    WAREHOUSE,
    TOWNHOUSE,
    CHURCH,
    WHARF,
    SHIP,
    DOCK,
    BOAT,
    LIGHTHOUSE,
    MOLE,
    WASHING,
    ROCK,
    STRATA,
    ARCH,
    PIRATE_BASE,
};

// One placed prop: where, which way, how big; seed varies its details.
struct PropPlace {
    PropKind kind;
    float x;
    float y;
    float z;
    float yaw;
    float scale;
    float lean;
    uint32_t seed;
};

constexpr int MAX_PLACES = 1024;
// Culling groups: a 4 x 4 grid over the map.
constexpr int GROUP_GRID = 4;
constexpr float MAP_HALF = 4000.0f;
constexpr uint32_t WHITE = 0xFFFFFFFFu;
// The lighthouse mole: length from the shore and the height of its top above the sea.
constexpr float MOLE_LENGTH = 140.0f;
constexpr float MOLE_TOP = 2.5f;
// Canyon strata strips: chunk length along x (one prop each), column spacing, row height, offset off the wall.
// The columns sit on the terrain's sample lines (31.25 m apart), so the strips follow the rock.
constexpr float STRATA_COLUMN = 8000.0f / 256.0f;
constexpr int STRATA_CHUNK_CELLS = 14;
constexpr float STRATA_CHUNK = STRATA_COLUMN * STRATA_CHUNK_CELLS;
// Height of one repeat of the strata texture (twelve beds of 8 m), and how far the skin floats off the rock.
constexpr float STRATA_SLAB = 96.0f;
constexpr float STRATA_OFFSET = 0.8f;
// The rock arch over the canyon: x position, height of the underside at its centre, rock thickness.
constexpr float ARCH_X = 600.0f;
constexpr float ARCH_UNDERSIDE = 100.0f;
constexpr float ARCH_THICKNESS = 20.0f;
// The pirates' fortress on its rock tower: the keep's height above the deck and the mooring mast above the keep.
constexpr float FORTRESS_HEIGHT = 28.0f;
constexpr float MAST_HEIGHT = 46.0f;

// Placements, computed on first use.
static PropPlace g_places[MAX_PLACES];
static int g_placeCount = -1;

// Palm clusters: each a run of PALM places in g_places, drawn as one mesh up close and as impostors far away.
struct PalmCluster {
    float x;
    float z;
    float radius;
    int first;
    int count;
};

constexpr int MAX_CLUSTERS = 128;
static PalmCluster g_clusters[MAX_CLUSTERS];
static int g_clusterCount = 0;
// Palm shade baked into the terrain under the clusters: a raster over the map, 0..255.
constexpr int SHADE_RES = 128;
static uint8_t g_palmShade[SHADE_RES * SHADE_RES];
// The lighthouse lamp (world), set when the lighthouse is placed.
static Vec3 g_lamp;
// The middle of the village (world, ground level), set when the village is placed.
static Vec3 g_village;
// The middle of the wharf's deck (world).
static Vec3 g_wharf;
// Chimney tops (world), for smoke.

// --- Placement

static void AddPlace(PropKind kind, float x, float y, float z, float yaw, float scale, float lean, uint32_t seed)
{
    if (g_placeCount >= MAX_PLACES)
        return;
    g_places[g_placeCount++] = PropPlace{kind, x, y, z, yaw, scale, lean, seed};
}

// Model matrix of a placed prop: turned by its yaw, moved to its position.
static Mat4 PlaceMatrix(const PropPlace& p)
{
    return MatRotate(QuatAxisAngle(Vec3(0, 1, 0), p.yaw)) * MatTranslate(Vec3(p.x, p.y, p.z));
}

static bool InCanyon(float x, float z, float margin)
{
    return Abs(x) < 3600.0f && Abs(z - CanyonCenterZ(x)) < CanyonHalfWidth(x) + margin;
}

static float BayDistance(float x, float z)
{
    float bx = (x - BAY_X) / BAY_RADIUS_X;
    float bz = (z - BAY_Z) / BAY_RADIUS_Z;
    return Sqrt(bx * bx + bz * bz);
}

// A cluster of palms around a centre, each with its own height, lean and turn; recorded as one PalmCluster.
static void PlacePalmCluster(const Terrain& t, Rng& rng, float cx, float cz, int count)
{
    if (g_clusterCount >= MAX_CLUSTERS)
        return;
    int first = g_placeCount;
    float radius = 0.0f;
    for (int i = 0; i < count; i++) {
        float a = rng.Range(0.0f, 2.0f * PI);
        float r = rng.Range(2.0f, 5.0f + count * 1.6f);
        float x = cx + FastCos(a) * r;
        float z = cz + FastSin(a) * r;
        float h = t.HeightAt(x, z);
        if (h < 1.2f || t.NormalAt(x, z).y < 0.8f)
            continue;
        // Palms near the shore lean toward the sea (away from the island centre).
        float seaward = FastAtan2(z, x) + rng.Range(-0.7f, 0.7f);
        AddPlace(PropKind::PALM, x, h - 0.3f, z, seaward, rng.Range(10.0f, 15.0f), rng.Range(0.1f, 0.3f), rng.Next());
        radius = Max(radius, r);
    }
    if (g_placeCount == first)
        return;
    g_clusters[g_clusterCount++] = PalmCluster{cx, cz, radius + 12.0f, first, g_placeCount - first};
}

// Is (x, z) free of the village's buildings within radius r?
static bool VillageClear(float x, float z, float r)
{
    for (int k = 0; k < g_placeCount; k++) {
        const PropPlace& p = g_places[k];
        bool building = p.kind == PropKind::HUT || p.kind == PropKind::WAREHOUSE || p.kind == PropKind::CHURCH ||
                        p.kind == PropKind::TOWNHOUSE;
        if (building && (p.x - x) * (p.x - x) + (p.z - z) * (p.z - z) < r * r)
            return false;
    }
    return true;
}

// The waterline of the bay at angle a around its centre (marching out from inside the bay).
static Vec3 BayShore(const Terrain& t, float a)
{
    float c = FastCos(a);
    float sn = FastSin(a);
    for (float e = 0.6f; e < 2.0f; e += 0.01f) {
        float x = BAY_X + c * e * BAY_RADIUS_X;
        float z = BAY_Z + sn * e * BAY_RADIUS_Z;
        if (t.HeightAt(x, z) > 0.8f)
            return Vec3(x, 0.0f, z);
    }
    return Vec3(BAY_X + c * BAY_RADIUS_X, 0.0f, BAY_Z + sn * BAY_RADIUS_Z);
}

static void PlaceVillage(const Terrain& t, Rng& rng)
{
    // The village sits on the bay's north shore: find the waterline due north of the bay centre.
    float sx = BAY_X;
    float sz = BAY_Z;
    for (int step = 0; step < 200 && t.HeightAt(sx, sz) < 1.0f; step++)
        sz -= 5.0f;
    // Street direction (along the shore) and the way inland.
    Vec3 along(1.0f, 0.0f, 0.0f);
    Vec3 inland(0.0f, 0.0f, -1.0f);
    float face = FastAtan2(-inland.x, -inland.z);
    // The main pier runs out from the shore point; a short jetty further west.
    AddPlace(PropKind::DOCK, sx, 0.0f, sz + 4.0f, face, 1.0f, 0.0f, rng.Next());
    AddPlace(PropKind::DOCK, sx - 170.0f, 0.0f, sz + 4.0f + 6.0f, face, 0.7f, 0.0f, rng.Next());
    // Warehouses either side of the pier head, the church at the back of the village.
    for (int side = -1; side <= 1; side += 2) {
        Vec3 w = Vec3(sx, 0, sz) + along * (side * 34.0f) + inland * 22.0f;
        AddPlace(PropKind::WAREHOUSE, w.x, t.HeightAt(w.x, w.z), w.z, face + side * 0.05f, 1.0f, 0.0f, rng.Next());
    }
    g_village = Vec3(sx, 0.0f, sz) + inland * 60.0f;
    // The waterfront: a long wharf with two cranes, a steamer moored along the pier, tall houses facing the quay.
    AddPlace(PropKind::WHARF, sx, 0.0f, sz + 2.0f, face, 1.0f, 0.0f, rng.Next());
    g_wharf = Vec3(sx, 1.7f, sz + 5.0f);
    // Small boats tied up along the wharf.
    for (int i = 0; i < 4; i++) {
        float a = -70.0f + i * 40.0f + rng.Range(-4.0f, 4.0f);
        if (Abs(a) < 20.0f)
            a += 25.0f;
        Vec3 b = Vec3(sx, 0, sz) + along * a - inland * 14.0f;
        AddPlace(PropKind::BOAT, b.x, 0.0f, b.z, face + PI * 0.5f + rng.Range(-0.1f, 0.1f), rng.Range(0.9f, 1.2f),
                 0.0f, rng.Next());
    }
    // Washing lines strung between posts behind the waterfront.
    for (int i = 0; i < 7; i++) {
        Vec3 w = Vec3(sx, 0, sz) + along * (-150.0f + i * 50.0f + rng.Range(-6.0f, 6.0f)) + inland * 31.0f;
        float h = t.HeightAt(w.x, w.z);
        if (h > 1.0f && h < 25.0f)
            AddPlace(PropKind::WASHING, w.x, h, w.z, face + rng.Range(-0.4f, 0.4f), 1.0f, 0.0f, rng.Next());
    }
    AddPlace(PropKind::SHIP, sx + 15.0f, 0.0f, sz + 34.0f, face, 1.0f, 0.0f, rng.Next());
    for (int i = 0; i < 12; i++) {
        float a = -160.0f + i * 29.0f + rng.Range(-3.0f, 3.0f);
        if (Abs(a) < 50.0f)
            continue;
        Vec3 p = Vec3(sx, 0, sz) + along * a + inland * 20.0f;
        float h = t.HeightAt(p.x, p.z);
        if (h < 1.0f || h > 25.0f)
            continue;
        AddPlace(PropKind::TOWNHOUSE, p.x, h, p.z, face + rng.Range(-0.05f, 0.05f), 1.0f, 0.0f, rng.Next());
    }
    Vec3 church = Vec3(sx, 0, sz) + along * 20.0f + inland * 100.0f;
    AddPlace(PropKind::CHURCH, church.x, t.HeightAt(church.x, church.z), church.z, face + 0.1f, 1.0f, 0.0f,
             rng.Next());
    // Three rows of huts along the shore, three sizes, turned up to 15 degrees off the street.
    for (int row = 0; row < 3; row++) {
        for (float a = -200.0f; a <= 200.0f; a += rng.Range(17.0f, 23.0f)) {
            if (rng.Uniform() < 0.15f)
                continue;
            Vec3 p = Vec3(sx, 0, sz) + along * (a + rng.Range(-3.0f, 3.0f)) + inland * (32.0f + row * 24.0f +
                                                                                        rng.Range(-3.0f, 3.0f));
            float h = t.HeightAt(p.x, p.z);
            if (h < 1.5f || h > 30.0f || t.NormalAt(p.x, p.z).y < 0.9f || !VillageClear(p.x, p.z, 16.0f))
                continue;
            static const float SIZES[3] = {1.0f, 1.25f, 1.55f};
            float size = SIZES[static_cast<int>(rng.Uniform() * 2.99f)];
            AddPlace(PropKind::HUT, p.x, h, p.z, face + rng.Range(-0.26f, 0.26f), size, 0.0f, rng.Next());
        }
    }
    // Boats moored along the pier, and a few at anchor in the bay.
    for (int i = 0; i < 4; i++) {
        float side = i % 2 == 0 ? -1.0f : 1.0f;
        Vec3 p = Vec3(sx, 0, sz) + along * (side * 7.5f) - inland * (14.0f + i * 7.0f);
        AddPlace(PropKind::BOAT, p.x, 0.0f, p.z, face + rng.Range(-0.1f, 0.1f), rng.Range(0.9f, 1.2f), 0.0f,
                 rng.Next());
    }
    int boats = 0;
    for (int i = 0; i < 40 && boats < 3; i++) {
        float a = rng.Range(0.0f, 2.0f * PI);
        float e = rng.Range(0.2f, 0.7f);
        float x = BAY_X + FastCos(a) * e * BAY_RADIUS_X;
        float z = BAY_Z + FastSin(a) * e * BAY_RADIUS_Z;
        if (t.HeightAt(x, z) > -3.0f)
            continue;
        AddPlace(PropKind::BOAT, x, 0.0f, z, rng.Range(0.0f, 2.0f * PI), rng.Range(0.9f, 1.4f), 0.0f, rng.Next());
        boats++;
    }
    // Palm groves at both ends of the waterfront (the quay itself stays open, so the houses show).
    for (int side = -1; side <= 1; side += 2) {
        for (int i = 0; i < 2; i++) {
            Vec3 p = Vec3(sx, 0, sz) + along * (side * (130.0f + i * 60.0f)) + inland * rng.Range(8.0f, 16.0f);
            PlacePalmCluster(t, rng, p.x, p.z, 6 + static_cast<int>(rng.Uniform() * 5.0f));
        }
    }
    // More houses along the bay's east shore, toward the lighthouse mole.
    for (float a = PI * 1.62f; a < PI * 2.15f; a += rng.Range(0.035f, 0.05f)) {
        Vec3 shore = BayShore(t, a);
        Vec3 out = Normalize(Vec3(shore.x - BAY_X, 0.0f, shore.z - BAY_Z));
        for (int row = 0; row < 2; row++) {
            Vec3 p = shore + out * (26.0f + row * 22.0f + rng.Range(-3.0f, 3.0f));
            float h = t.HeightAt(p.x, p.z);
            if (rng.Uniform() < 0.2f || h < 1.5f || h > 30.0f || t.NormalAt(p.x, p.z).y < 0.9f ||
                !VillageClear(p.x, p.z, 16.0f))
                continue;
            float toBay = FastAtan2(-out.x, -out.z);
            static const float SIZES[3] = {1.0f, 1.25f, 1.55f};
            AddPlace(PropKind::HUT, p.x, h, p.z, toBay + rng.Range(-0.26f, 0.26f),
                     SIZES[static_cast<int>(rng.Uniform() * 2.99f)], 0.0f, rng.Next());
        }
    }
    // The lighthouse stands at the end of a stone mole running from the eastern shore into the bay's mouth.
    Vec3 root = BayShore(t, PI * 0.18f);
    Vec3 dir = Normalize(Vec3(BAY_X - BAY_RADIUS_X * 0.2f - root.x, 0.0f, BAY_Z + BAY_RADIUS_Z * 0.6f - root.z));
    float moleYaw = FastAtan2(dir.x, dir.z);
    AddPlace(PropKind::MOLE, root.x, 0.0f, root.z, moleYaw, 1.0f, 0.0f, rng.Next());
    Vec3 end = root + dir * MOLE_LENGTH;
    AddPlace(PropKind::LIGHTHOUSE, end.x, MOLE_TOP, end.z, 0.0f, 1.0f, 0.0f, rng.Next());
    g_lamp = Vec3(end.x, MOLE_TOP + 23.5f, end.z);
    // Two boats tied up inside the mole.
    for (int i = 0; i < 2; i++) {
        Vec3 side = Vec3(-dir.z, 0.0f, dir.x);
        Vec3 b = root + dir * (50.0f + i * 35.0f) + side * 9.0f;
        if (t.HeightAt(b.x, b.z) < -2.0f)
            AddPlace(PropKind::BOAT, b.x, 0.0f, b.z, moleYaw, 1.1f, 0.0f, rng.Next());
    }
}

static void PlacePalms(const Terrain& t, Rng& rng)
{
    // The south shore (the harbour side, seen in the intro) gets the most: about 35 clusters along its beaches.
    int south = 0;
    for (int i = 0; i < 6000 && south < 18; i++) {
        float x = rng.Range(-2600.0f, 2600.0f);
        float z = rng.Range(1100.0f, 2800.0f);
        float h = t.HeightAt(x, z);
        if (h < 1.5f || h > 9.0f || t.NormalAt(x, z).y < 0.9f || !VillageClear(x, z, 14.0f))
            continue;
        PlacePalmCluster(t, rng, x, z, 5 + static_cast<int>(rng.Uniform() * 8.0f));
        south++;
    }
    int clusters = 0;
    for (int i = 0; i < 3000 && clusters < 8; i++) {
        float x = rng.Range(-3000.0f, 3000.0f);
        float z = rng.Range(-3000.0f, 3000.0f);
        float h = t.HeightAt(x, z);
        if (h < 1.5f || h > 60.0f || t.NormalAt(x, z).y < 0.88f || InCanyon(x, z, 80.0f))
            continue;
        // Beaches first: high ground only now and then.
        if (h > 8.0f && rng.Uniform() > 0.2f)
            continue;
        if (BayDistance(x, z) < 1.15f)
            continue;
        PlacePalmCluster(t, rng, x, z, 5 + static_cast<int>(rng.Uniform() * 6.0f));
        clusters++;
    }
    // Palms inland too, on the jungle flats behind the beaches, so low flight has things whipping past.
    int inland = 0;
    for (int i = 0; i < 6000 && inland < 10; i++) {
        float x = rng.Range(-2900.0f, 2900.0f);
        float z = rng.Range(-2900.0f, 2900.0f);
        float h = t.HeightAt(x, z);
        if (h < 8.0f || h > 90.0f || t.NormalAt(x, z).y < 0.92f || InCanyon(x, z, 120.0f) ||
            !VillageClear(x, z, 30.0f))
            continue;
        PlacePalmCluster(t, rng, x, z, 4 + static_cast<int>(rng.Uniform() * 4.0f));
        inland++;
    }
    // A cluster on each islet.
    for (int i = 0; i < IsletCount(); i++) {
        const Islet& is = IsletAt(i);
        PlacePalmCluster(t, rng, is.x, is.z, 4 + i % 3);
    }
}

static void PlacePlantation(const Terrain& t, Rng& rng)
{
    // Rows of banana plants on flat ground west of the village.
    float cx = BAY_X - 1300.0f;
    float cz = BAY_Z - 250.0f;
    for (int i = 0; i < 400; i++) {
        float x = rng.Range(-1800.0f, 400.0f);
        float z = rng.Range(1300.0f, 2300.0f);
        float h = t.HeightAt(x, z);
        if (h > 6.0f && h < 60.0f && t.NormalAt(x, z).y > 0.97f) {
            cx = x;
            cz = z;
            break;
        }
    }
    float dir = rng.Range(0.0f, PI);
    for (int row = 0; row < 5; row++) {
        for (int k = 0; k < 6; k++) {
            float ax = (row - 2.5f) * 7.0f + rng.Range(-1.0f, 1.0f);
            float az = (k - 3.0f) * 6.0f + rng.Range(-1.2f, 1.2f);
            float x = cx + ax * FastCos(dir) - az * FastSin(dir);
            float z = cz + ax * FastSin(dir) + az * FastCos(dir);
            float h = t.HeightAt(x, z);
            if (h < 2.0f)
                continue;
            AddPlace(PropKind::BANANA_PLANT, x, h - 0.1f, z, rng.Range(0.0f, 2.0f * PI), rng.Range(0.8f, 1.2f),
                     0.0f, rng.Next());
        }
    }
}

static void PlaceRocks(const Terrain& t, Rng& rng)
{
    // Boulders on the canyon floor along both walls.
    for (int i = 0; i < 30; i++) {
        float x = rng.Range(-3000.0f, 3000.0f);
        float side = rng.Uniform() < 0.5f ? -1.0f : 1.0f;
        float z = CanyonCenterZ(x) + side * (CanyonHalfWidth(x) + rng.Range(-15.0f, 25.0f));
        float h = t.HeightAt(x, z);
        if (h > 120.0f)
            continue;
        AddPlace(PropKind::ROCK, x, h - 1.0f, z, rng.Range(0.0f, 6.28f), rng.Range(5.0f, 12.0f), 0.0f, rng.Next());
    }
    // Fallen rocks at the canyon's waterline.
    for (int i = 0; i < 14; i++) {
        float x = -2600.0f + i * 400.0f + rng.Range(-120.0f, 120.0f);
        float side = i % 2 == 0 ? -1.0f : 1.0f;
        float z = CanyonCenterZ(x) + side * (CanyonHalfWidth(x) - rng.Range(5.0f, 20.0f));
        AddPlace(PropKind::ROCK, x, t.HeightAt(x, z) - 2.0f, z, rng.Range(0.0f, 6.28f), rng.Range(9.0f, 16.0f), 0.0f,
                 rng.Next());
    }
    // Strata strips on both canyon walls, and the arch.
    for (float x = -4000.0f + STRATA_COLUMN * 42.0f; x < 2700.0f; x += STRATA_CHUNK) {
        AddPlace(PropKind::STRATA, x, 0.0f, CanyonCenterZ(x), 0.0f, 1.0f, 0.0f, rng.Next());
    }
    AddPlace(PropKind::ARCH, ARCH_X, 0.0f, CanyonCenterZ(ARCH_X), 0.0f, 1.0f, 0.0f, rng.Next());
    // Rocks on beaches and rocky slopes.
    int placed = 0;
    for (int i = 0; i < 3000 && placed < 12; i++) {
        float x = rng.Range(-3800.0f, 3800.0f);
        float z = rng.Range(-3800.0f, 3800.0f);
        float h = t.HeightAt(x, z);
        float ny = t.NormalAt(x, z).y;
        if (h < -3.0f || h > 200.0f || ny > 0.92f || ny < 0.6f || BayDistance(x, z) < 1.9f)
            continue;
        AddPlace(PropKind::ROCK, x, h - 0.8f, z, rng.Range(0.0f, 6.28f), rng.Range(3.0f, 9.0f), 0.0f, rng.Next());
        placed++;
    }
}

// Marks the ground under each palm cluster in the shade raster (a soft disc a little larger than the cluster).
static void StampPalmShade()
{
    float cell = 2.0f * MAP_HALF / SHADE_RES;
    for (int c = 0; c < g_clusterCount; c++) {
        const PalmCluster& pc = g_clusters[c];
        float r = pc.radius + 10.0f;
        int i0 = static_cast<int>((pc.x - r + MAP_HALF) / cell);
        int i1 = static_cast<int>((pc.x + r + MAP_HALF) / cell) + 1;
        int j0 = static_cast<int>((pc.z - r + MAP_HALF) / cell);
        int j1 = static_cast<int>((pc.z + r + MAP_HALF) / cell) + 1;
        for (int j = j0 < 0 ? 0 : j0; j <= j1 && j < SHADE_RES; j++) {
            for (int i = i0 < 0 ? 0 : i0; i <= i1 && i < SHADE_RES; i++) {
                float x = -MAP_HALF + (i + 0.5f) * cell - pc.x;
                float z = -MAP_HALF + (j + 0.5f) * cell - pc.z;
                float k = SmoothStep(r + cell * 0.5f, r * 0.4f, Sqrt(x * x + z * z));
                uint8_t& v = g_palmShade[j * SHADE_RES + i];
                v = static_cast<uint8_t>(Max(static_cast<float>(v), k * 255.0f));
            }
        }
    }
}

static void PlaceAll()
{
    if (g_placeCount >= 0)
        return;
    g_placeCount = 0;
    const Terrain& t = BuildTerrain();
    Rng rng(0x7A1A1Au);
    PlaceVillage(t, rng);
    PlacePalms(t, rng);
    PlacePlantation(t, rng);
    PlaceRocks(t, rng);
    // Crags around the pirates' island.
    for (int i = 0; i < 9; i++) {
        float a = i * 0.7f + rng.Range(-0.2f, 0.2f);
        float r = rng.Range(150.0f, 250.0f);
        float x = PIRATE_X + FastCos(a) * r;
        float z = PIRATE_Z + FastSin(a) * r;
        AddPlace(PropKind::ROCK, x, t.HeightAt(x, z) - 4.0f, z, rng.Range(0.0f, 6.28f), rng.Range(16.0f, 30.0f), 0.0f,
                 rng.Next());
    }
    float py = PirateBasePosition().y - PIRATE_TOWER;
    AddPlace(PropKind::PIRATE_BASE, PIRATE_X, py, PIRATE_Z, 0.4f, 1.0f, 0.0f, rng.Next());
    StampPalmShade();
}

Vec3 LighthouseLampPosition()
{
    PlaceAll();
    return g_lamp;
}

Vec3 HarbourVillagePosition()
{
    PlaceAll();
    return g_village;
}

float PalmGroundShade(float x, float z)
{
    PlaceAll();
    float cell = 2.0f * MAP_HALF / SHADE_RES;
    float fx = Clamp((x + MAP_HALF) / cell - 0.5f, 0.0f, SHADE_RES - 1.001f);
    float fz = Clamp((z + MAP_HALF) / cell - 0.5f, 0.0f, SHADE_RES - 1.001f);
    int i = static_cast<int>(fx);
    int j = static_cast<int>(fz);
    float tx = fx - i;
    float tz = fz - j;
    const uint8_t* row = &g_palmShade[j * SHADE_RES + i];
    float a = Lerp(row[0], row[1], tx);
    float b = Lerp(row[SHADE_RES], row[SHADE_RES + 1], tx);
    return Lerp(a, b, tz) / 255.0f;
}

// The emerge shot's frame: u points from the volcano to the tower (outward, north-east), side across it.
static void TowerFrame(Vec3& tower, Vec3& u, Vec3& side)
{
    tower = PirateBasePosition();
    u = Normalize(Vec3(tower.x - VOLCANO_X, 0.0f, tower.z - VOLCANO_Z));
    side = Vec3(-u.z, 0.0f, u.x);
}

void PirateTowerShot(Vec3& eye, Vec3& target)
{
    Vec3 tower;
    Vec3 u;
    Vec3 side;
    TowerFrame(tower, u, side);
    // Low on the island side, looking up past the keep at the mooring mast.
    eye = tower - u * 300.0f + side * 30.0f + Vec3(0.0f, -70.0f, 0.0f);
    target = tower + Vec3(0.0f, FORTRESS_HEIGHT + 10.0f, 0.0f) - side * 40.0f;
}

int ZeppelinEmergePathCount()
{
    return 4;
}

Vec3 ZeppelinEmergePathPoint(int index)
{
    Vec3 tower;
    Vec3 u;
    Vec3 side;
    TowerFrame(tower, u, side);
    float y = tower.y + FORTRESS_HEIGHT + 20.0f;
    // Hidden behind the keep (on the camera's line of sight), sliding out past its side, then toward the volcano.
    const Vec3 points[4] = {tower + u * 380.0f + side * 30.0f, tower + u * 180.0f - side * 150.0f,
                            tower - u * 150.0f - side * 280.0f, tower - u * 700.0f - side * 300.0f};
    Vec3 p = points[index < 0 ? 0 : (index > 3 ? 3 : index)];
    return Vec3(p.x, y, p.z);
}

Vec3 HarbourWharfPosition()
{
    PlaceAll();
    return g_wharf;
}

int PalmCount()
{
    PlaceAll();
    int n = 0;
    for (int c = 0; c < g_clusterCount; c++)
        n += g_clusters[c].count;
    return n;
}

// The palm with the given number, counting through the clusters.
static const PropPlace& NthPalm(int index)
{
    for (int c = 0; c < g_clusterCount; c++) {
        if (index < g_clusters[c].count)
            return g_places[g_clusters[c].first + index];
        index -= g_clusters[c].count;
    }
    return g_places[g_clusters[0].first];
}

PalmInstance Palm(int index)
{
    PlaceAll();
    const PropPlace& p = NthPalm(index);
    Vec3 lean(FastCos(p.yaw), 0.0f, FastSin(p.yaw));
    PalmInstance palm;
    palm.base = Vec3(p.x, p.y, p.z);
    palm.top = palm.base + Vec3(0, p.scale, 0) + lean * (p.lean * p.scale);
    palm.height = p.scale;
    return palm;
}

int PalmClusterCount()
{
    PlaceAll();
    return g_clusterCount;
}

int PalmClusterFirst(int index)
{
    PlaceAll();
    int n = 0;
    for (int c = 0; c < index; c++)
        n += g_clusters[c].count;
    return n;
}

int PalmClusterSize(int index)
{
    PlaceAll();
    return g_clusters[index].count;
}

Vec3 PalmClusterCenter(int index)
{
    PlaceAll();
    const PalmCluster& c = g_clusters[index];
    return Vec3(c.x, BuildTerrain().HeightAt(c.x, c.z) + 7.0f, c.z);
}

float PalmClusterRadius(int index)
{
    PlaceAll();
    return g_clusters[index].radius + 8.0f;
}

// --- Builders: vegetation

// A bent two-sided leaf strip through three points with widths, mapped to an atlas card (base at v1, tip at v0).
static void AddLeaf(MeshData& mesh, const Vec3* pts, const float* widths, int count, const Vec3& side,
                    const UvRect& uv, uint32_t color)
{
    for (int face = 0; face < 2; face++) {
        int base = static_cast<int>(mesh.vertices.size());
        for (int i = 0; i < count; i++) {
            Vec3 along = Normalize(i + 1 < count ? pts[i + 1] - pts[i] : pts[i] - pts[i - 1]);
            Vec3 n = Normalize(Cross(side, along));
            if (face == 1)
                n = -n;
            float v = uv.v1 + (uv.v0 - uv.v1) * static_cast<float>(i) / (count - 1);
            AddVertex(mesh, pts[i] - side * widths[i], n, uv.u0, v, color);
            AddVertex(mesh, pts[i] + side * widths[i], n, uv.u1, v, color);
        }
        for (int i = 0; i + 1 < count; i++) {
            int a = base + i * 2;
            if (face == 0)
                AddQuad(mesh, a, a + 1, a + 3, a + 2);
            else
                AddQuad(mesh, a, a + 2, a + 3, a + 1);
        }
    }
}

// A palm up close: a curved trunk and a crown of crossed cut-out cards (three side views at 60 degrees and a star
// from above), which keep reading as a palm at 300 m. The crown's normals point mostly up, so the baked light is even.
static void BuildPalm(MeshData& mesh, const PropPlace& p)
{
    Rng rng(p.seed);
    float h = p.scale;
    Vec3 base(p.x, p.y, p.z);
    Vec3 lean(FastCos(p.yaw), 0.0f, FastSin(p.yaw));
    float bend = p.lean * h;
    auto trunk = [&](float t) {
        return base + Vec3(0, h * t, 0) + lean * (bend * t * t);
    };
    AddSweep(
        mesh, 3, 5, trunk,
        [](float t) {
            return Lerp(0.34f, 0.2f, t);
        },
        PropRect(PX_TRUNK), 0xFFB09070u);
    Vec3 top = trunk(1.0f);
    float length = h * rng.Range(0.42f, 0.5f);
    float turn = rng.Range(0.0f, PI);
    uint32_t tint = PackARGB(rng.Range(0.88f, 1.0f), 1.0f, rng.Range(0.85f, 1.0f));
    for (int k = 0; k < 3; k++) {
        float a = turn + k * PI / 3.0f;
        Vec3 across(FastCos(a) * length, 0.0f, FastSin(a) * length);
        Vec3 up(0.0f, 0.3f * length, 0.0f);
        Vec3 down(0.0f, -0.7f * length, 0.0f);
        int first = static_cast<int>(mesh.vertices.size());
        AddCard(mesh, top - across + down, top + across + down, top + across + up, top - across + up,
                PropRect(PX_CROWN_SIDE), tint, true);
        for (size_t v = first; v < mesh.vertices.size(); v++) {
            Vertex& vx = mesh.vertices[v];
            Vec3 n = Normalize(Vec3(vx.nx * 0.35f, 1.0f, vx.nz * 0.35f));
            vx.nx = n.x;
            vx.ny = n.y;
            vx.nz = n.z;
        }
    }
    float a = turn + 0.5f;
    Vec3 e1(FastCos(a) * length, 0.0f, FastSin(a) * length);
    Vec3 e2(-e1.z, 0.0f, e1.x);
    Vec3 c = top + Vec3(0.0f, 0.15f, 0.0f);
    AddCard(mesh, c - e1 - e2, c + e1 - e2, c + e1 + e2, c - e1 + e2, PropRect(PX_CROWN_TOP), tint, true);
}

static void BuildBananaPlant(MeshData& mesh, const PropPlace& p)
{
    Rng rng(p.seed);
    float s = p.scale;
    Vec3 base(p.x, p.y, p.z);
    Vec3 top = base + Vec3(0, 2.4f * s, 0);
    AddTube(mesh, base, top, 0.18f * s, 0.13f * s, 4, PropSwatchUv(PS_WHITE), 0xFF6A8A3Au, false);
    for (int i = 0; i < 4; i++) {
        float a = p.yaw + i * 2.0f * PI / 4.0f + rng.Range(-0.3f, 0.3f);
        Vec3 dir(FastCos(a), 0.0f, FastSin(a));
        Vec3 side = Normalize(Cross(Vec3(0, 1, 0), dir));
        Vec3 pts[3] = {top - Vec3(0, 0.3f, 0), top + dir * (1.1f * s) + Vec3(0, 0.6f * s, 0),
                       top + dir * (2.1f * s) + Vec3(0, 0.2f * s, 0)};
        float widths[3] = {0.12f * s, 0.42f * s, 0.15f * s};
        AddLeaf(mesh, pts, widths, 3, side, PropRect(PX_BANANA_LEAF), WHITE);
    }
    MeshData bunch;
    AddEllipsoid(bunch, Vec3(), Vec3(0.25f, 0.4f, 0.25f), 4, 2, PropSwatchUv(PS_YELLOW), WHITE);
    Append(mesh, bunch, MatTranslate(top + Vec3(0.3f * s, -0.7f * s, 0)));
}

// A boulder: a lumpy, smooth-shaded blob (noise on the radius, so the normals follow the lumps), rock texture,
// tinted toward the canyon's strata browns.
static void BuildRock(MeshData& mesh, const PropPlace& p)
{
    MeshData rock;
    float seedX = static_cast<float>(p.seed % 97);
    UvRect r = PropRect(PX_ROCK);
    AddSurface(
        rock, 6, 4,
        [&](float s, float t) {
            float a = s * 2.0f * PI;
            float b = t * PI;
            Vec3 dir(FastSin(b) * FastSin(a), FastCos(b), FastSin(b) * FastCos(a));
            float lump = Fbm(dir.x * 1.6f + seedX, dir.z * 1.6f + dir.y * 1.3f, 3, p.seed);
            float k = 0.7f + 0.55f * lump;
            return Vec3(dir.x * k, dir.y * 0.7f * k, dir.z * 0.85f * k);
        },
        [&](float s, float t) {
            return Vec2{Lerp(r.u0, r.u1, s), Lerp(r.v0, r.v1, t)};
        },
        [&](float, float) {
            float warm = ((p.seed >> 3) % 7) / 6.0f;
            return PackARGB(0.85f + 0.12f * warm, 0.75f + 0.06f * warm, 0.66f);
        },
        SurfaceOptions{true, true});
    Append(mesh, rock, MatScale(Vec3(p.scale, p.scale, p.scale)) * PlaceMatrix(p));
}

// --- Builders: village

// Four walls (front with the door) of a w x d box, wall height wh, mapped to the wall texture.
static void AddWalls(MeshData& mesh, float w, float d, float wh, uint32_t color)
{
    UvRect r = PropRect(PX_WALL);
    float hw = w * 0.5f;
    float hd = d * 0.5f;
    Vec3 corners[4] = {Vec3(-hw, 0, hd), Vec3(hw, 0, hd), Vec3(hw, 0, -hd), Vec3(-hw, 0, -hd)};
    for (int i = 0; i < 4; i++) {
        Vec3 a = corners[i];
        Vec3 b = corners[(i + 1) % 4];
        // The front wall (facing +Z) shows the door; the others only windows (the right part of the texture).
        UvRect wr = i == 0 ? r : UvRect{r.u0 + (r.u1 - r.u0) * 0.5f, r.v0, r.u1, r.v1};
        AddCard(mesh, a, b, b + Vec3(0, wh, 0), a + Vec3(0, wh, 0), wr, color, false);
    }
}

// Hip roof over a w x d box at height wh with an overhang; underside included.
// Terracotta, the default roof colour.
constexpr uint32_t ROOF_TERRACOTTA = 0xFFC4502Eu;

static void AddHipRoof(MeshData& mesh, float w, float d, float wh, float rise, float overhang,
                       uint32_t color = ROOF_TERRACOTTA)
{
    UvRect r = PropRect(PX_ROOF);
    float hw = w * 0.5f + overhang;
    float hd = d * 0.5f + overhang;
    float ridge = Max(hw - hd, 0.0f);
    Vec3 e0(-hw, wh - 0.2f, hd);
    Vec3 e1(hw, wh - 0.2f, hd);
    Vec3 e2(hw, wh - 0.2f, -hd);
    Vec3 e3(-hw, wh - 0.2f, -hd);
    Vec3 r0(-ridge, wh + rise, 0);
    Vec3 r1(ridge, wh + rise, 0);
    AddCard(mesh, e0, e1, r1, r0, r, color, false);
    AddCard(mesh, e2, e3, r0, r1, r, color, false);
    UvRect tri = {r.u0 + (r.u1 - r.u0) * 0.25f, r.v0, r.u0 + (r.u1 - r.u0) * 0.75f, r.v1};
    Vec3 n1 = Normalize(Cross(e2 - e1, r1 - e1));
    int a = AddVertex(mesh, e1, n1, tri.u0, tri.v1, color);
    int b = AddVertex(mesh, e2, n1, tri.u1, tri.v1, color);
    int c = AddVertex(mesh, r1, n1, (tri.u0 + tri.u1) * 0.5f, tri.v0, color);
    AddTriangle(mesh, a, b, c);
    Vec3 n3 = Normalize(Cross(e0 - e3, r0 - e3));
    a = AddVertex(mesh, e3, n3, tri.u0, tri.v1, color);
    b = AddVertex(mesh, e0, n3, tri.u1, tri.v1, color);
    c = AddVertex(mesh, r0, n3, (tri.u0 + tri.u1) * 0.5f, tri.v0, color);
    AddTriangle(mesh, a, b, c);
    // Dark underside of the eaves.
    AddCard(mesh, e3, e2, e1, e0, PropSwatchUv(PS_DARK_WOOD), WHITE, false);
}

static void BuildHut(MeshData& mesh, const PropPlace& p)
{
    Rng rng(p.seed);
    MeshData hut;
    float w = rng.Range(5.5f, 8.0f) * p.scale;
    float d = rng.Range(4.5f, 6.0f) * p.scale;
    float wh = (rng.Uniform() < 0.3f ? 5.6f : 3.3f) * (0.6f + 0.4f * p.scale);
    static const uint32_t WALLS[4] = {0xFFF1E8D6u, 0xFFF5ECDAu, 0xFFEAD9B8u, 0xFFF0D8C8u};
    AddBox(hut, Vec3(0, -0.4f, 0), Vec3(w * 0.5f + 0.25f, 0.6f, d * 0.5f + 0.25f), PropSwatchUv(PS_WHITE),
           0xFF8A7A6Au);
    AddWalls(hut, w, d, wh, WALLS[p.seed % 4]);
    // Mostly terracotta, with blue, green, ochre and plum roofs mixed in.
    static const uint32_t ROOFS[8] = {ROOF_TERRACOTTA, ROOF_TERRACOTTA, ROOF_TERRACOTTA, 0xFFB04428u,
                                      0xFF3A6EA8u, 0xFF2F7A5Au, 0xFFD49A3Au, 0xFF8A3A5Au};
    AddHipRoof(hut, w, d, wh, rng.Range(1.6f, 2.4f), 0.75f, ROOFS[(p.seed >> 4) % 8]);
    // A porch post pair and a shutter colour stripe make each hut a little different.
    if (rng.Uniform() < 0.5f) {
        AddTube(hut, Vec3(-w * 0.3f, 0, d * 0.5f + 0.6f), Vec3(-w * 0.3f, wh - 0.2f, d * 0.5f + 0.6f), 0.1f, 0.1f, 4,
                PropSwatchUv(PS_DARK_WOOD), WHITE, false);
        AddTube(hut, Vec3(w * 0.3f, 0, d * 0.5f + 0.6f), Vec3(w * 0.3f, wh - 0.2f, d * 0.5f + 0.6f), 0.1f, 0.1f, 4,
                PropSwatchUv(PS_DARK_WOOD), WHITE, false);
    }
    Append(mesh, hut, PlaceMatrix(p));
}

static void BuildWarehouse(MeshData& mesh, const PropPlace& p)
{
    MeshData shed;
    float w = 16.0f;
    float d = 10.0f;
    float wh = 6.0f;
    AddBox(shed, Vec3(0, -0.5f, 0), Vec3(w * 0.5f + 0.3f, 0.8f, d * 0.5f + 0.3f), PropSwatchUv(PS_WHITE), 0xFF8A7A6Au);
    AddWalls(shed, w, d, wh, 0xFFE8C8A0u);
    AddHipRoof(shed, w, d, wh, 3.0f, 0.9f);
    // Crates stacked by the door.
    for (int i = 0; i < 3; i++)
        AddBox(shed, Vec3(-5.0f + i * 1.7f, 0.75f, d * 0.5f + 1.5f), Vec3(0.75f, 0.75f, 0.75f), PropRect(PX_CRATE),
               WHITE);
    AddBox(shed, Vec3(-4.2f, 2.25f, d * 0.5f + 1.5f), Vec3(0.75f, 0.75f, 0.75f), PropRect(PX_CRATE), WHITE);
    Append(mesh, shed, PlaceMatrix(p));
}

// A flag on a pole: the pole from foot up height metres, the flag streaming along dir.
static void AddFlagPole(MeshData& mesh, const Vec3& foot, float height, const Vec3& dir, const AtlasRect& flag)
{
    AddTube(mesh, foot, foot + Vec3(0, height, 0), 0.12f, 0.08f, 4, PropSwatchUv(PS_WHITE), 0xFFE0D8C8u, false);
    Vec3 mid = foot + Vec3(0, height - 1.1f, 0);
    Vec3 pts[3] = {mid + dir * 0.1f, mid + dir * 1.8f + Vec3(0, -0.15f, 0), mid + dir * 3.4f + Vec3(0, 0.05f, 0)};
    float widths[3] = {0.95f, 0.95f, 0.9f};
    AddLeaf(mesh, pts, widths, 3, Vec3(0, 1, 0), PropRect(flag), WHITE);
}

// A washing line: two posts, a sagging line and a row of colourful clothes (two-sided cut-out-free cards).
static void BuildWashing(MeshData& mesh, const PropPlace& p)
{
    Rng rng(p.seed);
    MeshData line;
    float half = 5.0f;
    UvRect white = PropSwatchUv(PS_WHITE);
    for (int side = -1; side <= 1; side += 2)
        AddTube(line, Vec3(side * half, 0, 0), Vec3(side * half, 2.6f, 0), 0.07f, 0.06f, 4, PropSwatchUv(PS_DARK_WOOD),
                WHITE, false);
    AddTube(line, Vec3(-half, 2.5f, 0), Vec3(half, 2.5f, 0), 0.02f, 0.02f, 3, PropSwatchUv(PS_INK), WHITE, false);
    static const uint32_t CLOTHES[6] = {0xFFF5F2EAu, 0xFFC1121Fu, 0xFF2A62B8u, 0xFFFFD23Fu, 0xFF2F7A5Au,
                                        0xFFF0C8B8u};
    for (int i = 0; i < 6; i++) {
        float x = -half + 1.0f + i * 1.6f;
        float w = rng.Range(0.5f, 0.75f);
        float hgt = rng.Range(0.7f, 1.2f);
        float sag = 0.12f * (1.0f - (x / half) * (x / half));
        Vec3 a(x - w, 2.5f - sag - hgt, 0.0f);
        Vec3 b(x + w, 2.5f - sag - hgt, 0.0f);
        Vec3 c(x + w, 2.5f - sag, 0.0f);
        Vec3 d(x - w, 2.5f - sag, 0.0f);
        AddCard(line, a, b, c, d, white, CLOTHES[(p.seed + i * 7) % 6], true);
    }
    Append(mesh, line, PlaceMatrix(p));
}

// A tall harbour house of two or three storeys with pastel walls, a coloured roof, a balcony and a flag.
static void BuildTownhouse(MeshData& mesh, const PropPlace& p)
{
    Rng rng(p.seed);
    MeshData house;
    float w = rng.Range(9.0f, 12.0f);
    float d = rng.Range(8.0f, 10.0f);
    float wh = rng.Uniform() < 0.5f ? 8.5f : 11.5f;
    static const uint32_t WALLS[4] = {0xFFF1E8D6u, 0xFFF0D8A8u, 0xFFD8E8E0u, 0xFFF0C8B8u};
    static const uint32_t ROOFS[4] = {ROOF_TERRACOTTA, 0xFF3A6EA8u, 0xFF2F7A5Au, 0xFFB04428u};
    AddBox(house, Vec3(0, -0.4f, 0), Vec3(w * 0.5f + 0.3f, 0.7f, d * 0.5f + 0.3f), PropSwatchUv(PS_WHITE),
           0xFF8A7A6Au);
    AddWalls(house, w, d, wh * 0.5f, WALLS[p.seed % 4]);
    MeshData upper;
    AddWalls(upper, w, d, wh * 0.5f, WALLS[(p.seed + 1) % 4]);
    Append(house, upper, MatTranslate(Vec3(0, wh * 0.5f, 0)));
    AddBox(house, Vec3(0, wh * 0.5f, d * 0.5f + 0.7f), Vec3(w * 0.4f, 0.12f, 0.7f), PropSwatchUv(PS_DARK_WOOD),
           WHITE);
    AddHipRoof(house, w, d, wh, rng.Range(2.4f, 3.2f), 0.6f, ROOFS[(p.seed >> 3) % 4]);
    AddFlagPole(house, Vec3(w * 0.3f, wh + 1.0f, 0.0f), 5.0f, Vec3(1, 0, 0), PX_TOWN_FLAG);
    // A chimney on the roof (its top is listed for smoke).
    Vec3 chimney(-w * 0.25f, wh + 1.6f, -d * 0.2f);
    AddBox(house, chimney, Vec3(0.5f, 1.6f, 0.5f), PropSwatchUv(PS_WHITE), 0xFF9A6A50u);
    Append(mesh, house, PlaceMatrix(p));
}

// A cargo crane on the wharf: a lattice-looking tower, a jib and a hanging hook, hazard yellow.
static void AddCrane(MeshData& mesh, const Vec3& base, float yaw)
{
    MeshData crane;
    uint32_t yellow = 0xFFE8B923u;
    AddBox(crane, Vec3(0, 0.6f, 0), Vec3(2.0f, 0.6f, 2.0f), PropSwatchUv(PS_WHITE), 0xFF3A3A3Eu);
    AddTube(crane, Vec3(0, 1.2f, 0), Vec3(0, 15.0f, 0), 0.7f, 0.55f, 4, PropSwatchUv(PS_WHITE), yellow, false);
    AddBox(crane, Vec3(0, 15.6f, -2.0f), Vec3(1.2f, 1.0f, 1.6f), PropSwatchUv(PS_WHITE), 0xFF2A2A2Eu);
    AddTube(crane, Vec3(0, 16.2f, 2.0f), Vec3(0, 18.5f, 16.0f), 0.45f, 0.3f, 4, PropSwatchUv(PS_WHITE), yellow, false);
    AddTube(crane, Vec3(0, 18.4f, 15.5f), Vec3(0, 8.0f, 15.5f), 0.05f, 0.05f, 3, PropSwatchUv(PS_INK), WHITE, false);
    AddBox(crane, Vec3(0, 7.6f, 15.5f), Vec3(0.5f, 0.4f, 0.5f), PropSwatchUv(PS_WHITE), 0xFF2A2A2Eu);
    Append(mesh, crane, MatRotate(QuatAxisAngle(Vec3(0, 1, 0), yaw)) * MatTranslate(base));
}

// The waterfront: a long plank quay along the shore on pilings, two cranes, stacked crates and flags.
static void BuildWharf(MeshData& mesh, const PropPlace& p)
{
    MeshData wharf;
    UvRect planks = PropRect(PX_PLANKS);
    AddBox(wharf, Vec3(0, 1.4f, 3.0f), Vec3(85.0f, 0.3f, 6.0f), planks, WHITE);
    for (int i = 0; i < 9; i++)
        AddTube(wharf, Vec3(-80.0f + i * 20.0f, -5.0f, 8.6f), Vec3(-80.0f + i * 20.0f, 1.6f, 8.6f), 0.3f, 0.3f, 4,
                PropSwatchUv(PS_DARK_WOOD), WHITE, false);
    for (int side = -1; side <= 1; side += 2)
        AddCrane(wharf, Vec3(side * 30.0f, 1.7f, 4.0f), side > 0 ? 0.3f : -0.3f);
    for (int i = 0; i < 6; i++) {
        float x = -60.0f + i * 22.0f;
        AddBox(wharf, Vec3(x, 2.5f, 1.0f), Vec3(0.9f, 0.9f, 0.9f), PropRect(PX_CRATE), WHITE);
        AddBox(wharf, Vec3(x + 2.0f, 2.5f, 1.2f), Vec3(0.9f, 0.9f, 0.9f), PropRect(PX_CRATE), WHITE);
        AddBox(wharf, Vec3(x + 1.0f, 4.3f, 1.1f), Vec3(0.9f, 0.9f, 0.9f), PropRect(PX_CRATE), WHITE);
    }
    for (int i = 0; i < 4; i++)
        AddFlagPole(wharf, Vec3(-75.0f + i * 50.0f, 1.7f, 8.0f), 9.0f, Vec3(1, 0, 0), PX_TOWN_FLAG);
    Append(mesh, wharf, PlaceMatrix(p));
}

// A coastal steamer moored along the pier (bow toward the place's +Z): white hull with a red boot top, a deck house,
// a red and black funnel, two masts and deck cargo. About 46 m long.
static void BuildShip(MeshData& mesh, const PropPlace& p)
{
    MeshData ship;
    float length = 46.0f;
    float beam = 4.6f;
    float depth = 5.0f;
    UvRect white = PropSwatchUv(PS_WHITE);
    AddSurface(
        ship, 10, 8,
        [&](float s, float t) {
            float z = (t - 0.5f) * length;
            float k = 1.0f - FastPow(Abs(t - 0.4f) / 0.6f, 3.0f);
            float a = (s - 0.5f) * PI;
            return Vec3(beam * k * FastSin(a), 3.2f - depth * FastCos(a) * (0.5f + 0.5f * k), z);
        },
        [&](float, float) {
            return Vec2{white.u0, white.v0};
        },
        [&](float s, float) {
            float down = Abs(s - 0.5f);
            return down < 0.2f ? 0xFFA02A24u : (down > 0.44f ? 0xFF2A2A2Eu : 0xFFF0ECE4u);
        },
        SurfaceOptions{true, false});
    AddBox(ship, Vec3(0, 3.2f, 0), Vec3(beam * 0.92f, 0.1f, length * 0.42f), PropRect(PX_PLANKS), WHITE);
    AddBox(ship, Vec3(0, 5.2f, -4.0f), Vec3(3.2f, 2.0f, 6.0f), white, 0xFFF0ECE4u);
    AddBox(ship, Vec3(0, 7.6f, -6.0f), Vec3(2.6f, 0.5f, 2.6f), white, 0xFF2A62B8u);
    AddTube(ship, Vec3(0, 7.0f, -1.0f), Vec3(0, 12.5f, -1.0f), 1.3f, 1.2f, 8, white, 0xFFC1121Fu, true);
    AddTube(ship, Vec3(0, 12.5f, -1.0f), Vec3(0, 13.6f, -1.0f), 1.25f, 1.25f, 8, white, 0xFF1A1A1Au, true);
    AddTube(ship, Vec3(0, 3.2f, 14.0f), Vec3(0, 16.0f, 14.0f), 0.25f, 0.15f, 4, white, 0xFF4A3220u, false);
    AddTube(ship, Vec3(0, 3.2f, -16.0f), Vec3(0, 14.0f, -16.0f), 0.25f, 0.15f, 4, white, 0xFF4A3220u, false);
    for (int i = 0; i < 4; i++)
        AddBox(ship, Vec3((i % 2) * 2.0f - 1.0f, 4.1f, 6.0f + (i / 2) * 2.2f), Vec3(0.9f, 0.9f, 0.9f),
               PropRect(PX_CRATE), WHITE);
    AddFlagPole(ship, Vec3(0, 3.2f, -21.0f), 4.0f, Vec3(0, 0, -1), PX_TOWN_FLAG);
    Append(mesh, ship, PlaceMatrix(p));
}

// The village church: whitewashed nave with a terracotta roof and a square bell tower with a pyramid cap.
static void BuildChurch(MeshData& mesh, const PropPlace& p)
{
    MeshData church;
    AddBox(church, Vec3(0, -0.6f, 0), Vec3(6.6f, 0.9f, 10.6f), PropSwatchUv(PS_WHITE), 0xFF8A7A6Au);
    MeshData nave;
    AddWalls(nave, 20.0f, 12.0f, 7.0f, 0xFFF1E8D6u);
    AddHipRoof(nave, 20.0f, 12.0f, 7.0f, 4.0f, 0.8f);
    Append(church, nave, MatRotate(QuatAxisAngle(Vec3(0, 1, 0), PI * 0.5f)));
    MeshData tower;
    AddWalls(tower, 5.0f, 5.0f, 17.0f, 0xFFF5ECDAu);
    AddBox(tower, Vec3(0, 14.5f, 2.55f), Vec3(1.0f, 1.4f, 0.05f), PropSwatchUv(PS_INK), WHITE);
    AddBox(tower, Vec3(2.55f, 14.5f, 0), Vec3(0.05f, 1.4f, 1.0f), PropSwatchUv(PS_INK), WHITE);
    AddBox(tower, Vec3(-2.55f, 14.5f, 0), Vec3(0.05f, 1.4f, 1.0f), PropSwatchUv(PS_INK), WHITE);
    AddHipRoof(tower, 5.0f, 5.0f, 17.0f, 4.5f, 0.5f);
    AddTube(tower, Vec3(0, 21.3f, 0), Vec3(0, 23.5f, 0), 0.1f, 0.1f, 4, PropSwatchUv(PS_INK), WHITE, false);
    AddBox(tower, Vec3(0, 22.8f, 0), Vec3(0.6f, 0.08f, 0.08f), PropSwatchUv(PS_INK), WHITE);
    Append(church, tower, MatTranslate(Vec3(0, 0, 12.5f)));
    AddFlagPole(church, Vec3(0, 7.0f, -4.0f), 6.0f, Vec3(1, 0, 0), PX_TOWN_FLAG);
    Append(mesh, church, PlaceMatrix(p));
}

// A pier from the shore out over the water (along the place's +Z after rotation).
static void BuildDock(MeshData& mesh, const PropPlace& p)
{
    MeshData dock;
    float length = 40.0f * p.scale;
    AddBox(dock, Vec3(0, 1.3f, length * 0.5f - 6.0f), Vec3(2.0f, 0.2f, length * 0.5f), PropRect(PX_PLANKS), WHITE);
    int posts = static_cast<int>(length / 6.0f) + 1;
    for (int i = 0; i < posts; i++) {
        float z = -6.0f + i * (length / (posts - 1));
        for (int side = -1; side <= 1; side += 2)
            AddTube(dock, Vec3(side * 1.8f, -5.0f, z), Vec3(side * 1.8f, 1.6f, z), 0.18f, 0.18f, 4,
                    PropSwatchUv(PS_DARK_WOOD), WHITE, false);
    }
    // A T-head at the end with bollards.
    AddBox(dock, Vec3(0, 1.3f, length - 4.0f), Vec3(7.0f, 0.2f, 2.5f), PropRect(PX_PLANKS), WHITE);
    for (int side = -1; side <= 1; side += 2)
        AddTube(dock, Vec3(side * 6.0f, 1.5f, length - 4.0f), Vec3(side * 6.0f, 2.1f, length - 4.0f), 0.25f, 0.3f, 5,
                PropSwatchUv(PS_INK), WHITE, true);
    Append(mesh, dock, PlaceMatrix(p));
}

static void BuildBoat(MeshData& mesh, const PropPlace& p)
{
    static const uint32_t HULLS[5] = {0xFFC1121Fu, 0xFF2A62B8u, 0xFFF5F2EAu, 0xFFE8B923u, 0xFF2F7A2Eu};
    uint32_t hullColor = HULLS[p.seed % 5];
    MeshData boat;
    float length = 9.0f;
    float beam = 1.4f;
    float depth = 1.3f;
    UvRect planks = PropRect(PX_PLANKS);
    AddSurface(
        boat, 8, 6,
        [&](float s, float t) {
            float z = (t - 0.5f) * length;
            float k = 1.0f - FastPow(Abs(t - 0.45f) / 0.55f, 2.2f);
            float a = (s - 0.5f) * PI;
            return Vec3(beam * k * FastSin(a), 0.8f - depth * FastCos(a) * (0.4f + 0.6f * k), z);
        },
        [&](float s, float t) {
            return MapUv(planks, s, t * 2.0f);
        },
        [&](float s, float) {
            return Abs(s - 0.5f) > 0.42f ? 0xFFF5F2EAu : hullColor;
        },
        SurfaceOptions{true, false});
    // Deck, a little cabin, a mast on half of them.
    AddBox(boat, Vec3(0, 0.75f, 0), Vec3(beam * 0.8f, 0.05f, length * 0.38f), planks, WHITE);
    AddBox(boat, Vec3(0, 1.4f, 1.0f), Vec3(0.8f, 0.65f, 1.2f), PropSwatchUv(PS_WHITE), 0xFFF1E8D6u);
    if (p.seed & 2)
        AddTube(boat, Vec3(0, 0.8f, -1.5f), Vec3(0, 7.0f, -1.5f), 0.1f, 0.07f, 4, PropSwatchUv(PS_DARK_WOOD), WHITE,
                false);
    Append(mesh, boat, MatScale(Vec3(p.scale, p.scale, p.scale)) * PlaceMatrix(p));
}

static void BuildLighthouse(MeshData& mesh, const PropPlace& p)
{
    MeshData tower;
    UvRect white = PropSwatchUv(PS_WHITE);
    // Tower in three bands (white, red, white), gallery, lamp room, red cap.
    std::vector<Vec2> lower = {{3.0f, 8.0f}, {3.3f, -1.0f}};
    std::vector<Vec2> band = {{2.65f, 13.0f}, {2.95f, 8.0f}};
    std::vector<Vec2> upper = {{2.3f, 20.0f}, {2.65f, 13.0f}};
    AddLathe(tower, lower, 12, white, 0xFFF5F2EAu, false);
    AddLathe(tower, band, 12, white, 0xFFC1121Fu, false);
    AddLathe(tower, upper, 12, white, 0xFFF5F2EAu, false);
    AddTube(tower, Vec3(0, 20.0f, 0), Vec3(0, 20.6f, 0), 3.2f, 3.2f, 12, PropSwatchUv(PS_INK), WHITE, true);
    AddTube(tower, Vec3(0, 20.6f, 0), Vec3(0, 23.6f, 0), 1.7f, 1.7f, 10, PropSwatchUv(PS_LAMP), WHITE, false);
    std::vector<Vec2> cap = {{0.0f, 26.6f}, {1.0f, 25.8f}, {2.0f, 23.6f}};
    AddLathe(tower, cap, 10, white, 0xFFC1121Fu, false);
    AddTube(tower, Vec3(0, 26.4f, 0), Vec3(0, 27.8f, 0), 0.12f, 0.05f, 4, PropSwatchUv(PS_INK), WHITE, false);
    // Railing ring.
    MeshData rail;
    AddTorus(rail, 3.1f, 0.06f, 16, 3, PropSwatchUv(PS_INK), WHITE);
    Append(tower, rail, Place(Vec3(0, 21.6f, 0), 0.0f, PI * 0.5f, 0.0f));
    Append(mesh, tower, PlaceMatrix(p));
}

// The stone mole: a rough causeway from the shore out along the place's +Z, and a round platform at its end for
// the lighthouse.
static void BuildMole(MeshData& mesh, const PropPlace& p)
{
    Rng rng(p.seed);
    MeshData mole;
    UvRect stone = PropSwatchUv(PS_WHITE);
    for (int i = 0; i < 4; i++) {
        float z0 = i * MOLE_LENGTH / 4.0f;
        AddBox(mole, Vec3(rng.Range(-0.5f, 0.5f), (MOLE_TOP - 8.0f) * 0.5f, z0 + MOLE_LENGTH / 8.0f),
               Vec3(5.0f, (MOLE_TOP + 8.0f) * 0.5f, MOLE_LENGTH / 8.0f + 1.0f), stone,
               i % 2 == 0 ? 0xFFB8B0A4u : 0xFFAEA698u);
    }
    AddTube(mole, Vec3(0, -8.0f, MOLE_LENGTH), Vec3(0, MOLE_TOP, MOLE_LENGTH), 9.0f, 8.5f, 12, stone, 0xFFC0B8AAu,
            true);
    Append(mesh, mole, PlaceMatrix(p));
    // Tumbled blocks along both sides.
    for (int i = 0; i < 10; i++) {
        float side = i % 2 == 0 ? -1.0f : 1.0f;
        float z = rng.Range(5.0f, MOLE_LENGTH - 5.0f);
        Vec3 local(side * rng.Range(5.0f, 7.0f), -0.5f, z);
        Vec3 w = TransformPoint(PlaceMatrix(p), local);
        PropPlace block = {PropKind::ROCK, w.x, w.y, w.z, rng.Range(0.0f, 6.28f), rng.Range(2.5f, 4.0f), 0.0f,
                           rng.Next()};
        BuildRock(mesh, block);
    }
}

// --- Builders: the canyon

// Keeps the part of a convex polygon with y >= y0 (keepAbove) or y <= y0.
static int ClipY(const Vec3* in, int n, float y0, bool keepAbove, Vec3* out)
{
    int m = 0;
    for (int k = 0; k < n; k++) {
        const Vec3& a = in[k];
        const Vec3& b = in[(k + 1) % n];
        float da = keepAbove ? a.y - y0 : y0 - a.y;
        float db = keepAbove ? b.y - y0 : y0 - b.y;
        if (da >= 0.0f)
            out[m++] = a;
        if ((da >= 0.0f) != (db >= 0.0f))
            out[m++] = Lerp(a, b, da / (da - db));
    }
    return m;
}

// Lays one terrain triangle's skin of strata: the triangle cut into horizontal slabs STRATA_SLAB high, each slab
// mapped onto the whole strata texture (u across the sample cell starting at cellX, v down from the slab's top),
// lifted STRATA_OFFSET off the rock. Coplanar with the rock, so the rock never pokes through.
static void AddStrataTriangle(MeshData& mesh, const Terrain& t, const Vec3& a, const Vec3& b, const Vec3& c,
                              float cellX)
{
    float lo = Min(a.y, Min(b.y, c.y));
    float hi = Max(a.y, Max(b.y, c.y));
    UvRect uv = PropRect(PX_STRATA);
    Vec3 tri[3] = {a, b, c};
    for (float y0 = Floor(lo / STRATA_SLAB) * STRATA_SLAB; y0 < hi; y0 += STRATA_SLAB) {
        Vec3 lower[5];
        Vec3 slab[6];
        int k = ClipY(tri, 3, y0, true, lower);
        k = ClipY(lower, k, y0 + STRATA_SLAB, false, slab);
        if (k < 3)
            continue;
        int base = static_cast<int>(mesh.vertices.size());
        for (int i = 0; i < k; i++) {
            // Lifted along the terrain's (continuous) normal, so neighbouring pieces meet without cracks.
            Vec3 vn = t.NormalAt(slab[i].x, slab[i].z);
            Vec3 p = slab[i] + vn * STRATA_OFFSET;
            float u = Clamp((slab[i].x - cellX) / STRATA_COLUMN, 0.0f, 1.0f);
            float v = Clamp((y0 + STRATA_SLAB - slab[i].y) / STRATA_SLAB, 0.0f, 1.0f);
            AddVertex(mesh, p, vn, Lerp(uv.u0, uv.u1, u), Lerp(uv.v0, uv.v1, v), WHITE);
        }
        for (int i = 1; i + 1 < k; i++)
            AddTriangle(mesh, base, base + i, base + i + 1);
    }
}

// Strata over the steep canyon walls of one stretch (review 2 §8, review 3 §7): every terrain triangle steeper than
// about 50 degrees near the canyon gets a skin of the strata texture.
static void BuildStrata(MeshData& mesh, const PropPlace& p)
{
    const Terrain& t = BuildTerrain();
    int res = t.resolution;
    float half = t.size * 0.5f;
    int i0 = static_cast<int>((p.x + half) / STRATA_COLUMN + 0.5f);
    for (int i = i0; i < i0 + STRATA_CHUNK_CELLS && i + 1 < res; i++) {
        float x = -half + i * STRATA_COLUMN;
        float zc = CanyonCenterZ(x);
        float reach = CanyonHalfWidth(x) + 130.0f;
        int j0 = static_cast<int>((zc - reach + half) / STRATA_COLUMN);
        int j1 = static_cast<int>((zc + reach + half) / STRATA_COLUMN) + 1;
        for (int j = j0 < 0 ? 0 : j0; j < j1 && j + 1 < res; j++) {
            auto at = [&](int ii, int jj) {
                return Vec3(-half + ii * STRATA_COLUMN, t.heights[static_cast<size_t>(jj) * res + ii],
                            -half + jj * STRATA_COLUMN);
            };
            Vec3 va = at(i, j);
            Vec3 vb = at(i + 1, j);
            Vec3 vc = at(i + 1, j + 1);
            Vec3 vd = at(i, j + 1);
            // The same split as the terrain tiles.
            Vec3 tris[2][3] = {{va, vd, vc}, {va, vc, vb}};
            if ((i + j) % 2 != 0) {
                tris[0][0] = va;
                tris[0][1] = vd;
                tris[0][2] = vb;
                tris[1][0] = vb;
                tris[1][1] = vd;
                tris[1][2] = vc;
            }
            for (const auto& tr : tris) {
                Vec3 n = Normalize(Cross(tr[1] - tr[0], tr[2] - tr[0]));
                float top = Max(tr[0].y, Max(tr[1].y, tr[2].y));
                if (n.y < 0.62f && top > 4.0f)
                    AddStrataTriangle(mesh, t, tr[0], tr[1], tr[2], x);
            }
        }
    }
}

Vec3 CanyonArchPosition()
{
    return Vec3(ARCH_X, ARCH_UNDERSIDE * 0.5f, CanyonCenterZ(ARCH_X));
}

// A natural rock arch across the canyon: a lumpy, rounded rock tube on a half ellipse, thicker at its feet, whose
// legs sink into both walls. Smooth shading, the rock texture tinted like the strata.
static void BuildArch(MeshData& mesh, const PropPlace& p)
{
    float half = CanyonHalfWidth(p.x) + 110.0f;
    float legY = -20.0f;
    float apex = ARCH_UNDERSIDE + ARCH_THICKNESS * 0.8f;
    auto path = [&](float t) {
        float a = t * PI;
        return Vec3(p.x + 6.0f * FastSin(a * 2.0f), legY + (apex - legY) * FastSin(a), p.z - half * FastCos(a));
    };
    AddSurface(
        mesh, 10, 24,
        [&](float s, float t) {
            constexpr float E = 1e-3f;
            Vec3 tangent = Normalize(path(Clamp(t + E, 0.0f, 1.0f)) - path(Clamp(t - E, 0.0f, 1.0f)));
            Vec3 side = Normalize(Cross(Vec3(0, 1, 0), tangent));
            Vec3 up = Cross(tangent, side);
            float a = s * 2.0f * PI;
            float lump = Fbm(FastCos(a) * 1.5f + t * 9.0f, FastSin(a) * 1.5f + t * 5.0f, 3, p.seed);
            float r = ARCH_THICKNESS * (1.0f + 1.2f * Abs(FastCos(t * PI))) * (0.8f + 0.45f * lump);
            // A flatter, wider section: rock arches are broader than they are deep.
            return path(t) + side * (FastCos(a) * r * 1.3f) + up * (FastSin(a) * r * 0.8f);
        },
        [&](float s, float t) {
            UvRect r = PropRect(PX_ROCK);
            return Vec2{Lerp(r.u0, r.u1, s), Lerp(r.v0, r.v1, t)};
        },
        [](float, float) {
            return 0xFFC8A890u;
        },
        SurfaceOptions{false, true});
}

// --- Builders: the pirate base

static void BuildPirateBase(MeshData& mesh, const PropPlace& p)
{
    MeshData base;
    Rng rng(p.seed);
    // The rock spire: a rough faceted column up to the deck.
    MeshData spire;
    std::vector<Vec2> profile = {{10.0f, PIRATE_TOWER - 1.0f}, {13.0f, PIRATE_TOWER - 14.0f}, {19.0f, 100.0f},
                                 {16.0f, 80.0f},               {22.0f, 58.0f},                 {30.0f, 34.0f},
                                 {42.0f, 12.0f},               {56.0f, -14.0f}};
    AddLathe(spire, profile, 11, PropRect(PX_ROCK), WHITE, false);
    for (Vertex& v : spire.vertices) {
        float n = Fbm(FastAtan2(v.pz, v.px) * 2.0f, v.py / 9.0f, 3, p.seed);
        float k = 0.8f + 0.45f * n;
        v.px *= k;
        v.pz *= k;
    }
    Flatten(spire);
    TintMesh(spire, 0xFF625854u);
    Append(base, spire);
    UvRect planks = PropRect(PX_PLANKS);
    float deckY = PIRATE_TOWER;
    // Top deck with beams down to the rock, and a lower ring gallery.
    AddTube(base, Vec3(0, deckY - 0.8f, 0), Vec3(0, deckY, 0), 21.0f, 21.0f, 14, planks, WHITE, true);
    for (int i = 0; i < 6; i++) {
        float a = i * PI / 3.0f;
        Vec3 rim(FastCos(a) * 19.0f, deckY - 0.8f, FastSin(a) * 19.0f);
        AddTube(base, rim, Vec3(FastCos(a) * 11.0f, deckY - 18.0f, FastSin(a) * 11.0f), 0.6f, 0.6f, 4,
                PropSwatchUv(PS_DARK_WOOD), WHITE, false);
    }
    AddTube(base, Vec3(0, 79.4f, 0), Vec3(0, 80.0f, 0), 25.0f, 25.0f, 12, planks, WHITE, true);
    // The fortress on the deck (review 5 §8): an octagonal gunmetal keep with hazard-striped bands,
    // battlements, lit slit windows and the skull emblem facing the island, searchlights on the rim, a mooring mast
    // for the zeppelin on top and big pirate flags.
    UvRect white = PropSwatchUv(PS_WHITE);
    constexpr uint32_t GUNMETAL = 0xFF3A3F47u;
    constexpr uint32_t HAZARD = 0xFFE8B923u;
    constexpr uint32_t INK = 0xFF1A1A1Au;
    float keepY = deckY;
    AddTube(base, Vec3(0, keepY, 0), Vec3(0, keepY + FORTRESS_HEIGHT, 0), 15.0f, 13.0f, 8, white, GUNMETAL, true);
    for (int band = 0; band < 2; band++) {
        float y0 = keepY + (band == 0 ? 2.0f : FORTRESS_HEIGHT - 5.0f);
        float r = (band == 0 ? 15.4f : 13.6f);
        for (int k = 0; k < 16; k++) {
            float a0 = k * PI / 8.0f;
            float a1 = (k + 1) * PI / 8.0f;
            Vec3 p0(FastCos(a0) * r, y0, FastSin(a0) * r);
            Vec3 p1(FastCos(a1) * r, y0, FastSin(a1) * r);
            AddCard(base, p1, p0, p0 + Vec3(0, 3.0f, 0), p1 + Vec3(0, 3.0f, 0), white, k % 2 ? HAZARD : INK, false);
        }
    }
    for (int k = 0; k < 8; k++) {
        float a = (k + 0.5f) * PI / 4.0f;
        Vec3 c(FastCos(a) * 12.5f, keepY + FORTRESS_HEIGHT + 1.5f, FastSin(a) * 12.5f);
        AddBox(base, c, Vec3(2.2f, 1.5f, 2.2f), white, GUNMETAL);
        Vec3 w(FastCos(a) * 14.6f, keepY + 13.0f, FastSin(a) * 14.6f);
        AddBox(base, w, Vec3(0.9f, 2.2f, 0.9f), white, 0xFFFFD98Au);
    }
    // The emblem toward the island (and a second one on the far side).
    float toIsland = FastAtan2(VOLCANO_X - p.x, VOLCANO_Z - p.z) - p.yaw;
    for (int side = 0; side < 2; side++) {
        float a = toIsland + side * PI;
        Vec3 out(FastSin(a), 0.0f, FastCos(a));
        Vec3 across(out.z, 0.0f, -out.x);
        Vec3 c = out * 14.9f + Vec3(0, keepY + 15.0f, 0);
        float h = 7.0f;
        AddCard(base, c - across * h - Vec3(0, h, 0), c + across * h - Vec3(0, h, 0), c + across * h + Vec3(0, h, 0),
                c - across * h + Vec3(0, h, 0), PropRect(PX_FLAG), WHITE, false);
    }
    // Searchlights on the battlements.
    for (int k = 0; k < 4; k++) {
        float a = k * PI * 0.5f + 0.4f;
        Vec3 out(FastCos(a), 0.0f, FastSin(a));
        Vec3 c = out * 11.0f + Vec3(0, keepY + FORTRESS_HEIGHT + 0.5f, 0);
        AddTube(base, c, c + Vec3(0, 1.5f, 0), 0.5f, 0.5f, 4, white, INK, false);
        Vec3 lamp = c + Vec3(0, 2.2f, 0);
        AddTube(base, lamp - out * 1.0f, lamp + out * 0.8f + Vec3(0, 0.6f, 0), 1.0f, 1.2f, 8, white, GUNMETAL, false);
        AddTube(base, lamp + out * 0.8f + Vec3(0, 0.6f, 0), lamp + out * 0.85f + Vec3(0, 0.62f, 0), 1.15f, 1.15f, 8,
                PropSwatchUv(PS_LAMP), WHITE, true);
    }
    // The mooring mast: a tapered steel mast with rings, the mooring cone and a red lamp.
    float mastFoot = keepY + FORTRESS_HEIGHT;
    AddTube(base, Vec3(0, mastFoot, 0), Vec3(0, mastFoot + MAST_HEIGHT, 0), 2.2f, 0.8f, 5, white, GUNMETAL, false);
    for (int k = 1; k <= 3; k++)
        AddTube(base, Vec3(0, mastFoot + k * 10.0f, 0), Vec3(0, mastFoot + k * 10.0f + 0.8f, 0), 2.6f - k * 0.4f,
                2.6f - k * 0.4f, 6, white, HAZARD, true);
    MeshData cone;
    AddEllipsoid(cone, Vec3(), Vec3(2.0f, 2.6f, 2.0f), 8, 4, white, INK);
    AddEllipsoid(cone, Vec3(0, 2.8f, 0), Vec3(0.7f, 0.7f, 0.7f), 6, 3, white, 0xFFFF3B2Fu);
    Append(base, cone, MatTranslate(Vec3(0, mastFoot + MAST_HEIGHT, 0)));
    // Big flags on the rim.
    for (int i = 0; i < 4; i++) {
        float a = 0.2f + i * PI * 0.5f;
        Vec3 foot(FastCos(a) * 19.0f, deckY, FastSin(a) * 19.0f);
        AddTube(base, foot, foot + Vec3(0, 22.0f, 0), 0.3f, 0.2f, 4, white, INK, false);
        Vec3 dir(-FastSin(a), 0, FastCos(a));
        Vec3 mid = foot + Vec3(0, 18.5f, 0);
        Vec3 pts[3] = {mid + dir * 0.3f, mid + dir * 5.0f + Vec3(0, -0.5f, 0), mid + dir * 9.5f + Vec3(0, 0.2f, 0)};
        float widths[3] = {3.0f, 3.0f, 2.8f};
        AddLeaf(base, pts, widths, 3, Vec3(0, 1, 0), PropRect(PX_FLAG), WHITE);
    }
    for (int i = 0; i < 8; i++) {
        float a = i * 0.45f + rng.Range(-0.1f, 0.1f);
        Vec3 c(FastCos(a) * 21.0f, 80.0f, FastSin(a) * 21.0f);
        AddTube(base, c, c + Vec3(0, 1.6f, 0), 0.6f, 0.6f, 6, PropSwatchUv(i % 2 ? PS_RED : PS_YELLOW), WHITE, true);
    }
    Append(mesh, base, PlaceMatrix(p));
}

// --- Groups

static void BuildPlace(MeshData& mesh, const PropPlace& p)
{
    switch (p.kind) {
    case PropKind::PALM:
        BuildPalm(mesh, p);
        break;
    case PropKind::BANANA_PLANT:
        BuildBananaPlant(mesh, p);
        break;
    case PropKind::HUT:
        BuildHut(mesh, p);
        break;
    case PropKind::WAREHOUSE:
        BuildWarehouse(mesh, p);
        break;
    case PropKind::TOWNHOUSE:
        BuildTownhouse(mesh, p);
        break;
    case PropKind::WHARF:
        BuildWharf(mesh, p);
        break;
    case PropKind::SHIP:
        BuildShip(mesh, p);
        break;
    case PropKind::CHURCH:
        BuildChurch(mesh, p);
        break;
    case PropKind::DOCK:
        BuildDock(mesh, p);
        break;
    case PropKind::BOAT:
        BuildBoat(mesh, p);
        break;
    case PropKind::LIGHTHOUSE:
        BuildLighthouse(mesh, p);
        break;
    case PropKind::MOLE:
        BuildMole(mesh, p);
        break;
    case PropKind::WASHING:
        BuildWashing(mesh, p);
        break;
    case PropKind::ROCK:
        BuildRock(mesh, p);
        break;
    case PropKind::STRATA:
        BuildStrata(mesh, p);
        break;
    case PropKind::ARCH:
        BuildArch(mesh, p);
        break;
    case PropKind::PIRATE_BASE:
        BuildPirateBase(mesh, p);
        break;
    }
}

static int GroupOf(const PropPlace& p)
{
    int gx = static_cast<int>(Clamp((p.x + MAP_HALF) / (2.0f * MAP_HALF) * GROUP_GRID, 0.0f, GROUP_GRID - 1.0f));
    int gz = static_cast<int>(Clamp((p.z + MAP_HALF) / (2.0f * MAP_HALF) * GROUP_GRID, 0.0f, GROUP_GRID - 1.0f));
    return gz * GROUP_GRID + gx;
}

int PropGroupCount()
{
    return GROUP_GRID * GROUP_GRID;
}

MeshData MakePropGroup(int index)
{
    PlaceAll();
    MeshData mesh;
    for (int i = 0; i < g_placeCount; i++) {
        if (g_places[i].kind != PropKind::PALM && GroupOf(g_places[i]) == index)
            BuildPlace(mesh, g_places[i]);
    }
    return mesh;
}

Vec3 PropGroupCenter(int index)
{
    float cell = 2.0f * MAP_HALF / GROUP_GRID;
    return Vec3(-MAP_HALF + (index % GROUP_GRID + 0.5f) * cell, 120.0f, -MAP_HALF + (index / GROUP_GRID + 0.5f) * cell);
}

float PropGroupRadius(int)
{
    float half = MAP_HALF / GROUP_GRID;
    return Sqrt(2.0f * half * half + 260.0f * 260.0f) + 40.0f;
}

MeshData MakePalmCluster(int index)
{
    PlaceAll();
    MeshData mesh;
    const PalmCluster& c = g_clusters[index];
    for (int i = 0; i < c.count; i++)
        BuildPalm(mesh, g_places[c.first + i]);
    return mesh;
}

} // namespace content
