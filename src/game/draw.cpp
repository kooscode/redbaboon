// Drawing the world (docs/design.md §5, creative brief §1, §3): late-afternoon light and fog, terrain tiles culled to
// the view, props, planes with propellers, the zeppelin, rings and crates; then the transparent pass back to front:
// sea, blob shadows, propeller discs, the cumulus field, the volcano's smoke plume, particles, tracers, wingtip vapour
// and speed lines; then the lens flare and the cloud fly-through fade over the image.
#include "game/world.h"

namespace game {

constexpr int MAX_PUFFS = 420;
constexpr int CLOUD_CLUSTERS = 34;
constexpr float CLOUD_DRAW_RANGE = 6500.0f;
constexpr int PLUME_PUFFS = 12;
constexpr float PLUME_CYCLE = 24.0f;       // seconds a plume puff takes to rise and fade
constexpr int MAX_STREAKS = 64;
constexpr float SHADOW_HEIGHT = 200.0f;    // planes cast a blob shadow below this height
constexpr float VAPOUR_LOAD = 3.5f;        // wingtip vapour above this load factor (g-equivalents)
constexpr float TILE_LOD_DISTANCE = 600.0f;  // tiles farther than this draw at half resolution
constexpr float PROPS_RANGE = 3500.0f;     // prop groups beyond this are not drawn
constexpr float PALM_MESH_RANGE = 150.0f;  // palms: the cluster's mesh up to here, a billboard each beyond
constexpr float PALM_RANGE = 600.0f;       // and none beyond this (the terrain is darker under each cluster)
constexpr float DETAIL_RANGE = 400.0f;     // planes' propellers, canopy and scarf are drawn only this close
constexpr float PLANE_LOD_DISTANCE = 300.0f; // planes farther than this draw their coarse mesh
constexpr float ENEMY_RIM_DISTANCE = 250.0f; // the pirates' rim grows by its own size every this many metres
constexpr float PROP_DISC_RPS = 30.0f;     // above this the propeller is drawn as a disc
constexpr float SCREEN_ASPECT = 640.0f / 480.0f;
constexpr Vec3 WIND = Vec3(0.86f, 0.0f, -0.5f); // plume drift direction (unit; to the north-east, downwind)

struct Puff {
    Vec3 position;
    float size;
    int frame;
    uint32_t color;
};

// The cumulus field.
static Puff puffs[MAX_PUFFS];
static int puffCount = 0;

// Draw buffers.
static render::Billboard cloudBoards[MAX_PUFFS];
static render::Streak extraStreaks[MAX_STREAKS];

// Screen effects: the cloud fade (0..0.7) and the lens flare's visibility (0..1).
static float cloudFade = 0.0f;
static float flareVisible = 0.0f;

// Random numbers for drawing only (the simulation's RNG must not depend on the frame rate).
static Rng drawRng(0xC10D5u);

// --- Environment and culling

static Vec3 Rgb(uint32_t rgb)
{
    return Vec3(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f);
}

// The renderer's defaults are the brief's late afternoon: sun 32 degrees high in the south-west.
render::Environment WorldEnvironment()
{
    return render::Environment();
}

bool SphereVisible(const render::Camera& camera, const Vec3& center, float radius)
{
    Vec3 rel = Rotate(QuatConjugate(camera.rotation), center - camera.position);
    float depth = -rel.z;
    if (depth + radius < camera.nearZ || depth - radius > camera.farZ)
        return false;
    float tanY = tanf(camera.fovY * 0.5f);
    float tanX = tanY * SCREEN_ASPECT;
    if (fabsf(rel.x) > depth * tanX + radius * sqrtf(1.0f + tanX * tanX))
        return false;
    return fabsf(rel.y) <= depth * tanY + radius * sqrtf(1.0f + tanY * tanY);
}

// --- Clouds

void BuildClouds()
{
    Rng rng(0xC0FFEEu);
    puffCount = 0;
    for (int c = 0; c < CLOUD_CLUSTERS; c++) {
        Vec3 center(rng.Range(-4500.0f, 4500.0f), rng.Range(500.0f, 820.0f), rng.Range(-4500.0f, 4500.0f));
        // The cloud bank the zeppelin comes out of: the last clusters gather to the north-east.
        if (c >= CLOUD_CLUSTERS - 6) {
            Vec3 bank = Normalize(Vec3(assets.pirateBase.x, 0.0f, assets.pirateBase.z));
            if (LengthSq(bank) < 0.01f)
                bank = Normalize(Vec3(1.0f, 0.0f, -1.0f));
            center = bank * rng.Range(2700.0f, 3300.0f) + Vec3(rng.Range(-500, 500), 0.0f, rng.Range(-500, 500));
            center.y = rng.Range(450.0f, 600.0f);
        }
        int count = 6 + static_cast<int>(rng.Uniform() * 7.0f);
        float width = rng.Range(120.0f, 260.0f);
        for (int i = 0; i < count && puffCount < MAX_PUFFS; i++) {
            Puff& p = puffs[puffCount++];
            float up = rng.Range(-0.3f, 1.0f);
            p.position = center + Vec3(rng.Range(-width, width), up * width * 0.35f, rng.Range(-width, width) * 0.6f);
            p.size = rng.Range(110.0f, 220.0f);
            p.frame = static_cast<int>(rng.Uniform() * 3.99f);
            // Lit tops, cool blue-grey undersides.
            float lit = Clamp((up + 0.3f) / 1.3f, 0.0f, 1.0f);
            Vec3 color = Lerp(Rgb(0xB9C6D6), Vec3(1.0f, 1.0f, 1.0f), lit);
            p.color = PackARGB(color.x, color.y, color.z, 0.9f);
        }
    }
}

void UpdateClouds(float dt)
{
    cloudFade = fmaxf(0.0f, cloudFade - dt * (0.7f / 0.3f));
    const Plane& p = Player();
    if (world.titleMode || !p.alive)
        return;
    for (int i = 0; i < puffCount; i++) {
        float inside = puffs[i].size * 0.35f;
        if (LengthSq(p.flight.position - puffs[i].position) < inside * inside) {
            cloudFade = 0.7f;
            CloudWisp(p.flight.position, p.flight.velocity);
            return;
        }
    }
}

// Cloud and plume billboards, culled and faded near the camera.
static void DrawClouds(const render::Camera& camera)
{
    int count = 0;
    for (int i = 0; i < puffCount; i++) {
        const Puff& p = puffs[i];
        Vec3 rel = p.position - camera.position;
        float distance = Length(rel);
        if (distance > CLOUD_DRAW_RANGE || !SphereVisible(camera, p.position, p.size))
            continue;
        // Fade out when the camera is about to enter the puff (no hard intersection).
        float nearFade = Clamp((distance - p.size * 0.3f) / (p.size * 0.7f), 0.0f, 1.0f);
        uint32_t alpha = static_cast<uint32_t>(((p.color >> 24) & 0xFF) * nearFade);
        if (alpha < 4)
            continue;
        render::Billboard& b = cloudBoards[count];
        b.position = p.position;
        b.size = p.size;
        b.rotation = 0.0f;
        b.color = (p.color & 0x00FFFFFF) | (alpha << 24);
        b.frame = p.frame;
        count++;
    }
    // One call: the renderer sorts the puffs and the plume back to front together.
    if (count > 0)
        render::DrawBillboards(assets.clouds, 2, 2, cloudBoards, count, render::Blend::ALPHA);
}

// The volcano's smoke plume: a tall column of big smoke puffs that rise and drift downwind on a loop, grey at the
// crater and paler above; seen from anywhere on the map.
static void DrawPlume()
{
    render::Billboard plumePuffs[PLUME_PUFFS];
    float time = world.time + 1000.0f;
    for (int i = 0; i < PLUME_PUFFS; i++) {
        float t = fmodf(time + i * (PLUME_CYCLE / PLUME_PUFFS), PLUME_CYCLE) / PLUME_CYCLE;
        render::Billboard& b = plumePuffs[i];
        b.position = assets.volcano + Vec3(0.0f, 30.0f + t * 520.0f, 0.0f) + WIND * (t * PLUME_CYCLE * 4.0f);
        b.size = 60.0f + t * 70.0f;
        b.rotation = i * 1.7f + t;
        Vec3 color = Lerp(Rgb(0x8A8580), Rgb(0xC9C4BE), t);
        float alpha = Clamp(t * 8.0f, 0.0f, 1.0f) * Clamp((1.0f - t) * 3.0f, 0.0f, 1.0f) * 0.9f;
        b.color = PackARGB(color.x, color.y, color.z, alpha);
        b.frame = i % 4;
    }
    render::DrawBillboards(assets.particles, 4, 4, plumePuffs, PLUME_PUFFS, render::Blend::ALPHA);
}

// --- Opaque world

// True when a sphere is in view and not entirely beyond the fog (where it would be sky-coloured anyway).
static bool WorthDrawing(const render::Camera& camera, const Vec3& center, float radius, float range)
{
    if (LengthSq(center - camera.position) > (range + radius) * (range + radius))
        return false;
    return SphereVisible(camera, center, radius);
}

// Palms up close as their cluster's mesh (cut off at PALM_MESH_RANGE per pixel by the renderer), from there to
// PALM_RANGE as one camera-facing billboard each.
static void DrawPalms(const render::Camera& camera, render::Material mesh)
{
    static render::Billboard boards[MAX_PALMS];
    mesh.maxDistance = PALM_MESH_RANGE;
    int count = 0;
    for (int i = 0; i < assets.palmClusterCount; i++) {
        const Vec3& center = assets.palmClusterCenter[i];
        float radius = assets.palmClusterRadius[i];
        float distance = Length(center - camera.position);
        if (distance - radius > PALM_RANGE || !SphereVisible(camera, center, radius))
            continue;
        if (distance - radius < PALM_MESH_RANGE)
            render::DrawMesh(assets.palmCluster[i], MatIdentity(), mesh);
        if (distance + radius < PALM_MESH_RANGE)
            continue;
        int first = assets.palmClusterFirst[i];
        for (int k = 0; k < assets.palmClusterSize[i]; k++) {
            const render::Billboard& palm = assets.palmBoard[first + k];
            float d = Length(palm.position - camera.position);
            if (d >= PALM_MESH_RANGE && d <= PALM_RANGE)
                boards[count++] = palm;
        }
    }
    if (count > 0)
        render::DrawBillboards(assets.palmImpostor, 1, 1, boards, count, render::Blend::ALPHA_TEST);
}

// Terrain tiles and prop groups, with their lighting baked in (unlit: the short vertex program).
static void DrawTerrain(const render::Camera& camera)
{
    float fogEnd = WorldEnvironment().fogEnd;
    render::Material ground;
    ground.texture = assets.terrainTexture;
    ground.detail = assets.terrainDetail;
    ground.detailScale = {25.0f, 25.0f};
    ground.unlit = true;
    for (int i = 0; i < assets.tileCount; i++) {
        if (!WorthDrawing(camera, assets.tileCenter[i], assets.tileRadius[i], fogEnd))
            continue;
        float distance = Length(assets.tileCenter[i] - camera.position) - assets.tileRadius[i];
        render::DrawMesh(distance > TILE_LOD_DISTANCE ? assets.tileLod[i] : assets.tiles[i], MatIdentity(), ground);
    }
    render::Material props;
    props.texture = assets.propsTexture;
    props.blend = render::Blend::ALPHA_TEST;
    props.unlit = true;
    for (int i = 0; i < assets.propGroupCount; i++) {
        if (WorthDrawing(camera, assets.propGroupCenter[i], assets.propGroupRadius[i], PROPS_RANGE))
            render::DrawMesh(assets.propGroup[i], MatIdentity(), props);
    }
    DrawPalms(camera, props);
}

static int LiveryIndex(PlaneKind kind)
{
    if (kind == PlaneKind::PLAYER)
        return 0;
    return kind == PlaneKind::ACE ? 2 : 1;
}

static float PropellerRps(const Plane& p)
{
    if (!p.alive)
        return 6.0f;
    return 12.0f + p.flight.throttle * 40.0f;
}

static Mat4 PlaneModel(const Plane& p)
{
    Vec3 position = Lerp(p.prevPosition, p.flight.position, world.drawAlpha);
    Quat rotation = QuatSlerp(p.prevRotation, p.flight.rotation, world.drawAlpha);
    return MatTRS(position, rotation, Vec3(1, 1, 1));
}

static Mat4 PropellerModel(const Plane& p, int livery)
{
    return MatRotate(QuatAxisAngle(Vec3(0, 0, 1), p.propAngle)) * MatTranslate(assets.propellerHub[livery]);
}

// The player's scarf: segments chained from the anchor, each bent a little more by a travelling sine wave.
static void DrawScarf(const Plane& p, const Mat4& model, const render::Material& material)
{
    float speed = Clamp(p.flight.speed / 100.0f, 0.4f, 1.4f);
    Mat4 chain = MatTranslate(assets.scarfAnchor);
    for (int i = 0; i < assets.scarfCount; i++) {
        float phase = world.time * 14.0f * speed - i * 1.3f;
        float bend = sinf(phase) * (0.12f + 0.06f * i);
        float sway = sinf(phase * 0.7f + 1.0f) * 0.08f * i;
        Mat4 local = MatRotate(QuatAxisAngle(Vec3(0, 1, 0), sway) * QuatAxisAngle(Vec3(1, 0, 0), bend));
        if (i > 0)
            local = local * MatTranslate(Vec3(0.0f, 0.0f, assets.scarfLength));
        chain = local * chain;
        render::DrawMesh(assets.scarf[i], chain * model, material);
    }
}

static void DrawPlanes(const render::Camera& camera)
{
    for (int i = 0; i < MAX_PLANES; i++) {
        const Plane& p = world.planes[i];
        if (!p.active || !SphereVisible(camera, p.flight.position, 10.0f))
            continue;
        Mat4 model = PlaneModel(p);
        int livery = LiveryIndex(p.kind);
        render::Material body;
        body.texture = assets.planeTexture[livery];
        body.specular = 0.5f;
        body.shininess = 32.0f;
        body.rim = i == PLAYER_PLANE ? 0.25f : 0.35f; // a warm rim lifts the pirates off the jungle
        if (i != PLAYER_PLANE)
            body.rimDistance = ENEMY_RIM_DISTANCE;     // and grows with distance, so they read far away
        float flash = p.hitFlash;
        body.emissive = Vec3(flash, flash, flash);
        // A burning wreck glows orange as it falls.
        if (!p.alive) {
            body.tint = 0xFF605850;
            float glow = 0.6f * (0.8f + 0.2f * sinf(world.time * 30.0f + i));
            body.emissive = Vec3(glow, glow * 0.42f, glow * 0.1f);
        }
        // Respawn invulnerability blinks.
        if (p.invulnerable > 0.0f && fmodf(p.invulnerable, 0.25f) < 0.08f)
            body.emissive = Vec3(0.4f, 0.4f, 0.4f);
        float distanceSq = LengthSq(p.flight.position - camera.position);
        bool distant = distanceSq > PLANE_LOD_DISTANCE * PLANE_LOD_DISTANCE;
        render::DrawMesh(distant ? assets.planeDistant[livery] : assets.plane[livery], model, body);
        // Small parts only near the camera: at range they are a pixel or two and cost a draw each.
        bool near = distanceSq < DETAIL_RANGE * DETAIL_RANGE;
        if (i == PLAYER_PLANE && near)
            DrawScarf(p, model, body);
        if (near && PropellerRps(p) < PROP_DISC_RPS) {
            render::Material blades = body;
            blades.specular = 0.8f;
            render::DrawMesh(assets.propeller[livery], PropellerModel(p, livery) * model, blades);
        }
    }
}

// --- Transparent world

static void DrawSea()
{
    render::Material sea;
    sea.texture = assets.waterTexture;
    sea.detail = assets.seaDetail;
    sea.detailScale = {0.2f, 0.2f};
    sea.detailOffset = {fmodf(world.time * -0.002f + 1.0f, 1.0f), fmodf(world.time * 0.003f, 1.0f)};
    sea.blend = render::Blend::ALPHA;
    sea.specular = 1.0f;
    sea.shininess = 100.0f;
    float t = world.time;
    sea.uvOffset = {fmodf(t * 0.012f, 1.0f), fmodf(t * 0.007f, 1.0f)};
    sea.fogDensity = 1.6f;
    render::DrawMesh(assets.sea, MatIdentity(), sea);
    render::DrawSeaGlints(world.time);
}

static void DrawShadow(const Vec3& at, float width, float length, float heading, float alpha)
{
    if (alpha <= 0.01f)
        return;
    float ground = GroundHeight(at.x, at.z);
    Vec3 position(at.x, ground + 0.1f, at.z);
    Quat rot = QuatAxisAngle(WORLD_UP, heading);
    if (ground > 0.01f) {
        Vec3 normal = assets.terrain->NormalAt(at.x, at.z);
        Vec3 fwd = Rotate(rot, Vec3(0, 0, -1));
        fwd = Normalize(fwd - normal * Dot(fwd, normal));
        rot = QuatLook(fwd, normal);
    }
    render::Material m;
    m.texture = assets.shadow;
    m.blend = render::Blend::ALPHA;
    m.unlit = true;
    m.tint = PackARGB(1.0f, 1.0f, 1.0f, alpha);
    m.depthBias = 0.4f;
    render::DrawMesh(assets.shadowQuad, MatTRS(position, rot, Vec3(width, 1.0f, length)), m);
}

static void DrawShadows()
{
    Vec3 sun = WorldEnvironment().sunDirection;
    for (int i = 0; i < MAX_PLANES; i++) {
        const Plane& p = world.planes[i];
        if (!p.active)
            continue;
        Vec3 pos = p.flight.position;
        float height = pos.y - GroundHeight(pos.x, pos.z);
        float alpha = Clamp(1.0f - height / SHADOW_HEIGHT, 0.0f, 1.0f);
        // Cast along the low sun, so it lies beside the plane where the chase camera sees it.
        Vec3 fwd = QuatForward(p.flight.rotation);
        Vec3 at = pos - sun * (fmaxf(height, 0.0f) / sun.y);
        DrawShadow(at, 12.0f, 10.0f, atan2f(-fwd.x, -fwd.z), alpha);
    }
    const Zeppelin& z = world.zeppelin;
    if (z.state == ZeppelinState::ABSENT || z.state == ZeppelinState::SUNK)
        return;
    // The zeppelin's shadow: a long ellipse straight below it, on the sea and the land.
    DrawShadow(z.position, 30.0f, 180.0f, -z.heading, 0.35f);
}

// Canopy glass and spinning propeller discs (translucent, after the opaque world).
static void DrawPlaneGlass(const render::Camera& camera)
{
    render::Material disc;
    disc.texture = assets.propellerTexture;
    disc.blend = render::Blend::ALPHA;
    render::Material glass;
    glass.blend = render::Blend::ALPHA;
    glass.specular = 0.9f;
    glass.shininess = 60.0f;
    for (int i = 0; i < MAX_PLANES; i++) {
        const Plane& p = world.planes[i];
        if (!p.active || !SphereVisible(camera, p.flight.position, 10.0f))
            continue;
        if (LengthSq(p.flight.position - camera.position) > DETAIL_RANGE * DETAIL_RANGE)
            continue;
        int livery = LiveryIndex(p.kind);
        Mat4 model = PlaneModel(p);
        glass.texture = assets.planeTexture[livery];
        render::DrawMesh(assets.canopy[livery], model, glass);
        if (PropellerRps(p) >= PROP_DISC_RPS)
            render::DrawMesh(assets.propellerDisc[livery], PropellerModel(p, livery) * model, disc);
    }
}

// The lighthouse lamp: a pulsing glow (fake bloom).
static void DrawLamp(const render::Camera& camera)
{
    if (!SphereVisible(camera, assets.lighthouseLamp, 20.0f))
        return;
    render::Billboard lamp;
    lamp.position = assets.lighthouseLamp;
    lamp.size = 14.0f + 4.0f * sinf(world.time * 2.0f);
    lamp.color = 0xC0FFE8A0;
    lamp.frame = 11;
    render::DrawBillboards(assets.particles, 4, 4, &lamp, 1, render::Blend::ADDITIVE);
}

// Wingtip vapour in hard turns and speed lines at high speed.
static void DrawAirStreaks(const render::Camera& camera)
{
    int count = 0;
    for (int i = 0; i < MAX_PLANES && count < MAX_STREAKS - 2; i++) {
        const Plane& p = world.planes[i];
        if (!p.alive || p.flight.turnLoad < VAPOUR_LOAD)
            continue;
        float strength = Clamp((p.flight.turnLoad - VAPOUR_LOAD) / 2.0f, 0.2f, 1.0f);
        for (int side = -1; side <= 1; side += 2) {
            Vec3 tip = p.flight.position + Rotate(p.flight.rotation, Vec3(side * 5.6f, 0.2f, 1.0f));
            render::Streak& s = extraStreaks[count++];
            s.from = tip - p.flight.velocity * 0.18f;
            s.to = tip;
            s.width = 0.35f;
            s.color = PackARGB(1.0f, 1.0f, 1.0f, 0.55f * strength);
        }
    }
    const Plane& player = Player();
    if (player.alive && player.flight.speed > 110.0f && !world.titleMode) {
        // Thin lines along the velocity, only in the outer 12 % of the screen.
        float strength = Clamp((player.flight.speed - 110.0f) / 20.0f, 0.0f, 1.0f);
        Vec3 dir = Normalize(player.flight.velocity);
        Vec3 fwd = QuatForward(camera.rotation);
        Vec3 right = QuatRight(camera.rotation);
        Vec3 up = QuatUp(camera.rotation);
        float tanY = tanf(camera.fovY * 0.5f);
        float tanX = tanY * SCREEN_ASPECT;
        for (int i = 0; i < 10 && count < MAX_STREAKS; i++) {
            float across = drawRng.Range(-1.0f, 1.0f);
            float edge = drawRng.Range(0.76f, 0.98f) * (drawRng.Uniform() < 0.5f ? -1.0f : 1.0f);
            bool sides = drawRng.Uniform() < 0.6f;
            float nx = sides ? edge : across;
            float ny = sides ? across : edge;
            float depth = drawRng.Range(12.0f, 30.0f);
            Vec3 at = camera.position + (fwd + right * (nx * tanX) + up * (ny * tanY)) * depth;
            render::Streak& s = extraStreaks[count++];
            s.from = at - dir * drawRng.Range(8.0f, 14.0f);
            s.to = at;
            s.width = 0.05f;
            s.color = PackARGB(1.0f, 1.0f, 1.0f, 0.25f * strength);
        }
    }
    if (count > 0)
        render::DrawStreaks(assets.streak, extraStreaks, count, render::Blend::ALPHA);
}

void DrawWorld(const render::Camera& camera)
{
    DrawTerrain(camera);
    DrawPlanes(camera);
    DrawZeppelin();
    DrawRingsAndCrates();
    DrawSea();
    DrawShadows();
    DrawPlaneGlass(camera);
    DrawLamp(camera);
    DrawClouds(camera);
    DrawPlume();
    DrawEffects();
    DrawProjectiles();
    DrawAirStreaks(camera);
}

// --- Screen effects

// True when the line from the camera toward the sun is blocked by the island or the zeppelin.
static bool SunBlocked(const render::Camera& camera, const Vec3& sun)
{
    for (int i = 1; i <= 24; i++) {
        Vec3 p = camera.position + sun * (i * 120.0f);
        if (p.y < GroundHeight(p.x, p.z))
            return true;
        if (InsideHull(p, 1.0f))
            return true;
    }
    return false;
}

static void DrawLensFlare(const render::Camera& camera)
{
    // Ghost sizes (pixels) and positions along the sun-to-centre line (1 = the sun, -1 = mirrored).
    constexpr int GHOSTS = 6;
    static const float POSITIONS[GHOSTS] = {1.0f, 0.55f, 0.2f, -0.15f, -0.45f, -0.8f};
    static const float SIZES[GHOSTS] = {150.0f, 26.0f, 44.0f, 18.0f, 60.0f, 34.0f};
    static const uint32_t COLORS[GHOSTS] = {0xFFFFD98A, 0x60FFE0A0, 0x40A0D0FF, 0x50FFC080, 0x30C0FFC0, 0x40FFB060};
    Vec3 sun = WorldEnvironment().sunDirection;
    float sx = 0.0f;
    float sy = 0.0f;
    bool front = Dot(QuatForward(camera.rotation), sun) > 0.2f && render::Project(camera.position + sun * 5000.0f,
                                                                                   sx, sy);
    float w = static_cast<float>(render::ScreenWidth());
    float h = static_cast<float>(render::ScreenHeight());
    bool onScreen = front && sx > -50.0f && sx < w + 50.0f && sy > -50.0f && sy < h + 50.0f;
    float target = onScreen && !SunBlocked(camera, sun) ? 1.0f : 0.0f;
    flareVisible = Approach(flareVisible, target, 1.0f / 0.15f, 1.0f / 60.0f);
    if (flareVisible < 0.02f || !front)
        return;
    // The glow frame of the particle atlas (frame 11: column 3, row 2).
    constexpr float U0 = 0.75f;
    constexpr float V0 = 0.5f;
    constexpr float FRAME = 0.25f;
    float cx = w * 0.5f;
    float cy = h * 0.5f;
    for (int i = 0; i < GHOSTS; i++) {
        float gx = cx + (sx - cx) * POSITIONS[i];
        float gy = cy + (sy - cy) * POSITIONS[i];
        float size = SIZES[i];
        uint32_t alpha = static_cast<uint32_t>(((COLORS[i] >> 24) & 0xFF) * flareVisible);
        uint32_t color = (COLORS[i] & 0x00FFFFFF) | (alpha << 24);
        render::DrawImage(assets.particles, gx - size * 0.5f, gy - size * 0.5f, size, size, U0, V0, U0 + FRAME,
                          V0 + FRAME, color, render::Blend::ADDITIVE);
    }
}

void DrawScreenEffects(const render::Camera& camera)
{
    DrawLensFlare(camera);
    if (cloudFade > 0.01f) {
        uint32_t alpha = static_cast<uint32_t>(cloudFade * 255.0f);
        render::DrawRect(0.0f, 0.0f, static_cast<float>(render::ScreenWidth()),
                         static_cast<float>(render::ScreenHeight()), (alpha << 24) | 0x00FFFFFF);
    }
}

} // namespace game
