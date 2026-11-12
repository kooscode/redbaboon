// GPU resources of the game: generates every mesh and texture with the content generator, uploads them and frees
// the CPU copies (docs/design.md §7). The static level (terrain tiles and prop groups) gets the fixed sun and sky
// light baked into its vertex colours, so it draws with the renderer's short unlit vertex program. Also makes the
// game's own tracer ribbon texture and shadow quad, and keeps the landmarks the mission needs.
#include "game/world.h"

#include <unordered_map>

namespace game {

// The game's own textures are made 128 x 128: smaller textures did not show on the Xbox renderer (2026-11-11:
// content's 64 x 64 blob shadow and a 32 x 32 streak drew nothing; the same quad with a 256 x 256 texture drew).
constexpr int STREAK_SIZE = 128;
constexpr float PLANE_LOD_CELL = 0.6f;     // vertex clustering cells of the distant meshes (metres)
constexpr float ZEPPELIN_LOD_CELL = 6.0f;
constexpr float RING_LOD_CELL = 2.5f;
constexpr int SHADOW_SIZE = 128;

Assets assets;

// --- Helpers

// Multiplies every vertex colour by the sun and sky light its normal receives (the renderer's lit formula without
// specular: hemisphere ambient plus sun diffuse), clamped to 1.
static void BakeLighting(MeshData& mesh)
{
    render::Environment env;
    for (Vertex& v : mesh.vertices) {
        Vec3 n(v.nx, v.ny, v.nz);
        Vec3 ambient = Lerp(env.groundAmbient, env.ambient, n.y * 0.5f + 0.5f);
        Vec3 light = ambient + env.sunColor * fmaxf(Dot(n, env.sunDirection), 0.0f);
        float a = ((v.color >> 24) & 0xFF) / 255.0f;
        float r = ((v.color >> 16) & 0xFF) / 255.0f * light.x;
        float g = ((v.color >> 8) & 0xFF) / 255.0f * light.y;
        float b = (v.color & 0xFF) / 255.0f * light.z;
        v.color = PackARGB(r, g, b, a);
    }
}

static render::MeshId UploadBaked(MeshData mesh)
{
    BakeLighting(mesh);
    return render::CreateMesh(mesh);
}

// A half-resolution version of a terrain tile for distant drawing. Content's tiles start with an (n+1) x (n+1)
// grid of vertices, row by row; this keeps every second vertex and hangs a two-sided 30 m skirt from all four edges
// (the cracks against full tiles are hidden behind it). Returns an empty mesh when the tile is already coarse.
static MeshData MakeTileLod(const MeshData& full)
{
    MeshData lod;
    int row = 0;
    while (row < static_cast<int>(full.vertices.size()) && full.vertices[row].pz == full.vertices[0].pz)
        row++;
    int n = row - 1;
    if (n < 16 || n % 2 != 0 || static_cast<int>(full.vertices.size()) < row * row)
        return lod;
    int half = n / 2;
    for (int j = 0; j <= half; j++) {
        for (int i = 0; i <= half; i++)
            lod.vertices.push_back(full.vertices[(j * 2) * row + i * 2]);
    }
    int side = half + 1;
    for (int j = 0; j < half; j++) {
        for (int i = 0; i < half; i++) {
            uint16_t a = static_cast<uint16_t>(j * side + i);
            uint16_t b = static_cast<uint16_t>(a + 1);
            uint16_t c = static_cast<uint16_t>(a + side + 1);
            uint16_t d = static_cast<uint16_t>(a + side);
            const uint16_t quad[6] = {a, d, c, a, c, b};
            for (uint16_t index : quad)
                lod.indices.push_back(index);
        }
    }
    // Skirts: for each edge, a strip from each pair of edge vertices down 30 m, both faces.
    for (int edge = 0; edge < 4; edge++) {
        for (int k = 0; k < half; k++) {
            int i0 = edge == 1 ? half : (edge == 3 ? 0 : k);
            int j0 = edge == 0 ? 0 : (edge == 2 ? half : k);
            int i1 = edge == 0 || edge == 2 ? k + 1 : i0;
            int j1 = edge == 1 || edge == 3 ? k + 1 : j0;
            uint16_t top0 = static_cast<uint16_t>(j0 * side + i0);
            uint16_t top1 = static_cast<uint16_t>(j1 * side + i1);
            uint16_t base = static_cast<uint16_t>(lod.vertices.size());
            Vertex low0 = lod.vertices[top0];
            Vertex low1 = lod.vertices[top1];
            low0.py -= 30.0f;
            low1.py -= 30.0f;
            lod.vertices.push_back(low0);
            lod.vertices.push_back(low1);
            uint16_t b0 = base;
            uint16_t b1 = static_cast<uint16_t>(base + 1);
            const uint16_t strip[12] = {top0, b0, b1, top0, b1, top1, top0, b1, b0, top0, top1, b1};
            for (uint16_t index : strip)
                lod.indices.push_back(index);
        }
    }
    return lod;
}

// A coarse copy of a mesh for drawing at range (vertex clustering): vertices in the same cube of `cell` metres
// merge into the first of them, and triangles that collapse are dropped. Cheap and good enough where the mesh
// is a few dozen pixels across.
static MeshData MakeDistantMesh(const MeshData& mesh, float cell)
{
    MeshData out;
    std::unordered_map<uint64_t, uint16_t> cells;
    std::vector<uint16_t> remap(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); i++) {
        const Vertex& v = mesh.vertices[i];
        auto key = [&](float c) {
            return static_cast<uint64_t>(static_cast<int64_t>(floorf(c / cell)) + 32768) & 0xFFFF;
        };
        uint64_t id = key(v.px) | (key(v.py) << 16) | (key(v.pz) << 32);
        auto found = cells.find(id);
        if (found == cells.end()) {
            uint16_t index = static_cast<uint16_t>(out.vertices.size());
            cells.emplace(id, index);
            out.vertices.push_back(v);
            remap[i] = index;
        } else {
            remap[i] = found->second;
        }
    }
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        uint16_t a = remap[mesh.indices[t]];
        uint16_t b = remap[mesh.indices[t + 1]];
        uint16_t c = remap[mesh.indices[t + 2]];
        if (a == b || b == c || a == c)
            continue;
        out.indices.push_back(a);
        out.indices.push_back(b);
        out.indices.push_back(c);
    }
    return out;
}

// A white ribbon, opaque along its centre line and fading to the edges (across: U) and the ends (along: V).
static ImageData MakeStreakImage()
{
    ImageData image;
    image.width = STREAK_SIZE;
    image.height = STREAK_SIZE;
    image.pixels.resize(STREAK_SIZE * STREAK_SIZE);
    for (int y = 0; y < STREAK_SIZE; y++) {
        for (int x = 0; x < STREAK_SIZE; x++) {
            float u = fabsf((x + 0.5f) / STREAK_SIZE * 2.0f - 1.0f);
            float v = fabsf((y + 0.5f) / STREAK_SIZE * 2.0f - 1.0f);
            float across = Clamp(1.0f - u * u, 0.0f, 1.0f);
            float along = Clamp((1.0f - v) * 4.0f, 0.0f, 1.0f);
            image.pixels[y * STREAK_SIZE + x] = PackARGB(1.0f, 1.0f, 1.0f, across * across * along);
        }
    }
    return image;
}

// A soft dark blob for shadows (content's shape: alpha 0.6 in the middle, smooth to 0 at the rim).
static ImageData MakeShadowImage()
{
    ImageData image;
    image.width = SHADOW_SIZE;
    image.height = SHADOW_SIZE;
    image.pixels.resize(SHADOW_SIZE * SHADOW_SIZE);
    for (int y = 0; y < SHADOW_SIZE; y++) {
        for (int x = 0; x < SHADOW_SIZE; x++) {
            float dx = (x + 0.5f) / SHADOW_SIZE * 2.0f - 1.0f;
            float dy = (y + 0.5f) / SHADOW_SIZE * 2.0f - 1.0f;
            float t = Clamp((1.0f - sqrtf(dx * dx + dy * dy)) / 0.75f, 0.0f, 1.0f);
            image.pixels[y * SHADOW_SIZE + x] = PackARGB(0.1f, 0.08f, 0.06f, 0.6f * t * t * (3.0f - 2.0f * t));
        }
    }
    return image;
}

// A unit quad in the XZ plane facing up, for blob shadows.
static MeshData MakeShadowQuad()
{
    MeshData mesh;
    for (int i = 0; i < 4; i++) {
        Vertex v;
        float u = (i == 1 || i == 2) ? 1.0f : 0.0f;
        float w = i >= 2 ? 1.0f : 0.0f;
        v.px = u - 0.5f;
        v.py = 0.0f;
        v.pz = w - 0.5f;
        v.nx = 0.0f;
        v.ny = 1.0f;
        v.nz = 0.0f;
        v.u = u;
        v.v = w;
        v.color = 0xFFFFFFFF;
        mesh.vertices.push_back(v);
    }
    // Counter-clockwise seen from above.
    const uint16_t indices[6] = {0, 2, 1, 0, 3, 2};
    for (uint16_t index : indices)
        mesh.indices.push_back(index);
    return mesh;
}

float GroundHeight(float x, float z)
{
    const content::Terrain* terrain = assets.terrain;
    if (!terrain)
        return 0.0f;
    float half = terrain->size * 0.5f;
    if (fabsf(x) >= half || fabsf(z) >= half)
        return 0.0f;
    return fmaxf(terrain->HeightAt(x, z), 0.0f);
}

// --- Loading

static void LoadLevel(ProgressFunction progress)
{
    assets.terrain = &content::BuildTerrain();
    assets.volcano = content::VolcanoCraterPosition();
    assets.harbour = content::HarbourPosition();
    assets.wharf = content::HarbourWharfPosition();
    assets.pirateBase = content::PirateBasePosition();
    assets.lighthouseLamp = content::LighthouseLampPosition();
    assets.canyonArch = content::CanyonArchPosition();
    assets.tileCount = content::TerrainTileCount();
    if (assets.tileCount > MAX_TILES)
        assets.tileCount = MAX_TILES;
    for (int i = 0; i < assets.tileCount; i++) {
        if (progress && i % 8 == 0)
            progress("Island", 0.45f * i / assets.tileCount);
        MeshData tile = content::MakeTerrainTile(i);
        BakeLighting(tile);
        MeshData lod = MakeTileLod(tile);
        assets.tiles[i] = render::CreateMesh(tile);
        assets.tileLod[i] = lod.indices.empty() ? assets.tiles[i] : render::CreateMesh(lod);
        assets.tileCenter[i] = content::TerrainTileCenter(i);
        assets.tileRadius[i] = content::TerrainTileRadius(i);
    }
    if (progress)
        progress("Palms", 0.45f);
    assets.propGroupCount = content::PropGroupCount();
    if (assets.propGroupCount > MAX_PROP_GROUPS)
        assets.propGroupCount = MAX_PROP_GROUPS;
    for (int i = 0; i < assets.propGroupCount; i++) {
        assets.propGroup[i] = UploadBaked(content::MakePropGroup(i));
        assets.propGroupCenter[i] = content::PropGroupCenter(i);
        assets.propGroupRadius[i] = content::PropGroupRadius(i);
    }
    assets.palmClusterCount = content::PalmClusterCount();
    if (assets.palmClusterCount > MAX_PALM_CLUSTERS)
        assets.palmClusterCount = MAX_PALM_CLUSTERS;
    int palms = 0;
    for (int i = 0; i < assets.palmClusterCount; i++) {
        assets.palmCluster[i] = UploadBaked(content::MakePalmCluster(i));
        assets.palmClusterCenter[i] = content::PalmClusterCenter(i);
        assets.palmClusterRadius[i] = content::PalmClusterRadius(i);
        assets.palmClusterFirst[i] = palms;
        int first = content::PalmClusterFirst(i);
        for (int k = 0; k < content::PalmClusterSize(i) && palms < MAX_PALMS; k++) {
            // The impostor's centre and size as content specifies them.
            content::PalmInstance palm = content::Palm(first + k);
            render::Billboard& b = assets.palmBoard[palms++];
            b.position = (palm.base + palm.top) * 0.5f + WORLD_UP * (0.15f * palm.height);
            b.size = 1.3f * palm.height;
            b.color = 0xFFC8C8C8;
        }
        assets.palmClusterSize[i] = palms - assets.palmClusterFirst[i];
    }
    assets.palmImpostor = render::CreateTexture(content::MakePalmImpostorTexture(), true, false);
    if (progress)
        progress("Sea", 0.55f);
    assets.sea = render::CreateMesh(content::MakeSea());
}

static void LoadPlanes()
{
    for (int i = 0; i < LIVERIES; i++) {
        content::Livery livery = static_cast<content::Livery>(i);
        MeshData plane = content::MakePlane(livery);
        assets.plane[i] = render::CreateMesh(plane);
        assets.planeDistant[i] = render::CreateMesh(MakeDistantMesh(plane, PLANE_LOD_CELL));
        assets.canopy[i] = render::CreateMesh(content::MakeCanopyGlass(livery));
        assets.propeller[i] = render::CreateMesh(content::MakePropeller(livery));
        assets.propellerDisc[i] = render::CreateMesh(content::MakePropellerDisc(livery));
        assets.planeTexture[i] = render::CreateTexture(content::MakePlaneTexture(livery), true, false);
        assets.propellerHub[i] = content::PropellerHub(livery);
        assets.gunMuzzle[i][0] = content::GunMuzzle(livery, 0);
        assets.gunMuzzle[i][1] = content::GunMuzzle(livery, 1);
    }
    assets.propellerTexture = render::CreateTexture(content::MakePropellerTexture(), true, false);
    assets.scarfCount = content::ScarfSegmentCount();
    if (assets.scarfCount > MAX_SCARF)
        assets.scarfCount = MAX_SCARF;
    assets.scarfLength = content::ScarfSegmentLength();
    assets.scarfAnchor = content::ScarfAnchor();
    for (int i = 0; i < assets.scarfCount; i++)
        assets.scarf[i] = render::CreateMesh(content::MakeScarfSegment(i));
}

static void LoadZeppelin()
{
    MeshData zeppelin = content::MakeZeppelin();
    assets.zeppelin = render::CreateMesh(zeppelin);
    assets.zeppelinDistant = render::CreateMesh(MakeDistantMesh(zeppelin, ZEPPELIN_LOD_CELL));
    assets.zeppelinBody = render::CreateMesh(content::MakeZeppelinBody());
    assets.zeppelinFrame = render::CreateMesh(content::MakeZeppelinFrame());
    assets.hullSectionCount = content::ZeppelinHullSectionCount();
    if (assets.hullSectionCount > MAX_HULL_SECTIONS)
        assets.hullSectionCount = MAX_HULL_SECTIONS;
    for (int i = 0; i < assets.hullSectionCount; i++)
        assets.hullSectionCenter[i] = content::ZeppelinHullSectionCenter(i);
    BuildZeppelinStages();
    assets.turret = render::CreateMesh(content::MakeTurret());
    assets.zeppelinEngine = render::CreateMesh(content::MakeZeppelinEngine());
    assets.zeppelinTexture = render::CreateTexture(content::MakeZeppelinTexture(), true, true);
    assets.rocket = render::CreateMesh(content::MakeBananaRocket());
    assets.crate = render::CreateMesh(content::MakeCrate());
    MeshData ring = content::MakeRing();
    assets.ring = render::CreateMesh(ring);
    assets.ringDistant = render::CreateMesh(MakeDistantMesh(ring, RING_LOD_CELL));
}

void LoadLogo()
{
    if (assets.title == render::NO_TEXTURE)
        assets.title = render::CreateTexture(content::MakeTitleTexture(), false, false);
}

bool LoadAssets(ProgressFunction progress)
{
    LoadLogo();
    if (progress)
        progress("Island", 0.0f);
    LoadLevel(progress);
    if (progress)
        progress("Planes", 0.6f);
    LoadPlanes();
    if (progress)
        progress("Zeppelin", 0.72f);
    LoadZeppelin();
    if (progress)
        progress("Textures", 0.85f);
    assets.terrainTexture = render::CreateTexture(content::MakeTerrainMacroTexture(), true, true);
    assets.terrainDetail = render::CreateTexture(content::MakeTerrainDetailTexture(), true, true);
    assets.waterTexture = render::CreateTexture(content::MakeSeaTexture(), true, true);
    assets.seaDetail = render::CreateTexture(content::MakeSeaDetailTexture(), true, true);
    assets.propsTexture = render::CreateTexture(content::MakePropsTexture(), true, false);
    assets.particles = render::CreateTexture(content::MakeParticleAtlas(), true, false);
    assets.clouds = render::CreateTexture(content::MakeCloudTexture(), true, false);
    assets.hud = render::CreateTexture(content::MakeHudTexture(), false, false);
    assets.shadow = render::CreateTexture(MakeShadowImage(), true, false);
    assets.streak = render::CreateTexture(MakeStreakImage(), false, false);
    assets.shadowQuad = render::CreateMesh(MakeShadowQuad());
    CreateHudArt();
    CreateControlsArt();
    BuildClouds();
    if (progress)
        progress("Ready", 1.0f);
    return assets.tiles[0] != render::NO_MESH && assets.plane[0] != render::NO_MESH;
}

} // namespace game
