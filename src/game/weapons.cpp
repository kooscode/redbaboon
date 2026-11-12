// Weapons (docs/design.md §8, creative brief §5): banana machine guns (simulated bullets drawn as tracer streaks,
// alternating wing guns, heat and overheat lock-out), homing banana rockets, zeppelin turret tracers and flak.
// Bullets are swept spheres tested against planes, the zeppelin's engines, turrets and hull.
#include "game/world.h"

namespace game {

constexpr int MAX_BULLETS = 400;
constexpr int MAX_ROCKETS = 24;
constexpr int MAX_FLAK = 24;

constexpr float PLAYER_FIRE_RATE = 14.0f;    // rounds per second, alternating wings
constexpr float ENEMY_FIRE_RATE = 9.0f;
constexpr float GUN_SPREAD = 0.02f;          // random spread (radians, about 1 degree)
constexpr float HEAT_PER_SECOND = 0.25f;     // 4 s of fire to overheat
constexpr float COOL_PER_SECOND = 0.4f;
constexpr float OVERHEAT_LOCK = 2.0f;
constexpr float PLAYER_BULLET_DAMAGE = 6.0f;
constexpr float ENEMY_BULLET_DAMAGE = 4.0f;
constexpr float TURRET_BULLET_DAMAGE = 1.8f;
constexpr float PLAYER_RADIUS = 4.5f;        // hit sphere of the player's plane
constexpr float NEAR_MISS = 30.0f;           // enemy rounds passing this close count as being shot at
constexpr float ENEMY_RADIUS = 8.5f;         // generous: an arcade gun should hit what the reticle covers
constexpr float ENGINE_RADIUS = 7.0f;
constexpr float TURRET_RADIUS = 4.5f;
constexpr float TRACER_LENGTH = 10.0f;
constexpr float ASSIST_CONE = 4.0f * DEG;    // aim assist only for targets this close to the guns' line
constexpr float ASSIST_MAX = 2.0f * DEG;     // and bends the stream at most this much
constexpr float BULLET_RANGE = 450.0f;
constexpr float TURRET_BULLET_LIFE = 2.2f;   // turret tracers reach the turrets' 900 m range

constexpr float ROCKET_SPEED = 230.0f;
constexpr float ROCKET_LIFE = 4.0f;
constexpr float ROCKET_TURN = 60.0f * DEG;   // homing rate
constexpr float ROCKET_CONE = 0.7f;          // cos of the homing cone half-angle (~45 degrees)
constexpr float ROCKET_DAMAGE = 45.0f;
constexpr float ROCKET_BLAST = 16.0f;

constexpr float FLAK_RADIUS = 30.0f;
constexpr float FLAK_DAMAGE = 9.0f;

struct Bullet {
    Vec3 position;
    Vec3 velocity;
    float life;
    float age;
    float damage;
    bool fromPlayer;
    bool tracer;    // every second round is drawn as a tracer streak
};

struct Rocket {
    Vec3 position;
    Vec3 velocity;
    float life;
    float smokeTimer;
    TargetRef target;
    bool active;
};

struct Shell {
    Vec3 position;
    Vec3 velocity;
    float fuse;
    bool active;
};

// Live bullets packed at the front.
static Bullet bullets[MAX_BULLETS];
static int bulletCount = 0;
static Rocket rockets[MAX_ROCKETS];
static Shell shells[MAX_FLAK];

// Alternates so that every second round fired is a tracer.
static bool tracerToggle = false;

// Draw buffer for tracers (two streaks per bullet: edge and core).
static render::Streak tracerStreaks[MAX_BULLETS * 2];

// --- Helpers

// Earliest parameter t in [0, 1] where the segment a->b enters the sphere, or -1.
static float SegmentSphere(const Vec3& a, const Vec3& b, const Vec3& center, float radius)
{
    Vec3 d = b - a;
    Vec3 m = a - center;
    float c = Dot(m, m) - radius * radius;
    if (c <= 0.0f)
        return 0.0f;
    float dd = Dot(d, d);
    if (dd < 1e-6f)
        return -1.0f;
    float bq = Dot(m, d);
    if (bq > 0.0f)
        return -1.0f;
    float disc = bq * bq - dd * c;
    if (disc < 0.0f)
        return -1.0f;
    float t = (-bq - sqrtf(disc)) / dd;
    return t <= 1.0f ? t : -1.0f;
}

// Segment against the zeppelin hull ellipsoid.
static float SegmentHull(const Vec3& a, const Vec3& b)
{
    const Zeppelin& z = world.zeppelin;
    if (z.state == ZeppelinState::ABSENT || z.state == ZeppelinState::SUNK)
        return -1.0f;
    Quat inv = QuatConjugate(z.rotation);
    Vec3 la = Rotate(inv, a - z.position);
    Vec3 lb = Rotate(inv, b - z.position);
    Vec3 scale(1.0f / HULL_RADII.x, 1.0f / HULL_RADII.y, 1.0f / HULL_RADII.z);
    return SegmentSphere(Mul(la, scale), Mul(lb, scale), Vec3(), 1.0f);
}

static Vec3 RandomSpread(float amount)
{
    return Vec3(world.rng.Range(-amount, amount), world.rng.Range(-amount, amount),
                world.rng.Range(-amount, amount));
}

static void AddBullet(const Vec3& position, const Vec3& velocity, float damage, bool fromPlayer, float life)
{
    if (bulletCount >= MAX_BULLETS)
        return;
    tracerToggle = !tracerToggle;
    Bullet& b = bullets[bulletCount++];
    b.tracer = tracerToggle;
    b.position = position;
    b.velocity = velocity;
    b.life = life;
    b.age = 0.0f;
    b.damage = damage;
    b.fromPlayer = fromPlayer;
}

void ClearProjectiles()
{
    bulletCount = 0;
    for (Rocket& r : rockets)
        r.active = false;
    for (Shell& s : shells)
        s.active = false;
}

// --- Guns

// Light aim assist for the player: a pirate whose lead point is within ASSIST_CONE of the guns pulls the stream
// toward that point by up to ASSIST_MAX (the target still has to be in front of the reticle).
static Vec3 AimAssist(const Plane& shooter, const Vec3& dir)
{
    Vec3 best = dir;
    float bestCos = cosf(ASSIST_CONE);
    for (int i = 1; i < MAX_PLANES; i++) {
        const Plane& p = world.planes[i];
        if (!p.alive)
            continue;
        Vec3 lead = LeadPoint(shooter.flight.position, shooter.flight.velocity, p.flight.position,
                              p.flight.velocity, BULLET_SPEED);
        Vec3 to = lead - shooter.flight.position;
        float distance = Length(to);
        if (distance > BULLET_RANGE)
            continue;
        Vec3 want = to / distance;
        float c = Dot(dir, want);
        if (c > bestCos) {
            bestCos = c;
            best = want;
        }
    }
    float angle = acosf(Clamp(Dot(dir, best), -1.0f, 1.0f));
    if (angle < 1e-4f)
        return dir;
    float pull = fminf(1.0f, ASSIST_MAX / angle);
    return Normalize(Lerp(dir, best, pull));
}

static void FireOneRound(int index)
{
    Plane& plane = world.planes[index];
    bool player = index == PLAYER_PLANE;
    const Quat& rot = plane.flight.rotation;
    int livery = plane.kind == PlaneKind::PLAYER ? 0 : (plane.kind == PlaneKind::ACE ? 2 : 1);
    Vec3 muzzle = plane.flight.position + Rotate(rot, assets.gunMuzzle[livery][plane.gunSide]);
    plane.gunSide ^= 1;
    Vec3 dir = QuatForward(rot);
    if (player)
        dir = AimAssist(plane, dir);
    dir = Normalize(dir + RandomSpread(GUN_SPREAD));
    Vec3 velocity = plane.flight.velocity + dir * BULLET_SPEED;
    AddBullet(muzzle, velocity, player ? PLAYER_BULLET_DAMAGE : ENEMY_BULLET_DAMAGE, player, BULLET_LIFE);
    MuzzleFlash(muzzle + dir * 1.0f, plane.flight.velocity);
    if (player) {
        world.stats.shotsFired++;
        audio::Play(audio::Sfx::GUN, 0.55f, plane.gunSide ? 0.45f : -0.45f, world.rng.Range(0.96f, 1.04f));
        if (world.stats.shotsFired % 3 == 0)
            Rumble(0.0f, 0.2f, 50);
        CameraShake(0.02f);
    } else {
        audio::Play3D(audio::Sfx::GUN, muzzle, 0.6f, world.rng.Range(0.8f, 0.9f));
    }
}

void UpdateGuns(int index, float dt)
{
    Plane& plane = world.planes[index];
    // The cooldown only runs down to zero: idle time must not bank rounds that would all leave in one step.
    plane.gunCooldown = fmaxf(plane.gunCooldown - dt, 0.0f);
    if (plane.overheat > 0.0f) {
        plane.overheat -= dt;
        plane.gunHeat = Clamp(plane.gunHeat - dt / OVERHEAT_LOCK, 0.0f, 1.0f);
        return;
    }
    bool firing = plane.trigger && plane.alive && plane.flight.stunt == Stunt::NONE;
    if (!firing) {
        plane.gunHeat = Clamp(plane.gunHeat - COOL_PER_SECOND * dt, 0.0f, 1.0f);
        return;
    }
    float rate = index == PLAYER_PLANE ? PLAYER_FIRE_RATE : ENEMY_FIRE_RATE;
    plane.gunHeat += HEAT_PER_SECOND * dt;
    if (plane.gunHeat >= 1.0f) {
        plane.gunHeat = 1.0f;
        plane.overheat = OVERHEAT_LOCK;
        if (index == PLAYER_PLANE) {
            audio::Play(audio::Sfx::OVERHEAT, 1.0f);
            HudPopup("OVERHEAT", 0xFFFF3B2F);
        }
        return;
    }
    while (plane.gunCooldown <= 0.0f) {
        plane.gunCooldown += 1.0f / rate;
        FireOneRound(index);
    }
}

// --- Rockets

bool FireRocket(int index, const TargetRef& target)
{
    Plane& plane = world.planes[index];
    if (plane.rockets <= 0 || plane.rocketCooldown > 0.0f || !plane.alive)
        return false;
    for (Rocket& r : rockets) {
        if (r.active)
            continue;
        const Quat& rot = plane.flight.rotation;
        r.active = true;
        r.position = plane.flight.position + Rotate(rot, Vec3(0.0f, -1.2f, -2.0f));
        r.velocity = plane.flight.velocity + QuatForward(rot) * 40.0f;
        r.life = ROCKET_LIFE;
        r.smokeTimer = 0.0f;
        r.target = target;
        plane.rockets--;
        plane.rocketCooldown = 0.5f;
        audio::Play3D(audio::Sfx::ROCKET, r.position);
        if (index == PLAYER_PLANE)
            Rumble(0.3f, 0.3f, 150);
        return true;
    }
    return false;
}

// Blast damage to everything the player can hurt within the radius.
static void RocketBlast(const Vec3& at)
{
    Explosion(at, 9.0f, Vec3());
    bool hit = false;
    for (int i = 1; i < MAX_PLANES; i++) {
        Plane& p = world.planes[i];
        if (!p.alive)
            continue;
        float d = Length(p.flight.position - at);
        if (d < ROCKET_BLAST + ENEMY_RADIUS) {
            DamagePlane(i, ROCKET_DAMAGE * Clamp(1.3f - d / ROCKET_BLAST, 0.3f, 1.0f), true, at);
            hit = true;
        }
    }
    const Zeppelin& z = world.zeppelin;
    if (z.state == ZeppelinState::CIRCLING || z.state == ZeppelinState::ARRIVING) {
        for (int e = 0; e < z.engineCount; e++) {
            if (z.engineHealth[e] > 0.0f && Length(EngineWorldPosition(e) - at) < ROCKET_BLAST + ENGINE_RADIUS) {
                DamageEngine(e, ROCKET_DAMAGE, at);
                hit = true;
            }
        }
        for (int t = 0; t < z.turretCount; t++) {
            if (z.turretHealth[t] > 0.0f && Length(TurretWorldPosition(t) - at) < ROCKET_BLAST + TURRET_RADIUS) {
                DamageTurret(t, ROCKET_DAMAGE, at);
                hit = true;
            }
        }
    }
    if (hit)
        HudHitMarker();
}

static void UpdateRocket(Rocket& r, float dt)
{
    r.life -= dt;
    Vec3 dir = Normalize(r.velocity);
    float speed = Length(r.velocity);
    speed = fminf(speed + 300.0f * dt, ROCKET_SPEED);
    if (TargetValid(r.target)) {
        Vec3 to = Normalize(TargetPosition(r.target) - r.position);
        float c = Dot(dir, to);
        if (c > ROCKET_CONE) {
            float angle = acosf(Clamp(c, -1.0f, 1.0f));
            float step = fminf(angle, ROCKET_TURN * dt);
            if (angle > 1e-4f)
                dir = Normalize(Lerp(dir, to, step / angle));
        }
    }
    Vec3 from = r.position;
    r.velocity = dir * speed;
    r.position += r.velocity * dt;

    Glow(r.position - dir * 1.0f, 3.0f, 0xFFFFC060);
    r.smokeTimer -= dt;
    if (r.smokeTimer <= 0.0f) {
        r.smokeTimer = 0.03f;
        SmokePuff(r.position - dir * 1.5f, Vec3(), 1.0f, 0.1f);
    }

    bool boom = r.life <= 0.0f || r.position.y < GroundHeight(r.position.x, r.position.z);
    for (int i = 1; i < MAX_PLANES && !boom; i++) {
        const Plane& p = world.planes[i];
        if (p.alive && SegmentSphere(from, r.position, p.flight.position, ENEMY_RADIUS + 3.0f) >= 0.0f)
            boom = true;
    }
    const Zeppelin& z = world.zeppelin;
    for (int e = 0; e < z.engineCount && !boom; e++) {
        if (z.engineHealth[e] > 0.0f && SegmentSphere(from, r.position, EngineWorldPosition(e), ENGINE_RADIUS) >= 0)
            boom = true;
    }
    if (!boom && SegmentHull(from, r.position) >= 0.0f)
        boom = true;
    if (boom) {
        r.active = false;
        RocketBlast(r.position);
    }
}

// --- Turrets and flak

void FireTurretBullet(const Vec3& from, const Vec3& velocity)
{
    AddBullet(from, velocity, TURRET_BULLET_DAMAGE, false, TURRET_BULLET_LIFE);
    MuzzleFlash(from, Vec3());
}

void FireFlak(const Vec3& from, const Vec3& velocity, float fuse)
{
    for (Shell& s : shells) {
        if (s.active)
            continue;
        s.active = true;
        s.position = from;
        s.velocity = velocity;
        s.fuse = fuse;
        return;
    }
}

static void UpdateShell(Shell& s, float dt)
{
    s.position += s.velocity * dt;
    s.fuse -= dt;
    if (s.fuse > 0.0f)
        return;
    s.active = false;
    FlakBurst(s.position);
    Plane& player = Player();
    if (!player.alive)
        return;
    float d = Length(player.flight.position - s.position);
    if (d < FLAK_RADIUS)
        DamagePlane(PLAYER_PLANE, FLAK_DAMAGE * (1.0f - d / FLAK_RADIUS), false, s.position);
    if (d < FLAK_RADIUS * 3.0f)
        CameraShake(0.3f * (1.0f - d / (FLAK_RADIUS * 3.0f)));
}

// --- Bullets

// Tests one player bullet's step against enemies and the zeppelin. Returns true when it hit something.
static bool PlayerBulletHits(const Bullet& b, const Vec3& from, const Vec3& to)
{
    for (int i = 1; i < MAX_PLANES; i++) {
        const Plane& p = world.planes[i];
        if (!p.alive || StuntDodging(p.flight))
            continue;
        if (SegmentSphere(from, to, p.flight.position, ENEMY_RADIUS) >= 0.0f) {
            // Hit: a puff of dark smoke and chips where the round struck, as well as the sparks and the tick.
            Vec3 at = p.flight.position + Normalize(to - p.flight.position) * 3.0f;
            SmokePuff(at, p.flight.velocity * 0.8f, 1.5f, 0.75f);
            DamagePlane(i, b.damage, true, at);
            return true;
        }
    }
    const Zeppelin& z = world.zeppelin;
    if (z.state == ZeppelinState::ABSENT || z.state == ZeppelinState::SUNK)
        return false;
    for (int e = 0; e < z.engineCount; e++) {
        if (z.engineHealth[e] > 0.0f && SegmentSphere(from, to, EngineWorldPosition(e), ENGINE_RADIUS) >= 0.0f) {
            DamageEngine(e, b.damage, to);
            return true;
        }
    }
    for (int t = 0; t < z.turretCount; t++) {
        if (z.turretHealth[t] > 0.0f && SegmentSphere(from, to, TurretWorldPosition(t), TURRET_RADIUS) >= 0.0f) {
            DamageTurret(t, b.damage, to);
            return true;
        }
    }
    float t = SegmentHull(from, to);
    if (t >= 0.0f) {
        Vec3 at = Lerp(from, to, t);
        Sparks(at, Normalize(at - z.position), 2);
        return true;
    }
    return false;
}

// Moves one bullet a step. Returns false when it is used up.
static bool UpdateBullet(Bullet& b, float dt)
{
    b.life -= dt;
    b.age += dt;
    if (b.life <= 0.0f)
        return false;
    Vec3 from = b.position;
    b.velocity.y -= GRAVITY * 0.3f * dt;
    b.position += b.velocity * dt;
    if (b.fromPlayer) {
        if (PlayerBulletHits(b, from, b.position)) {
            world.stats.shotsHit++;
            return false;
        }
    } else {
        Plane& player = Player();
        if (player.alive && SegmentSphere(from, b.position, player.flight.position, NEAR_MISS) >= 0.0f)
            world.shotAt = 0.0f;
        bool target = player.alive && player.invulnerable <= 0.0f && !StuntDodging(player.flight);
        if (target && SegmentSphere(from, b.position, player.flight.position, PLAYER_RADIUS) >= 0.0f) {
            DamagePlane(PLAYER_PLANE, b.damage, false, b.position);
            return false;
        }
        if (SegmentHull(from, b.position) >= 0.0f)
            return false;
    }
    float ground = GroundHeight(b.position.x, b.position.z);
    if (b.position.y < ground) {
        if (ground <= 0.01f)
            SmokePuff(Vec3(b.position.x, 0.5f, b.position.z), Vec3(0, 6, 0), 1.0f, 0.0f);
        else
            SmokePuff(b.position, Vec3(0, 2, 0), 1.5f, 0.4f);
        return false;
    }
    return true;
}

void UpdateProjectiles(float dt)
{
    int i = 0;
    while (i < bulletCount) {
        if (UpdateBullet(bullets[i], dt))
            i++;
        else
            bullets[i] = bullets[--bulletCount];
    }
    for (Rocket& r : rockets) {
        if (r.active)
            UpdateRocket(r, dt);
    }
    for (Shell& s : shells) {
        if (s.active)
            UpdateShell(s, dt);
    }
}

// --- Drawing

void DrawProjectiles()
{
    int count = 0;
    for (int i = 0; i < bulletCount; i++) {
        const Bullet& b = bullets[i];
        if (!b.tracer)
            continue;
        Vec3 dir = Normalize(b.velocity);
        // A fresh bullet's tracer grows out of the muzzle instead of reaching back into the plane.
        float length = fminf(TRACER_LENGTH, b.age * BULLET_SPEED);
        Vec3 tail = b.position - dir * length;
        render::Streak& edge = tracerStreaks[count++];
        edge.from = tail;
        edge.to = b.position;
        edge.width = 0.35f;
        edge.color = b.fromPlayer ? 0xFFFFD23F : 0xFFFF5A3C;
        render::Streak& core = tracerStreaks[count++];
        core.from = tail + dir * (length * 0.3f);
        core.to = b.position;
        core.width = 0.15f;
        core.color = b.fromPlayer ? 0xFFFFFBE0 : 0xFFFFE0D0;
    }
    if (count > 0)
        render::DrawStreaks(assets.streak, tracerStreaks, count, render::Blend::ADDITIVE);

    render::Material material;
    material.texture = assets.propsTexture;
    material.specular = 0.4f;
    for (const Rocket& r : rockets) {
        if (!r.active)
            continue;
        Vec3 dir = Normalize(r.velocity);
        Quat rot = QuatLook(dir, fabsf(dir.y) > 0.95f ? Vec3(0, 0, 1) : WORLD_UP);
        render::DrawMesh(assets.rocket, MatTRS(r.position, rot, Vec3(1, 1, 1)), material);
    }
}

} // namespace game
