// Pirate pilots (docs/design.md §8): raid the harbour, attack the player with lead pursuit and a skill-based aim
// error, break away or stunt when shot at, extend after a pass, and keep clear of the ground, the zeppelin and the
// edge of the flight area. Only a few pirates shoot at once, so the player is never hosed from every side.
#include "game/world.h"

namespace game {

constexpr float FIRE_RANGE = 350.0f;     // pirates open fire inside this (the guns reach about 450 m)
constexpr float AVOID_LOOKAHEAD = 2.5f;   // seconds of flight checked for terrain
constexpr float MIN_CLEARANCE = 70.0f;
constexpr float CHASE_CLEARANCE = 25.0f;  // attacking a player within CHASE_RANGE: low enough to follow him
constexpr float CHASE_RANGE = 600.0f;     // into the canyon
constexpr float RAID_RADIUS = 450.0f;
constexpr float RAID_ALTITUDE = 160.0f;
constexpr float NOTICE_RANGE = 800.0f;    // raiders turn on the player within this range
constexpr float STRAFE_RADIUS = 250.0f;   // a strafer's circle round the wharf
constexpr float STRAFE_ALTITUDE = 80.0f;
constexpr int MAX_SHOOTERS = 3;           // pirates allowed to fire at the player at the same time
constexpr float EXTEND_RANGE = 450.0f;    // an extending pirate turns back in at this distance
constexpr float TAIL_RANGE = 350.0f;      // the player behind within this is a threat
constexpr float EVADE_ONCE_RANGE = 300.0f; // wave 1's raiders break once when the player is behind within this

// --- Steering

Vec3 LeadPoint(const Vec3& shooter, const Vec3& shooterVelocity, const Vec3& target, const Vec3& targetVelocity,
               float bulletSpeed)
{
    // Bullets carry the shooter's velocity, so lead by the relative velocity; two passes refine the flight time.
    Vec3 relative = targetVelocity - shooterVelocity;
    Vec3 aim = target;
    for (int i = 0; i < 2; i++) {
        float t = Length(aim - shooter) / bulletSpeed;
        aim = target + relative * t;
    }
    return aim;
}

void SteerToward(const Plane& plane, const Vec3& point, float aggression, FlightControls& controls)
{
    const Quat& rot = plane.flight.rotation;
    Vec3 dir = Normalize(point - plane.flight.position);
    Vec3 local = Rotate(QuatConjugate(rot), dir);
    Vec3 fwd = QuatForward(rot);
    float headingNow = atan2f(fwd.x, -fwd.z);
    float headingWant = atan2f(dir.x, -dir.z);
    float headingError = WrapPi(headingWant - headingNow);
    // Bank toward the heading; pull when the point is above the canopy (which also tightens a banked turn).
    controls.turn = Clamp(headingError * 2.5f, -1.0f, 1.0f) * aggression;
    controls.pitch = Clamp(local.y * 4.0f, -1.0f, 1.0f) * aggression;
    if (local.z > 0.0f && fabsf(local.y) < 0.3f)
        controls.pitch = 0.7f * aggression;
    // Rudder for the last few degrees of aim.
    controls.rudder = local.z < -0.95f ? Clamp(local.x * 6.0f, -1.0f, 1.0f) : 0.0f;
}

// --- Modes

static void SetMode(AiBrain& brain, AiMode mode)
{
    brain.mode = mode;
    brain.modeTime = 0.0f;
}

// Overrides for safety: terrain, ceiling, the zeppelin and the edge of the map. Returns true when one applies. A
// pirate attacking a player close by keeps less clearance, so it follows him down into the canyon.
static bool Avoid(Plane& p)
{
    const FlightState& f = p.flight;
    const Plane& player = Player();
    bool chasing = p.ai.mode == AiMode::ATTACK && player.alive &&
                   LengthSq(player.flight.position - f.position) < CHASE_RANGE * CHASE_RANGE;
    float clearance = chasing ? CHASE_CLEARANCE : MIN_CLEARANCE;
    Vec3 ahead = f.position + f.velocity * AVOID_LOOKAHEAD;
    float groundAhead = GroundHeight(ahead.x, ahead.z);
    float groundHere = GroundHeight(f.position.x, f.position.z);
    Vec3 fwd = QuatForward(f.rotation);
    Vec3 flat = Normalize(Vec3(fwd.x, 0.0f, fwd.z));
    if (ahead.y < groundAhead + clearance || f.position.y < groundHere + clearance * 0.6f) {
        SteerToward(p, f.position + flat * 150.0f + Vec3(0.0f, 160.0f, 0.0f), 1.0f, p.controls);
        p.controls.turn = 0.0f;
        p.controls.throttle = 0.5f;
        return true;
    }
    const Zeppelin& z = world.zeppelin;
    bool zeppelinUp = z.state != ZeppelinState::ABSENT && z.state != ZeppelinState::SUNK;
    if (zeppelinUp && InsideHull(f.position + f.velocity * 1.5f, 1.8f)) {
        Vec3 away = Normalize(f.position - z.position);
        SteerToward(p, f.position + away * 300.0f + Vec3(0.0f, 120.0f, 0.0f), 1.0f, p.controls);
        return true;
    }
    if (f.position.y > CEILING - 300.0f) {
        SteerToward(p, f.position + flat * 300.0f - Vec3(0.0f, 150.0f, 0.0f), 0.8f, p.controls);
        return true;
    }
    if (fabsf(f.position.x) > WARN_HALF_SIZE || fabsf(f.position.z) > WARN_HALF_SIZE) {
        SteerToward(p, Vec3(0.0f, fmaxf(f.position.y, 300.0f), 0.0f), 0.9f, p.controls);
        return true;
    }
    return false;
}

static void Raid(Plane& p, float dt)
{
    AiBrain& brain = p.ai;
    if (brain.passive) {
        SteerToward(p, brain.waypoint, 0.5f, p.controls);
        return;
    }
    float radius = brain.strafer ? STRAFE_RADIUS : RAID_RADIUS;
    brain.raidAngle += dt * p.flight.speed / radius;
    Vec3 center = brain.strafer ? assets.wharf : assets.harbour;
    Vec3 point(center.x + cosf(brain.raidAngle) * radius, brain.strafer ? STRAFE_ALTITUDE : RAID_ALTITUDE,
               center.z + sinf(brain.raidAngle) * radius);
    SteerToward(p, point, 0.7f, p.controls);
    // A dive at the harbour now and then, guns blazing at the ground for show.
    p.trigger = fmodf(brain.raidAngle, 2.0f * PI) < 0.4f;
}

// True when no terrain lies between two points (sampled every ~25 m).
static bool ClearShot(const Vec3& from, const Vec3& to)
{
    int steps = 1 + static_cast<int>(Length(to - from) / 25.0f);
    for (int i = 1; i < steps; i++) {
        Vec3 at = Lerp(from, to, static_cast<float>(i) / steps);
        if (at.y < GroundHeight(at.x, at.z))
            return false;
    }
    return true;
}

// True when this pirate may open fire (limits how many shoot at the player at once).
static bool MayShoot(int index)
{
    int shooting = 0;
    for (int i = 1; i < MAX_PLANES; i++) {
        if (i != index && world.planes[i].alive && world.planes[i].ai.burstTimer > 0.0f)
            shooting++;
    }
    return shooting < MAX_SHOOTERS;
}

static void Attack(int index, float dt)
{
    Plane& p = world.planes[index];
    AiBrain& brain = p.ai;
    const Plane& player = Player();
    Vec3 to = player.flight.position - p.flight.position;
    float distance = Length(to);

    // Aim error: a random offset perpendicular to the line of sight, re-rolled every pass.
    brain.aimTimer -= dt;
    if (brain.aimTimer <= 0.0f) {
        brain.aimTimer = world.rng.Range(1.0f, 1.8f);
        Vec3 r(world.rng.Range(-1, 1), world.rng.Range(-1, 1), world.rng.Range(-1, 1));
        brain.aimError = Normalize(r - to * (Dot(r, to) / fmaxf(distance * distance, 1.0f)));
    }
    float errorAngle = Lerp(1.6f, 0.5f, brain.skill) * DEG;
    Vec3 aim = LeadPoint(p.flight.position, p.flight.velocity, player.flight.position, player.flight.velocity,
                         BULLET_SPEED);
    aim += brain.aimError * (distance * errorAngle);
    float aggression = Lerp(0.7f, 1.0f, brain.skill);
    SteerToward(p, aim, aggression, p.controls);

    // Throttle: close the distance, then hold position behind the player.
    Vec3 playerFwd = QuatForward(player.flight.rotation);
    bool behindPlayer = Dot(playerFwd, to) < 0.0f;
    if (distance > 350.0f)
        p.controls.throttle = 0.8f;
    else if (distance < 120.0f && behindPlayer)
        p.controls.throttle = -0.6f;
    else
        p.controls.throttle = 0.0f;

    // Bursts while the aim is close enough.
    Vec3 fwd = QuatForward(p.flight.rotation);
    float cone = cosf(Lerp(6.0f, 3.5f, brain.skill) * DEG);
    bool lined = Dot(fwd, Normalize(aim - p.flight.position)) > cone && distance < FIRE_RANGE &&
                 ClearShot(p.flight.position, player.flight.position);
    brain.burstTimer -= dt;
    if (brain.burstTimer < -Lerp(1.4f, 0.6f, brain.skill) && lined && player.alive && MayShoot(index))
        brain.burstTimer = Lerp(0.6f, 1.0f, brain.skill);
    p.trigger = brain.burstTimer > 0.0f && lined;

    // Head-on pass: break off before a collision and extend.
    Vec3 closing = p.flight.velocity - player.flight.velocity;
    if (distance < 110.0f && Dot(closing, to) > 0.0f && Dot(fwd, to) > 0.0f) {
        brain.extendDirection = Normalize(Vec3(fwd.x, 0.15f, fwd.z));
        SetMode(brain, AiMode::EXTEND);
    }
}

// The player is down: fly on straight and climbing for a few seconds (out of any canyon the chase led into)
// before going back to the raid.
static void StartClimbOut(Plane& p)
{
    Vec3 fwd = QuatForward(p.flight.rotation);
    p.ai.extendDirection = Normalize(Vec3(fwd.x, 0.0f, fwd.z) + Vec3(0.0f, 0.45f, 0.0f));
    SetMode(p.ai, AiMode::EXTEND);
}

// Starts an evasion: aces stunt out of trouble, fighters break hard.
static void StartEvade(Plane& p)
{
    AiBrain& brain = p.ai;
    brain.evadeTurn = world.rng.Uniform() < 0.5f ? -1.0f : 1.0f;
    brain.evadeCooldown = Lerp(9.0f, 5.0f, brain.skill);
    SetMode(brain, AiMode::EVADE);
    if (p.kind == PlaneKind::ACE && p.flight.meter >= STUNT_COST) {
        float pick = world.rng.Uniform();
        if (pick < 0.6f)
            p.controls.stunt = brain.evadeTurn > 0.0f ? Stunt::ROLL_RIGHT : Stunt::ROLL_LEFT;
        else
            p.controls.stunt = Stunt::IMMELMANN;
    }
}

// True when the player is sitting on this plane's tail, within range.
static bool PlayerOnTail(const Plane& p, float range)
{
    const Plane& player = Player();
    if (!player.alive)
        return false;
    Vec3 to = p.flight.position - player.flight.position;
    float distance = Length(to);
    if (distance > range)
        return false;
    return Dot(QuatForward(player.flight.rotation), to / distance) > 0.97f;
}

// --- Update

void UpdateAi(int index, float dt)
{
    Plane& p = world.planes[index];
    AiBrain& brain = p.ai;
    brain.modeTime += dt;
    brain.evadeCooldown -= dt;
    brain.thinkTimer -= dt;
    p.controls = FlightControls();
    p.trigger = false;

    const Plane& player = Player();
    float distance = Length(player.flight.position - p.flight.position);
    if (brain.thinkTimer <= 0.0f) {
        brain.thinkTimer = world.rng.Range(0.4f, 0.8f);
        bool threatened = p.lastHit < 0.5f || PlayerOnTail(p, TAIL_RANGE);
        if (brain.mode == AiMode::RAID) {
            // Raiders come for the player when he is near, when hit, or after a short raid wherever he is.
            bool notice = distance < NOTICE_RANGE || p.lastHit < 1.0f || brain.modeTime > brain.raidTime;
            if (brain.passive)
                notice = p.lastHit < 1.0f || brain.modeTime > 25.0f;
            if (brain.strafer)
                notice = false;
            if (notice && player.alive)
                SetMode(brain, AiMode::ATTACK);
        } else if (brain.mode == AiMode::ATTACK) {
            float chance = Lerp(0.12f, 0.5f, brain.skill);
            if (brain.evadeOnce && PlayerOnTail(p, EVADE_ONCE_RANGE)) {
                // Wave 1: one break when the player gets behind, then it fights it out.
                StartEvade(p);
                brain.evadeOnce = false;
                brain.evadeCooldown = 1e9f;
            } else if (threatened && brain.evadeCooldown <= 0.0f && world.rng.Uniform() < chance) {
                StartEvade(p);
            }
            if (brain.mode == AiMode::ATTACK && !player.alive)
                StartClimbOut(p);
        }
    }

    if (Avoid(p)) {
        p.controls.stunt = Stunt::NONE;
        return;
    }
    switch (brain.mode) {
    case AiMode::RAID:
        Raid(p, dt);
        break;
    case AiMode::ATTACK:
        Attack(index, dt);
        break;
    case AiMode::EVADE:
        p.controls.turn = brain.evadeTurn;
        p.controls.pitch = 0.9f;
        p.controls.throttle = 1.0f;
        if (brain.modeTime > Lerp(2.6f, 1.6f, brain.skill)) {
            Vec3 fwd = QuatForward(p.flight.rotation);
            brain.extendDirection = Normalize(Vec3(fwd.x, 0.1f, fwd.z));
            SetMode(brain, p.kind == PlaneKind::ACE ? AiMode::ATTACK : AiMode::EXTEND);
        }
        break;
    case AiMode::EXTEND:
        SteerToward(p, p.flight.position + brain.extendDirection * 500.0f, 0.6f, p.controls);
        p.controls.throttle = 0.9f;
        if (brain.modeTime > Lerp(3.5f, 2.0f, brain.skill) || distance > EXTEND_RANGE)
            SetMode(brain, player.alive ? AiMode::ATTACK : AiMode::RAID);
        break;
    case AiMode::AVOID:
        SetMode(brain, AiMode::ATTACK);
        break;
    }
}

} // namespace game
