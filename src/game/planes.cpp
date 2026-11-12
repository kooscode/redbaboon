// Planes (docs/design.md §8): spawning, the per-step update (pilot, flight model, guns), damage with smoke and
// fire, being shot down (a burning fall that ends in an explosion or a splash), collisions with the ground, the sea
// and the zeppelin, engine sounds, and the targets the player can lock (planes, zeppelin engines and turrets).
#include "game/world.h"

namespace game {

constexpr float PLAYER_HEALTH = 100.0f;
constexpr float FIGHTER_HEALTH = 30.0f;
constexpr float ACE_HEALTH = 52.0f;
constexpr float HIT_FLASH_TIME = 0.08f;
constexpr float CRASH_CLEARANCE = 1.5f; // metres above the ground at which a plane crashes
constexpr float FALL_STEER_SPEED = 140.0f; // a steered fall moves sideways at most this fast
constexpr float COLLIDE_DISTANCE = 7.0f;
constexpr float PIRATE_FLOOR = 12.0f;   // pirates are kept at least this high above the ground
constexpr float ENEMY_VAPOUR_LOAD = 3.0f; // pirates trail coloured vapour above this load factor
constexpr float REGEN_DELAY = 5.0f;     // seconds without a hit before the player's hull starts mending
constexpr float REGEN_LIMIT = 0.3f;     // it mends up to this fraction of full health
constexpr float REGEN_RATE = 0.02f;     // fraction of full health per second

// The engine voice of each plane slot while its plane is in the air (-1: none), so only flying planes use mixer
// voices.
static audio::EngineId engines[MAX_PLANES] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

// --- Spawning

static void ReleaseEngine(int index)
{
    if (engines[index] < 0)
        return;
    audio::DestroyEngine(engines[index]);
    engines[index] = -1;
}

void ClearPlanes()
{
    for (int i = 0; i < MAX_PLANES; i++) {
        ReleaseEngine(i);
        world.planes[i] = Plane();
    }
}

void SilenceEngines()
{
    for (int i = 0; i < MAX_PLANES; i++) {
        if (engines[i] >= 0)
            audio::SetEngine(engines[i], Vec3(), Vec3(), 0.0f, 0.0f);
    }
}

int SpawnPlane(PlaneKind kind, const Vec3& position, const Vec3& direction, float speed)
{
    int first = kind == PlaneKind::PLAYER ? PLAYER_PLANE : 1;
    int last = kind == PlaneKind::PLAYER ? PLAYER_PLANE : MAX_PLANES - 1;
    for (int i = first; i <= last; i++) {
        Plane& p = world.planes[i];
        if (p.active && kind != PlaneKind::PLAYER)
            continue;
        p = Plane();
        if (engines[i] < 0)
            engines[i] = audio::CreateEngine(kind == PlaneKind::PLAYER ? audio::EngineType::PLAYER
                                                                       : audio::EngineType::PIRATE);
        p.active = true;
        p.alive = true;
        p.kind = kind;
        bool ace = kind == PlaneKind::ACE;
        p.params = kind == PlaneKind::PLAYER ? &PLAYER_FLIGHT : (ace ? &ACE_FLIGHT : &FIGHTER_FLIGHT);
        p.maxHealth = kind == PlaneKind::PLAYER ? PLAYER_HEALTH : (ace ? ACE_HEALTH : FIGHTER_HEALTH);
        p.health = p.maxHealth;
        p.rockets = kind == PlaneKind::PLAYER ? PLAYER_ROCKETS : 0;
        p.ai.skill = kind == PlaneKind::ACE ? 0.85f : 0.35f;
        p.ai.evadeTurn = world.rng.Uniform() < 0.5f ? -1.0f : 1.0f;
        ResetFlight(p.flight, position, QuatLook(Normalize(direction), WORLD_UP), speed);
        p.prevPosition = position;
        p.prevRotation = p.flight.rotation;
        return i;
    }
    return -1;
}

int CountEnemies()
{
    int count = 0;
    for (int i = 1; i < MAX_PLANES; i++) {
        if (world.planes[i].active)
            count++;
    }
    return count;
}

// --- Damage

static void ShootDown(int index, bool byPlayer)
{
    Plane& p = world.planes[index];
    p.alive = false;
    p.dyingTime = 0.0f;
    p.dyingSpin = world.rng.Range(1.5f, 3.5f) * (world.rng.Uniform() < 0.5f ? -1.0f : 1.0f);
    p.trigger = false;
    Explosion(p.flight.position, 18.0f, p.flight.velocity);
    if (index == PLAYER_PLANE)
        OnPlayerDown();
    else
        OnPlaneDestroyed(index, byPlayer);
}

void DamagePlane(int index, float amount, bool byPlayer, const Vec3& at)
{
    Plane& p = world.planes[index];
    if (!p.alive)
        return;
    bool player = index == PLAYER_PLANE;
    if (player && (p.invulnerable > 0.0f || world.titleMode))
        return;
    p.health -= amount;
    p.hitFlash = 1.0f;
    if (p.health < p.maxHealth * 0.25f)
        p.fireFlare = 0.2f;
    p.lastHit = 0.0f;
    Sparks(at, Normalize(at - p.flight.position), 3);
    if (player) {
        float before = p.health + amount;
        HudDamage(amount);
        CameraShake(0.25f);
        Rumble(0.8f, 0.0f, 120);
        audio::Play(audio::Sfx::CRUNCH, 1.0f, 0.0f, world.rng.Range(0.94f, 1.06f));
        if (before >= p.maxHealth * 0.25f && p.health < p.maxHealth * 0.25f)
            audio::Play(audio::Sfx::GLASS_CRACK, 1.0f);
        if (amount > 6.0f || world.rng.Uniform() < 0.08f)
            audio::Play(audio::Sfx::MONKEY_HURT, 0.7f);
    } else if (byPlayer) {
        HudHitMarker();
        audio::Play3D(audio::Sfx::HIT, at, 0.7f, world.rng.Range(1.1f, 1.3f));
        AddScore(10, nullptr);
    }
    if (p.health <= 0.0f) {
        p.health = 0.0f;
        ShootDown(index, byPlayer);
    }
}

// --- Update

// Smoke and fire from a damaged plane, spray over the water. A burning engine is a small flame at the cowling
// that moves with the plane (it flares up for a moment when hit); a shot-down plane trails black smoke with a
// fire core for its first 1.5 s.
static void PlaneTrails(Plane& p, float dt)
{
    Vec3 back = p.flight.position - QuatForward(p.flight.rotation) * 3.0f;
    float health = p.health / p.maxHealth;
    p.smokeTimer -= dt;
    p.fireFlare -= dt;
    bool burning = !p.alive || health < 0.25f;
    bool smoking = burning || health < 0.5f;
    if (smoking && p.smokeTimer <= 0.0f) {
        p.smokeTimer = 0.04f;
        // The player's burning engine coughs now and then.
        if (burning && p.alive && &p == &Player() && world.rng.Uniform() < 0.04f)
            audio::Play(audio::Sfx::SPUTTER, 1.0f);
        TrailPuff(back, p.flight.velocity * 0.1f, burning);
        if (burning && p.alive) {
            int livery = p.kind == PlaneKind::PLAYER ? 0 : (p.kind == PlaneKind::ACE ? 2 : 1);
            Vec3 cowling = p.flight.position + Rotate(p.flight.rotation, assets.propellerHub[livery] +
                                                                         Vec3(0.0f, 0.4f, 1.2f));
            float size = p.fireFlare > 0.0f ? 6.0f : world.rng.Range(2.5f, 4.0f);
            EngineFire(cowling, p.flight.velocity + Vec3(0.0f, 1.0f, 0.0f), size);
        } else if (!p.alive && p.dyingTime < 1.5f) {
            FirePuff(back, p.flight.velocity * 0.6f, 5.0f);
        }
    }
    // Pirates in a hard turn trail vapour from both wingtips in their colours: hazard yellow, aces purple.
    p.vapourTimer -= dt;
    if (p.alive && p.kind != PlaneKind::PLAYER && p.flight.turnLoad > ENEMY_VAPOUR_LOAD && p.vapourTimer <= 0.0f) {
        p.vapourTimer = 0.08f;
        uint32_t color = p.kind == PlaneKind::ACE ? 0xB0B070FF : 0xB0E8B923;
        for (int side = -1; side <= 1; side += 2)
            Vapour(p.flight.position + Rotate(p.flight.rotation, Vec3(side * 6.4f, 0.2f, 1.0f)), color);
    }
    float altitude = p.flight.position.y;
    bool overSea = GroundHeight(p.flight.position.x, p.flight.position.z) <= 0.01f;
    if (overSea && altitude < 15.0f && world.rng.Uniform() < (15.0f - altitude) / 15.0f)
        Spray(p.flight.position, p.flight.velocity);
}

// True when a point is inside the rock of the canyon arch. The arch (content/props.cpp) is a tube along a half
// ellipse across the canyon: legs at -20 m, its centre line about 230 m out to each side and 116 m up at the top,
// 20 m thick at the top and growing toward the legs; flying under it (below about 100 m) is the point.
static bool InsideArch(const Vec3& pos)
{
    constexpr float LEG_Y = -20.0f;
    constexpr float RISE = 136.0f;
    constexpr float HALF_SPAN = 230.0f;
    constexpr float THICKNESS = 20.0f;
    const Vec3& arch = assets.canyonArch;
    float dx = pos.x - arch.x;
    if (fabsf(dx) > THICKNESS * 1.6f)
        return false;
    float dy = pos.y - LEG_Y;
    float dz = pos.z - arch.z;
    float e = sqrtf((dz / HALF_SPAN) * (dz / HALF_SPAN) + (dy / RISE) * (dy / RISE));
    if (e < 1e-3f || dy < -THICKNESS)
        return false;
    // Distance from the centre line, measured along the ray from the ellipse's centre.
    float radius = sqrtf(dy * dy + dz * dz);
    float distance = fabsf(1.0f - 1.0f / e) * radius;
    float cosA = Clamp(fabsf(dz) / fmaxf(radius, 1.0f), 0.0f, 1.0f);
    float thickness = THICKNESS * (1.0f + 1.2f * cosA);
    return distance * distance + dx * dx < thickness * thickness;
}

// The ground, the sea, the canyon arch and the zeppelin hull destroy a plane; the zeppelin's own escorts, which
// drop from her belly, never hit her.
static bool Crashed(const Plane& p)
{
    const Vec3& pos = p.flight.position;
    if (pos.y < GroundHeight(pos.x, pos.z) + CRASH_CLEARANCE)
        return true;
    if (InsideArch(pos))
        return true;
    return world.zeppelin.state != ZeppelinState::ABSENT && !p.escort && InsideHull(pos, 1.05f);
}

// The final impact: an explosion on land, a splash at sea.
static void Impact(int index)
{
    Plane& p = world.planes[index];
    Vec3 pos = p.flight.position;
    Explosion(pos, 16.0f, Vec3());
    if (GroundHeight(pos.x, pos.z) <= 0.01f && pos.y < 5.0f)
        Splash(pos, p.steerFall ? 22.0f : 12.0f);
    p.active = false;
    ReleaseEngine(index);
}

static void UpdateFalling(int index, float dt)
{
    Plane& p = world.planes[index];
    p.dyingTime += dt;
    FlightState& f = p.flight;
    f.velocity.y -= GRAVITY * dt;
    f.velocity *= 1.0f / (1.0f + 0.15f * dt);
    // A steered fall: the horizontal speed that reaches the target as it hits the water (at most FALL_STEER_SPEED).
    if (p.steerFall) {
        float h = fmaxf(f.position.y, 1.0f);
        float down = -f.velocity.y;
        float t = (-down + sqrtf(down * down + 2.0f * GRAVITY * h)) / GRAVITY;
        t = fmaxf(t, 0.5f);
        Vec3 want = (p.fallTarget - f.position) / t;
        want.y = 0.0f;
        if (Length(want) > FALL_STEER_SPEED)
            want = Normalize(want) * FALL_STEER_SPEED;
        float blend = 1.0f - expf(-3.0f * dt);
        f.velocity.x = Lerp(f.velocity.x, want.x, blend);
        f.velocity.z = Lerp(f.velocity.z, want.z, blend);
    }
    f.position += f.velocity * dt;
    // Nose follows the fall, spinning around it.
    Vec3 dir = Normalize(f.velocity);
    Quat look = QuatLook(dir, QuatUp(f.rotation));
    f.rotation = QuatNormalize(QuatSlerp(f.rotation, look, 1.0f - expf(-3.0f * dt)));
    f.rotation = QuatNormalize(f.rotation * QuatAxisAngle(Vec3(0, 0, -1), p.dyingSpin * dt));
    f.throttle = Approach(f.throttle, 0.1f, 1.0f, dt);
    PlaneTrails(p, dt);
    if (Crashed(p) || p.dyingTime > 8.0f)
        Impact(index);
}

static void UpdateFlying(int index, float dt)
{
    Plane& p = world.planes[index];
    bool player = index == PLAYER_PLANE;
    if (!player)
        UpdateAi(index, dt);
    int stuntsBefore = p.flight.stuntsDone;
    Stunt stunt = p.flight.stunt;
    UpdateFlight(p.flight, *p.params, p.controls, dt);
    if (player && p.flight.stuntsDone > stuntsBefore)
        OnStuntDone(stunt);
    if (player && p.flight.stunt != Stunt::NONE && stunt == Stunt::NONE)
        audio::Play(audio::Sfx::STUNT, 0.8f);
    if (p.flight.position.y > CEILING + 150.0f)
        p.flight.position.y = CEILING + 150.0f;
    UpdateGuns(index, dt);
    p.hitFlash = Clamp(p.hitFlash - dt / HIT_FLASH_TIME, 0.0f, 1.0f);
    p.invulnerable -= dt;
    p.lastHit += dt;
    p.rocketCooldown -= dt;
    // A badly damaged player's plane patches itself up slowly once out of the fight, back to 30 %.
    if (player && p.lastHit > REGEN_DELAY && p.health < p.maxHealth * REGEN_LIMIT)
        p.health = fminf(p.maxHealth * REGEN_LIMIT, p.health + p.maxHealth * REGEN_RATE * dt);
    PlaneTrails(p, dt);
    // Pirates never fly into the ground on their own (a kill must be the player's): a pirate about to touch the
    // ground is lifted clear and pointed up. The rare save is invisible next to a crash that ends a wave.
    if (!player && !InsideHull(p.flight.position, 1.05f)) {
        float floor = GroundHeight(p.flight.position.x, p.flight.position.z) + PIRATE_FLOOR;
        bool arch = InsideArch(p.flight.position);
        if (p.flight.position.y < floor || arch) {
            p.flight.position.y = fmaxf(p.flight.position.y, floor) + (arch ? 40.0f : 0.0f);
            Vec3 fwd = QuatForward(p.flight.rotation);
            p.flight.rotation = QuatLook(Normalize(Vec3(fwd.x, 0.0f, fwd.z) + Vec3(0.0f, 0.6f, 0.0f)), WORLD_UP);
            p.flight.velocity = QuatForward(p.flight.rotation) * p.flight.speed;
        }
    }
    if (Crashed(p) && !(player && world.cinematic)) {
        bool hull = InsideHull(p.flight.position, 1.05f);
        LogEvent("%s %d crashed into the %s", player ? "player" : "plane", index, hull ? "zeppelin" : "ground");
        p.health = 0.0f;
        p.alive = false;
        if (player)
            OnPlayerDown();
        else
            OnPlaneDestroyed(index, false);
        Impact(index);
    }
}

// Mid-air collisions between the player and a pirate hurt both.
static void Collisions()
{
    Plane& player = Player();
    if (!player.alive || player.invulnerable > 0.0f)
        return;
    for (int i = 1; i < MAX_PLANES; i++) {
        Plane& p = world.planes[i];
        if (!p.alive)
            continue;
        if (LengthSq(p.flight.position - player.flight.position) < COLLIDE_DISTANCE * COLLIDE_DISTANCE) {
            Vec3 at = (p.flight.position + player.flight.position) * 0.5f;
            DamagePlane(i, 100.0f, true, at);
            DamagePlane(PLAYER_PLANE, 35.0f, false, at);
        }
    }
}

void UpdatePlanes(float dt)
{
    for (int i = 0; i < MAX_PLANES; i++) {
        Plane& p = world.planes[i];
        p.prevPosition = p.flight.position;
        p.prevRotation = p.flight.rotation;
        if (!p.active)
            continue;
        if (p.alive)
            UpdateFlying(i, dt);
        else
            UpdateFalling(i, dt);
        p.propAngle += (20.0f + p.flight.throttle * 40.0f) * dt;
        if (p.propAngle > 2.0f * PI)
            p.propAngle -= 2.0f * PI;
    }
    Collisions();
    for (int i = 0; i < MAX_PLANES; i++) {
        const Plane& p = world.planes[i];
        if (engines[i] < 0)
            continue;
        float boost = p.flight.boosting ? 1.0f : 0.0f;
        audio::SetEngine(engines[i], p.flight.position, p.flight.velocity, p.flight.throttle, 1.0f, boost);
    }
}

// --- Targets

bool TargetValid(const TargetRef& target)
{
    const Zeppelin& z = world.zeppelin;
    bool zeppelinUp = z.state == ZeppelinState::ARRIVING || z.state == ZeppelinState::CIRCLING;
    switch (target.kind) {
    case TargetKind::PLANE:
        return target.index > 0 && target.index < MAX_PLANES && world.planes[target.index].alive;
    case TargetKind::ENGINE:
        return zeppelinUp && target.index < z.engineCount && z.engineHealth[target.index] > 0.0f;
    case TargetKind::TURRET:
        return zeppelinUp && target.index < z.turretCount && z.turretHealth[target.index] > 0.0f;
    default:
        return false;
    }
}

Vec3 TargetPosition(const TargetRef& target)
{
    switch (target.kind) {
    case TargetKind::PLANE:
        return world.planes[target.index].flight.position;
    case TargetKind::ENGINE:
        return EngineWorldPosition(target.index);
    case TargetKind::TURRET:
        return TurretWorldPosition(target.index);
    default:
        return Vec3();
    }
}

Vec3 TargetVelocity(const TargetRef& target)
{
    if (target.kind == TargetKind::PLANE)
        return world.planes[target.index].flight.velocity;
    if (target.kind == TargetKind::ENGINE || target.kind == TargetKind::TURRET)
        return world.zeppelin.velocity;
    return Vec3();
}

float TargetHealth(const TargetRef& target)
{
    switch (target.kind) {
    case TargetKind::PLANE:
        return world.planes[target.index].health / world.planes[target.index].maxHealth;
    case TargetKind::ENGINE:
        return world.zeppelin.engineHealth[target.index] / ENGINE_HEALTH;
    case TargetKind::TURRET:
        return world.zeppelin.turretHealth[target.index] / TURRET_HEALTH;
    default:
        return 0.0f;
    }
}

float TargetRadius(const TargetRef& target)
{
    return target.kind == TargetKind::PLANE ? 6.0f : 7.0f;
}

// Ranks a candidate: near and in front of the player first.
static float TargetRank(const TargetRef& target)
{
    const Plane& player = Player();
    Vec3 to = TargetPosition(target) - player.flight.position;
    float distance = Length(to);
    float facing = Dot(QuatForward(player.flight.rotation), to / fmaxf(distance, 1.0f));
    return distance * (2.2f - facing * 1.2f);
}

TargetRef NextTarget(const TargetRef& current)
{
    constexpr int MAX_CANDIDATES = MAX_PLANES + MAX_ENGINES;
    TargetRef list[MAX_CANDIDATES];
    float rank[MAX_CANDIDATES];
    int count = 0;
    for (int i = 1; i < MAX_PLANES; i++) {
        TargetRef t;
        t.kind = TargetKind::PLANE;
        t.index = i;
        if (TargetValid(t))
            list[count++] = t;
    }
    for (int e = 0; e < world.zeppelin.engineCount; e++) {
        TargetRef t;
        t.kind = TargetKind::ENGINE;
        t.index = e;
        if (TargetValid(t))
            list[count++] = t;
    }
    if (count == 0)
        return TargetRef();
    for (int i = 0; i < count; i++)
        rank[i] = TargetRank(list[i]);
    // Insertion sort by rank (a handful of candidates).
    for (int i = 1; i < count; i++) {
        for (int j = i; j > 0 && rank[j] < rank[j - 1]; j--) {
            float r = rank[j];
            rank[j] = rank[j - 1];
            rank[j - 1] = r;
            TargetRef t = list[j];
            list[j] = list[j - 1];
            list[j - 1] = t;
        }
    }
    for (int i = 0; i < count; i++) {
        if (list[i].kind == current.kind && list[i].index == current.index)
            return list[(i + 1) % count];
    }
    return list[0];
}

} // namespace game
