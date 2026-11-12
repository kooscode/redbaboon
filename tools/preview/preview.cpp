// Content preview: generates every mesh and texture of src/content, renders the meshes from three angles and some
// level views with a software rasteriser, writes the textures, and reports triangle counts and generation times.
// Usage: preview OUT_DIR (make preview: build/preview/), or preview --profile. docs/design.md §7.
#include "content/content.h"
#include "content/terrain_shape.h"
#include "raster.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <string>
#include <vector>

using namespace content;
using preview::Blend;
using preview::DrawOptions;
using preview::Raster;

// Output folder (the command line's OUT_DIR).
static std::string g_out;

// --- Timing and reporting

static double Now()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// Total generation time of everything timed.
static double g_totalTime = 0.0;

template <typename F>
static auto Timed(const char* name, F f)
{
    double t0 = Now();
    auto r = f();
    double dt = Now() - t0;
    g_totalTime += dt;
    printf("  %-28s %7.2f ms", name, dt * 1000.0);
    return r;
}

static MeshData TimedMesh(const char* name, MeshData (*f)())
{
    MeshData m = Timed(name, f);
    printf("  %6d tris %6d verts\n", static_cast<int>(m.indices.size() / 3), static_cast<int>(m.vertices.size()));
    return m;
}

static ImageData TimedImage(const char* name, ImageData (*f)())
{
    ImageData img = Timed(name, f);
    printf("  %dx%d\n", img.width, img.height);
    return img;
}

// --- Output

static void SaveTexture(const char* name, const ImageData& img)
{
    int w = img.width;
    int h = img.height;
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * 2 * h * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t p = img.pixels[static_cast<size_t>(y) * w + x];
            float a = ((p >> 24) & 0xFF) / 255.0f;
            float checker = ((x / 8 + y / 8) % 2) ? 0.35f : 0.55f;
            uint8_t* o = &rgb[(static_cast<size_t>(y) * w * 2 + x) * 3];
            for (int c = 0; c < 3; c++) {
                float v = ((p >> (16 - c * 8)) & 0xFF) / 255.0f;
                o[c] = static_cast<uint8_t>((v * a + checker * (1.0f - a)) * 255.0f + 0.5f);
            }
            uint8_t* q = &rgb[(static_cast<size_t>(y) * w * 2 + w + x) * 3];
            q[0] = q[1] = q[2] = static_cast<uint8_t>(a * 255.0f + 0.5f);
        }
    }
    std::string path = g_out + "/tex_" + name + ".png";
    preview::WritePng(path.c_str(), w * 2, h, rgb.data(), 3);
}

static void Save(const std::string& name, Raster& r)
{
    int w = 0;
    int h = 0;
    std::vector<uint8_t> px = r.Resolve(w, h);
    std::string path = g_out + "/" + name + ".png";
    preview::WritePng(path.c_str(), w, h, px.data(), 3);
}

// Pastes several resolved views side by side into one PNG.
static void SaveSheet(const std::string& name, std::vector<Raster*> views)
{
    int w = 0;
    int h = 0;
    std::vector<std::vector<uint8_t>> parts;
    for (Raster* r : views)
        parts.push_back(r->Resolve(w, h));
    int total = w * static_cast<int>(parts.size());
    std::vector<uint8_t> out(static_cast<size_t>(total) * h * 3);
    for (size_t i = 0; i < parts.size(); i++) {
        for (int y = 0; y < h; y++)
            memcpy(&out[(static_cast<size_t>(y) * total + i * w) * 3], &parts[i][static_cast<size_t>(y) * w * 3],
                   static_cast<size_t>(w) * 3);
    }
    std::string path = g_out + "/" + name + ".png";
    preview::WritePng(path.c_str(), total, h, out.data(), 3);
}

// --- Mesh views

struct Part {
    const MeshData* mesh;
    Mat4 model;
    DrawOptions options;
};

static void Bounds(const std::vector<Part>& parts, Vec3& center, float& radius)
{
    Vec3 lo(1e30f, 1e30f, 1e30f);
    Vec3 hi(-1e30f, -1e30f, -1e30f);
    for (const Part& p : parts) {
        for (const Vertex& v : p.mesh->vertices) {
            Vec3 w = TransformPoint(p.model, Vec3(v.px, v.py, v.pz));
            lo = Vec3(fminf(lo.x, w.x), fminf(lo.y, w.y), fminf(lo.z, w.z));
            hi = Vec3(fmaxf(hi.x, w.x), fmaxf(hi.y, w.y), fmaxf(hi.z, w.z));
        }
    }
    center = (lo + hi) * 0.5f;
    radius = Length(hi - lo) * 0.5f;
}

// Renders the parts from three angles (front three-quarter from above, side, rear three-quarter from below).
// focusRadius > 0 frames a sphere around focus instead of the whole model.
static void ViewParts(const std::string& name, const std::vector<Part>& parts, float zoom = 1.0f,
                      Vec3 focus = Vec3(), float focusRadius = 0.0f)
{
    Vec3 center;
    float radius = 0.0f;
    Bounds(parts, center, radius);
    if (focusRadius > 0.0f) {
        center = focus;
        radius = focusRadius;
    }
    static const float ANGLES[3][2] = {{-35.0f, 22.0f}, {-90.0f, 4.0f}, {150.0f, 25.0f}};
    std::vector<Raster*> views;
    for (int i = 0; i < 3; i++) {
        Raster* r = new Raster(1024, 1024);
        float yaw = ANGLES[i][0] * DEG;
        float pitch = ANGLES[i][1] * DEG;
        Vec3 dir(sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch));
        float fov = 40.0f * DEG;
        float dist = radius / sinf(fov * 0.5f) * 0.8f / zoom;
        Vec3 eye = center + dir * dist;
        r->SetCamera(eye, QuatLook(center - eye, Vec3(0, 1, 0)), fov, 0.05f * radius, dist + radius * 4.0f);
        preview::Light light;
        light.fogStart = 1e6f;
        light.fogEnd = 2e6f;
        r->SetLight(light);
        r->ClearSky(Vec3(0.16f, 0.38f, 0.72f), Vec3(0.79f, 0.87f, 0.92f));
        for (const Part& p : parts) {
            if (p.options.blend == Blend::OPAQUE || p.options.blend == Blend::ALPHA_TEST)
                r->Draw(*p.mesh, p.model, p.options);
        }
        for (const Part& p : parts) {
            if (p.options.blend == Blend::ALPHA || p.options.blend == Blend::ADDITIVE)
                r->Draw(*p.mesh, p.model, p.options);
        }
        views.push_back(r);
    }
    SaveSheet("mesh_" + name, views);
    for (Raster* r : views)
        delete r;
}

static DrawOptions Opts(const ImageData* tex, Blend blend = Blend::OPAQUE, float specular = 0.3f)
{
    DrawOptions o;
    o.texture = tex;
    o.blend = blend;
    o.specular = specular;
    return o;
}

// --- Planes

static void PreviewPlanes()
{
    printf("planes\n");
    static const Livery LIVERIES[3] = {Livery::PLAYER_RED, Livery::PIRATE_GREEN, Livery::PIRATE_PURPLE};
    static const char* NAMES[3] = {"player", "pirate", "ace"};
    for (int i = 0; i < 3; i++) {
        Livery l = LIVERIES[i];
        double t0 = Now();
        MeshData plane = MakePlane(l);
        MeshData glass = MakeCanopyGlass(l);
        MeshData prop = MakePropeller(l);
        MeshData disc = MakePropellerDisc(l);
        ImageData tex = MakePlaneTexture(l);
        double dt = Now() - t0;
        g_totalTime += dt;
        printf("  plane %-8s %7.2f ms  plane %d tris, glass %d, propeller %d, disc %d\n", NAMES[i], dt * 1000.0,
               static_cast<int>(plane.indices.size() / 3), static_cast<int>(glass.indices.size() / 3),
               static_cast<int>(prop.indices.size() / 3), static_cast<int>(disc.indices.size() / 3));
        SaveTexture((std::string("plane_") + NAMES[i]).c_str(), tex);
        std::vector<Part> parts;
        parts.push_back(Part{&plane, MatIdentity(), Opts(&tex, Blend::OPAQUE, 0.5f)});
        Mat4 propModel = MatRotate(QuatAxisAngle(Vec3(0, 0, 1), 0.3f)) * MatTranslate(PropellerHub(l));
        parts.push_back(Part{&prop, propModel, Opts(&tex, Blend::OPAQUE, 0.8f)});
        parts.push_back(Part{&glass, MatIdentity(), Opts(&tex, Blend::ALPHA, 0.9f)});
        MeshData scarf;
        if (l == Livery::PLAYER_RED) {
            scarf = MakeScarf();
            parts.push_back(Part{&scarf, MatIdentity(), Opts(&tex)});
        }
        ViewParts(std::string("plane_") + NAMES[i], parts);
        // Close-up of the cockpit and pilot.
        Vec3 head = l == Livery::PLAYER_RED ? Vec3(0.0f, 0.88f, 0.3f) : Vec3(0.0f, 1.08f, 0.45f) * 1.15f;
        ViewParts(std::string("pilot_") + NAMES[i], parts, 1.0f, head, 0.75f);
        ViewParts(std::string("nose_") + NAMES[i], parts, 1.0f, PropellerHub(l) + Vec3(0, 0, 0.8f), 1.6f);
        if (i == 0) {
            ImageData discTex = MakePropellerTexture();
            SaveTexture("propeller_disc", discTex);
            std::vector<Part> spin;
            spin.push_back(Part{&plane, MatIdentity(), Opts(&tex, Blend::OPAQUE, 0.5f)});
            spin.push_back(Part{&disc, MatTranslate(PropellerHub(l)), Opts(&discTex, Blend::ALPHA)});
            spin.push_back(Part{&glass, MatIdentity(), Opts(&tex, Blend::ALPHA, 0.9f)});
            ViewParts("plane_player_spinning", spin);
        }
    }
}

// --- Zeppelin

static void PreviewZeppelin()
{
    printf("zeppelin\n");
    MeshData zep = TimedMesh("MakeZeppelin", MakeZeppelin);
    MeshData body = TimedMesh("MakeZeppelinBody", MakeZeppelinBody);
    MeshData frame = TimedMesh("MakeZeppelinFrame", MakeZeppelinFrame);
    MeshData engine = TimedMesh("MakeZeppelinEngine", MakeZeppelinEngine);
    MeshData turret = TimedMesh("MakeTurret", MakeTurret);
    MeshData section = TimedMesh("MakeZeppelinHullSection(0)", []() {
        return MakeZeppelinHullSection(0);
    });
    ImageData tex = TimedImage("MakeZeppelinTexture", MakeZeppelinTexture);
    SaveTexture("zeppelin", tex);
    std::vector<Part> parts;
    parts.push_back(Part{&zep, MatIdentity(), Opts(&tex, Blend::OPAQUE, 0.4f)});
    std::vector<MeshData*> keep;
    for (int i = 0; i < ZeppelinEngineCount(); i++)
        parts.push_back(Part{&engine, MatTranslate(ZeppelinEngine(i)), Opts(&tex)});
    for (int i = 0; i < ZeppelinTurretCount(); i++) {
        Vec3 t = ZeppelinTurret(i);
        Quat q = QuatAxisAngle(Vec3(0, 1, 0), t.x > 0 ? -0.9f : 0.9f);
        parts.push_back(Part{&turret, MatRotate(q) * MatTranslate(t), Opts(&tex)});
    }
    ViewParts("zeppelin", parts);
    // Burning apart: sections 2 and 3 gone, the frame shows.
    std::vector<MeshData> sections;
    for (int i = 0; i < ZeppelinHullSectionCount(); i++)
        sections.push_back(MakeZeppelinHullSection(i));
    std::vector<Part> burnt;
    burnt.push_back(Part{&body, MatIdentity(), Opts(&tex)});
    burnt.push_back(Part{&frame, MatIdentity(), Opts(&tex)});
    for (int i = 0; i < ZeppelinHullSectionCount(); i++) {
        if (i < 4 || i > 6)
            burnt.push_back(Part{&sections[i], MatIdentity(), Opts(&tex, Blend::OPAQUE, 0.4f)});
    }
    for (int i = 0; i < ZeppelinEngineCount(); i++)
        burnt.push_back(Part{&engine, MatTranslate(ZeppelinEngine(i)), Opts(&tex)});
    ViewParts("zeppelin_burnt", burnt);
    std::vector<Part> one;
    one.push_back(Part{&engine, MatIdentity(), Opts(&tex)});
    one.push_back(Part{&turret, MatTranslate(Vec3(0, 0, -12.0f)), Opts(&tex)});
    ViewParts("zeppelin_engine_turret", one);
}

// --- Level views

// The game's light bake (src/game/assets.cpp): vertex colour times hemisphere ambient plus sun diffuse, clamped;
// the level is then drawn unlit, as on the Xbox.
static void BakeLighting(MeshData& mesh)
{
    Vec3 sun(-0.545f, 0.530f, 0.650f);
    Vec3 sunColor(1.0f, 0.92f, 0.78f);
    Vec3 sky(0.36f, 0.42f, 0.52f);
    Vec3 ground(0.26f, 0.23f, 0.18f);
    for (Vertex& v : mesh.vertices) {
        Vec3 n(v.nx, v.ny, v.nz);
        Vec3 light = Lerp(ground, sky, n.y * 0.5f + 0.5f) + sunColor * fmaxf(Dot(n, sun), 0.0f);
        float a = ((v.color >> 24) & 0xFF) / 255.0f;
        v.color = PackARGB(((v.color >> 16) & 0xFF) / 255.0f * light.x, ((v.color >> 8) & 0xFF) / 255.0f * light.y,
                           (v.color & 0xFF) / 255.0f * light.z, a);
    }
}

// Everything of the level, generated once.
struct Level {
    std::vector<MeshData> tiles;
    MeshData sea;
    ImageData terrainTex;
    ImageData waterTex;
    ImageData macroTex;
    ImageData seaDetailTex;
    std::vector<MeshData> propGroups;
    std::vector<MeshData> palmClusters;
    MeshData ring;
    ImageData propsTex;
    std::vector<Part> extra;
};

static Level* g_level = nullptr;

static void BuildLevel()
{
    printf("terrain\n");
    g_level = new Level();
    Timed("BuildTerrain", []() {
        return &BuildTerrain();
    });
    printf("\n");
    double t0 = Now();
    int tris = 0;
    for (int i = 0; i < TerrainTileCount(); i++) {
        g_level->tiles.push_back(MakeTerrainTile(i));
        BakeLighting(g_level->tiles.back());
        tris += static_cast<int>(g_level->tiles.back().indices.size() / 3);
    }
    double dt = Now() - t0;
    g_totalTime += dt;
    printf("  %-28s %7.2f ms  %6d tris (64 tiles)\n", "MakeTerrainTile x64", dt * 1000.0, tris);
    g_level->sea = TimedMesh("MakeSea", MakeSea);
    double tp = Now();
    int propTris = 0;
    for (int i = 0; i < PropGroupCount(); i++) {
        g_level->propGroups.push_back(MakePropGroup(i));
        BakeLighting(g_level->propGroups.back());
        propTris += static_cast<int>(g_level->propGroups.back().indices.size() / 3);
    }
    double dtp = Now() - tp;
    g_totalTime += dtp;
    printf("  %-28s %7.2f ms  %6d tris (%d groups)\n", "MakePropGroup x16", dtp * 1000.0, propTris,
           PropGroupCount());
    double tq = Now();
    int palmTris = 0;
    for (int i = 0; i < PalmClusterCount(); i++) {
        g_level->palmClusters.push_back(MakePalmCluster(i));
        BakeLighting(g_level->palmClusters.back());
        palmTris += static_cast<int>(g_level->palmClusters.back().indices.size() / 3);
    }
    double dtq = Now() - tq;
    g_totalTime += dtq;
    printf("  %-28s %7.2f ms  %6d tris (%d clusters, %d palms)\n", "MakePalmCluster", dtq * 1000.0, palmTris,
           PalmClusterCount(), PalmCount());
    SaveTexture("palm_impostor", MakePalmImpostorTexture());
    g_level->ring = MakeRing();
    g_level->propsTex = TimedImage("MakePropsTexture", MakePropsTexture);
    SaveTexture("props", g_level->propsTex);
    for (const MeshData& m : g_level->palmClusters) {
        DrawOptions o = Opts(&g_level->propsTex, Blend::ALPHA_TEST, 0.0f);
        o.unlit = true;
        g_level->extra.push_back(Part{&m, MatIdentity(), o});
    }
    for (const MeshData& m : g_level->propGroups) {
        DrawOptions o = Opts(&g_level->propsTex, Blend::ALPHA_TEST, 0.0f);
        o.unlit = true;
        g_level->extra.push_back(Part{&m, MatIdentity(), o});
    }
    for (int i = 0; i < RingCount(); i++)
        g_level->extra.push_back(
            Part{&g_level->ring, MatRotate(RingRotation(i)) * MatTranslate(RingPosition(i)), Opts(&g_level->propsTex)});
    g_level->terrainTex = TimedImage("MakeTerrainDetailTexture", MakeTerrainDetailTexture);
    g_level->waterTex = TimedImage("MakeSeaTexture", MakeSeaTexture);
    g_level->macroTex = TimedImage("MakeTerrainMacroTexture", MakeTerrainMacroTexture);
    g_level->seaDetailTex = TimedImage("MakeSeaDetailTexture", MakeSeaDetailTexture);
    SaveTexture("terrain_macro", g_level->macroTex);
    SaveTexture("sea_detail", g_level->seaDetailTex);
    SaveTexture("terrain", g_level->terrainTex);
    SaveTexture("water", g_level->waterTex);
}

// Renders the level from eye toward target.
static void LevelShot(const char* name, const Vec3& eye, const Vec3& target, float fovDeg = 60.0f, int size = 1280,
                      bool fog = true)
{
    Raster r(size, size * 3 / 4);
    r.SetCamera(eye, QuatLook(target - eye, Vec3(0, 1, 0)), fovDeg * DEG, 2.0f, 16000.0f);
    preview::Light light;
    if (!fog) {
        light.fogStart = 1e6f;
        light.fogEnd = 2e6f;
    }
    r.SetLight(light);
    r.ClearSky(Vec3(0.165f, 0.384f, 0.722f), Vec3(0.79f, 0.87f, 0.92f));
    DrawOptions terrain = Opts(&g_level->macroTex, Blend::OPAQUE, 0.0f);
    terrain.unlit = true;
    terrain.detail = &g_level->terrainTex;
    terrain.detailScale = 25.0f;
    for (const MeshData& t : g_level->tiles)
        r.Draw(t, MatIdentity(), terrain);
    for (const Part& p : g_level->extra) {
        if (p.options.blend == Blend::OPAQUE || p.options.blend == Blend::ALPHA_TEST)
            r.Draw(*p.mesh, p.model, p.options);
    }
    DrawOptions sea = Opts(&g_level->waterTex, Blend::ALPHA, 0.9f);
    sea.detail = &g_level->seaDetailTex;
    sea.detailScale = 0.2f;
    sea.shininess = 90.0f;
    r.Draw(g_level->sea, MatIdentity(), sea);
    for (const Part& p : g_level->extra) {
        if (p.options.blend == Blend::ALPHA || p.options.blend == Blend::ADDITIVE)
            r.Draw(*p.mesh, p.model, p.options);
    }
    Save(std::string("level_") + name, r);
}

static void PreviewLevel()
{
    BuildLevel();
    LevelShot("overview_south", Vec3(400, 1700, 6200), Vec3(0, 100, 0), 55.0f);
    LevelShot("overview_northeast", Vec3(5200, 1500, -5200), Vec3(0, 150, 0), 55.0f);
    LevelShot("map_top", Vec3(0, 15000, 1), Vec3(0, 0, 0), 32.0f, 1024, false);
    Vec3 ring1 = RingPosition(1);
    LevelShot("canyon", ring1 + Vec3(-260, 20, 0), RingPosition(3), 65.0f);
    Vec3 arch = CanyonArchPosition();
    Vec3 archEye(arch.x - 380.0f, 45.0f, CanyonCenterZ(arch.x - 380.0f));
    LevelShot("canyon_arch", archEye, arch + Vec3(0, 20, 0), 65.0f);
    LevelShot("canyon_arch_close", Vec3(arch.x - 120.0f, 30.0f, arch.z + 40.0f), arch + Vec3(0, 50, 0), 65.0f);
    Vec3 h = HarbourPosition();
    LevelShot("harbour", h + Vec3(-250, 140, 950), h + Vec3(0, 0, -250), 60.0f);
    Vec3 pb = PirateBasePosition();
    LevelShot("pirate_base", pb + Vec3(-260, -40, 330), pb + Vec3(0, -60, 0), 60.0f);
    Vec3 v = VolcanoCraterPosition();
    LevelShot("volcano", v + Vec3(900, 250, 1200), v, 60.0f);
    // Chase camera behind the player over the harbour, a pirate ahead, the zeppelin beyond.
    {
        static MeshData player = MakePlane(Livery::PLAYER_RED);
        static MeshData playerGlass = MakeCanopyGlass(Livery::PLAYER_RED);
        static MeshData playerProp = MakePropellerDisc(Livery::PLAYER_RED);
        static MeshData scarf = MakeScarf();
        static MeshData pirate = MakePlane(Livery::PIRATE_GREEN);
        static MeshData ace = MakePlane(Livery::PIRATE_PURPLE);
        static MeshData zep = MakeZeppelin();
        static MeshData engine = MakeZeppelinEngine();
        static ImageData playerTex = MakePlaneTexture(Livery::PLAYER_RED);
        static ImageData pirateTex = MakePlaneTexture(Livery::PIRATE_GREEN);
        static ImageData aceTex = MakePlaneTexture(Livery::PIRATE_PURPLE);
        static ImageData discTex = MakePropellerTexture();
        static ImageData zepTex = MakeZeppelinTexture();
        Vec3 pos = h + Vec3(-300, 140, 700);
        Vec3 fwd = Normalize(Vec3(0.25f, -0.05f, -1.0f));
        Quat q = QuatLook(fwd, Vec3(0.25f, 1.0f, 0.0f));
        Mat4 m = MatRotate(q) * MatTranslate(pos);
        size_t keep = g_level->extra.size();
        g_level->extra.push_back(Part{&player, m, Opts(&playerTex, Blend::OPAQUE, 0.5f)});
        g_level->extra.push_back(Part{&scarf, m, Opts(&playerTex)});
        g_level->extra.push_back(Part{&playerGlass, m, Opts(&playerTex, Blend::ALPHA, 0.9f)});
        g_level->extra.push_back(Part{&playerProp, MatTranslate(PropellerHub()) * m, Opts(&discTex, Blend::ALPHA)});
        Mat4 pm = MatRotate(QuatLook(Vec3(-0.6f, 0.1f, -1.0f), Vec3(-0.5f, 1, 0))) *
                  MatTranslate(pos + fwd * 70.0f + Vec3(12, 6, 0));
        g_level->extra.push_back(Part{&pirate, pm, Opts(&pirateTex, Blend::OPAQUE, 0.5f)});
        Mat4 am = MatRotate(QuatLook(Vec3(0.8f, -0.1f, -1.0f), Vec3(0.6f, 1, 0))) *
                  MatTranslate(pos + fwd * 140.0f + Vec3(-40, 20, 0));
        g_level->extra.push_back(Part{&ace, am, Opts(&aceTex, Blend::OPAQUE, 0.5f)});
        Mat4 zm = MatRotate(QuatAxisAngle(Vec3(0, 1, 0), 1.2f)) * MatTranslate(pos + fwd * 1100.0f + Vec3(0, 260, 0));
        g_level->extra.push_back(Part{&zep, zm, Opts(&zepTex, Blend::OPAQUE, 0.4f)});
        for (int i = 0; i < ZeppelinEngineCount(); i++)
            g_level->extra.push_back(Part{&engine, MatTranslate(ZeppelinEngine(i)) * zm, Opts(&zepTex)});
        Vec3 eye = pos - fwd * 13.0f + Vec3(0, 3.2f, 0);
        LevelShot("chase", eye, pos + fwd * 25.0f, 58.0f);
        g_level->extra.resize(keep);
    }
    // The boss's arrival: the zeppelin sliding out from behind the pirate tower.
    {
        static MeshData zep = MakeZeppelin();
        static ImageData zepTex = MakeZeppelinTexture();
        Vec3 eye;
        Vec3 target;
        PirateTowerShot(eye, target);
        for (int k = 0; k < 2; k++) {
            Vec3 a = ZeppelinEmergePathPoint(k == 0 ? 0 : 1);
            Vec3 b = ZeppelinEmergePathPoint(k == 0 ? 1 : 2);
            Vec3 at = Lerp(a, b, k == 0 ? 0.3f : 0.2f);
            Mat4 zm = MatRotate(QuatLook(b - a, Vec3(0, 1, 0))) * MatTranslate(at);
            size_t keep = g_level->extra.size();
            g_level->extra.push_back(Part{&zep, zm, Opts(&zepTex, Blend::OPAQUE, 0.4f)});
            LevelShot(k == 0 ? "tower_emerge_0" : "tower_emerge_1", eye, target, 55.0f);
            g_level->extra.resize(keep);
        }
    }
    Vec3 lamp = LighthouseLampPosition();
    LevelShot("lighthouse", lamp + Vec3(-60, 5, 90), lamp + Vec3(0, -12, 0), 60.0f);
    LevelShot("village", h + Vec3(-150, 60, -150), h + Vec3(0, 5, -480), 60.0f);
    LevelShot("village_close", h + Vec3(-60, 18, -330), h + Vec3(20, 8, -560), 60.0f);
    Vec3 village = HarbourVillagePosition();
    Vec3 lamp0 = LighthouseLampPosition();
    LevelShot("harbour_intro", village + Vec3(-80, 70, -140), Lerp(village, lamp0, 0.45f), 60.0f);
    Vec3 low = village + Vec3(-1300.0f, 0.0f, -200.0f);
    low.y = BuildTerrain().HeightAt(low.x, low.z) + 50.0f;
    Vec3 lowAt = low + Vec3(-150.0f, -40.0f, -250.0f);
    Vec3 wharf = HarbourWharfPosition();
    LevelShot("intro_wharf", wharf + Vec3(-110, 30, -10), wharf + Vec3(80, -5, 35), 60.0f);
    LevelShot("jungle_low", low, lowAt, 60.0f);
    LevelShot("harbour_300m", village + Vec3(-250, 160, 420), village + Vec3(0, 0, 20), 55.0f);
}

// --- Small meshes and textures

static void PreviewItems()
{
    printf("items\n");
    ImageData props = MakePropsTexture();
    static MeshData (*const MAKERS[5])() = {MakeBanana, MakeBananaRocket, MakeCrate, MakeRing, MakeSkyDome};
    static const char* NAMES[5] = {"banana", "banana_rocket", "crate", "ring", "sky_dome"};
    for (int i = 0; i < 5; i++) {
        MeshData m = TimedMesh(NAMES[i], MAKERS[i]);
        std::vector<Part> parts;
        DrawOptions o = Opts(i == 4 ? nullptr : &props, Blend::OPAQUE, 0.4f);
        if (i == 4) {
            o.unlit = true;
            o.cull = false;
        }
        parts.push_back(Part{&m, MatIdentity(), o});
        ViewParts(NAMES[i], parts);
    }
    static ImageData (*const TEXTURES[6])() = {MakeParticleAtlas, MakeCloudTexture, MakeHudTexture, MakeTitleTexture,
                                               MakeShadowTexture, MakeFontTexture};
    static const char* TEX_NAMES[6] = {"particles", "clouds", "hud", "title", "shadow", "font"};
    for (int i = 0; i < 6; i++)
        SaveTexture(TEX_NAMES[i], TimedImage(TEX_NAMES[i], TEXTURES[i]));
    MeshData scarf = TimedMesh("MakeScarf", MakeScarf);
    MeshData seg = MakeScarfSegment(0);
    printf("  scarf segment %d tris\n", static_cast<int>(seg.indices.size() / 3));
}

// --- Load profile

// Runs f a few times and returns the fastest run in milliseconds.
template <typename F>
static double Best(F f)
{
    double best = 1e9;
    for (int i = 0; i < 3; i++) {
        double t0 = Now();
        f();
        double dt = Now() - t0;
        if (dt < best)
            best = dt;
    }
    return best * 1000.0;
}

// Host time of every content call the game makes while loading (src/game/assets.cpp), in its order, with a
// Pentium III 733 MHz estimate (the console is about 4 times slower per core than this host).
static void ProfileLoad()
{
    constexpr double CONSOLE = 4.0;
    double total = 0.0;
    auto row = [&](const char* name, double ms) {
        total += ms;
        printf("  %-30s %8.2f ms   console ~%7.1f ms\n", name, ms, ms * CONSOLE);
    };
    printf("load profile (host, best of 3)\n");
    row("MakeTitleTexture", Best([]() {
        MakeTitleTexture();
    }));
    row("BuildTerrain", Best([]() {
        BuildTerrain();
    }));
    row("MakeTerrainTile x64", Best([]() {
        for (int i = 0; i < TerrainTileCount(); i++)
            MakeTerrainTile(i);
    }));
    row("MakePropGroup x16", Best([]() {
        for (int i = 0; i < PropGroupCount(); i++)
            MakePropGroup(i);
    }));
    row("MakeSea", Best([]() {
        MakeSea();
    }));
    row("planes: meshes x3", Best([]() {
        for (int i = 0; i < 3; i++) {
            Livery l = static_cast<Livery>(i);
            MakePlane(l);
            MakeCanopyGlass(l);
            MakePropeller(l);
            MakePropellerDisc(l);
        }
    }));
    row("MakePlaneTexture x3", Best([]() {
        for (int i = 0; i < 3; i++)
            MakePlaneTexture(static_cast<Livery>(i));
    }));
    row("MakePropellerTexture", Best([]() {
        MakePropellerTexture();
    }));
    row("zeppelin meshes", Best([]() {
        MakeZeppelin();
        MakeZeppelinBody();
        MakeZeppelinFrame();
        for (int i = 0; i < ZeppelinHullSectionCount(); i++)
            MakeZeppelinHullSection(i);
        MakeTurret();
        MakeZeppelinEngine();
    }));
    row("MakeZeppelinTexture", Best([]() {
        MakeZeppelinTexture();
    }));
    row("rocket, crate, ring, scarf", Best([]() {
        MakeBananaRocket();
        MakeCrate();
        MakeRing();
        for (int i = 0; i < ScarfSegmentCount(); i++)
            MakeScarfSegment(i);
    }));
    row("MakeTerrainMacroTexture", Best([]() {
        MakeTerrainMacroTexture();
    }));
    row("MakeTerrainDetailTexture", Best([]() {
        MakeTerrainDetailTexture();
    }));
    row("MakeSeaTexture", Best([]() {
        MakeSeaTexture();
    }));
    row("MakeSeaDetailTexture", Best([]() {
        MakeSeaDetailTexture();
    }));
    row("MakePropsTexture", Best([]() {
        MakePropsTexture();
    }));
    row("MakeParticleAtlas", Best([]() {
        MakeParticleAtlas();
    }));
    row("MakeCloudTexture", Best([]() {
        MakeCloudTexture();
    }));
    row("MakeHudTexture", Best([]() {
        MakeHudTexture();
    }));
    printf("  %-30s %8.2f ms   console ~%7.1f ms\n", "total", total, total * CONSOLE);
}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--profile") == 0) {
        ProfileLoad();
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: preview OUT_DIR | preview --profile\n");
        return 2;
    }
    g_out = argv[1];
    mkdir(g_out.c_str(), 0755);
    PreviewPlanes();
    PreviewZeppelin();
    PreviewLevel();
    PreviewItems();
    printf("total generation time %.1f ms\n", g_totalTime * 1000.0);
    return 0;
}
