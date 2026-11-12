// The zeppelin "Silverback" (docs/design.md §2 phase 5, creative brief §8): arrives from the north-east, circles the
// island, fires tracers and flak from its gondola turrets, launches escort fighters from its belly, and goes down
// when its four engine pods are destroyed: nose-down tilt, chained explosions along the hull, a splash column.
#include "game/world.h"

namespace game {

constexpr float CRUISE_SPEED = 15.0f;
constexpr float LAST_STAND_SPEED = 22.0f; // with one engine left
constexpr uint32_t LAST_ENGINE_GLOW = 0xFFFF6A1A;
constexpr Vec3 LAST_ENGINE_EMISSIVE = Vec3(1.0f, 0.416f, 0.102f); // the same orange
constexpr float LAST_ENGINE_PULSE = 2.0f; // Hz, while armoured
constexpr Vec3 RED_HOT = Vec3(1.0f, 0.22f, 0.04f); // the last engine once its armour is off
constexpr uint32_t RED_HOT_GLOW = 0xFFFF3A0A;
constexpr float ENGINE_GAP = 12.0f;       // an engine never goes down within this of the previous one
constexpr float ARRIVE_SPEED = 28.0f;
constexpr float ARRIVE_DISTANCE = 600.0f; // she comes out of the clouds this far beyond her circle
constexpr float EMERGE_SPEED = 110.0f;    // m/s along the emerge path in the arrival cut (seen from far off)
constexpr float CIRCLE_RADIUS = 900.0f;
constexpr float CRUISE_ALTITUDE = 400.0f;
constexpr float SEA_CLEARANCE = 300.0f;   // the dive comes down only this far from any land
constexpr float SINK_TIME = 4.0f;
constexpr float WRECK_UNDERWATER = 6.0f; // the wreck's parts fade over this depth (cut-outs: gone 3 m under); the
                                         // sea is see-through, so the sunken cage would show
constexpr float ZEPPELIN_LOD_DISTANCE = 1200.0f; // farther: the coarse hull and no turrets
constexpr float SECTION_FIRST = 0.5f;    // crash seconds before the first hull section burns through
constexpr float SECTION_GAP = 0.5f;      // seconds between sections: the twelve burn away over about 6 s
constexpr float SECTION_GLOW = 0.3f;     // flames rush over a section this long before it is gone
constexpr float TURRET_RANGE = 900.0f;
constexpr float TURRET_BULLET_SPEED = 420.0f;
constexpr float FLAK_SPEED = 330.0f;
constexpr int ESCORTS_TOTAL = 14;        // pairs keep coming while she flies, up to this many
constexpr float LAUNCH_INTERVAL = 8.0f;  // at most one pair this often

// The zeppelin's engine voice while it is in the world, and the sound bed of its death dive (-1: none).
static audio::EngineId zeppelinSound = -1;
static audio::EngineId fallSound = -1;

// --- Helpers

static Quat ZeppelinRotation(const Zeppelin& z)
{
    Quat yaw = QuatAxisAngle(WORLD_UP, -z.heading);
    Quat pitch = QuatAxisAngle(Vec3(1, 0, 0), z.pitch);
    Quat roll = QuatAxisAngle(Vec3(0, 0, -1), z.roll);
    return QuatNormalize(yaw * pitch * roll);
}

Vec3 EngineWorldPosition(int index)
{
    const Zeppelin& z = world.zeppelin;
    return z.position + Rotate(z.rotation, content::ZeppelinEngine(index));
}

Vec3 TurretWorldPosition(int index)
{
    const Zeppelin& z = world.zeppelin;
    return z.position + Rotate(z.rotation, content::ZeppelinTurret(index));
}

bool InsideHull(const Vec3& point, float margin)
{
    const Zeppelin& z = world.zeppelin;
    if (z.state == ZeppelinState::ABSENT)
        return false;
    Vec3 local = Rotate(QuatConjugate(z.rotation), point - z.position);
    float x = local.x / HULL_RADII.x;
    float y = local.y / HULL_RADII.y;
    float zz = local.z / HULL_RADII.z;
    return x * x + y * y + zz * zz < margin * margin;
}

int EnginesLeft()
{
    int count = 0;
    for (int e = 0; e < world.zeppelin.engineCount; e++) {
        if (world.zeppelin.engineHealth[e] > 0.0f)
            count++;
    }
    return count;
}

// --- Life cycle

void ClearZeppelinSound()
{
    if (zeppelinSound >= 0)
        audio::SetEngine(zeppelinSound, Vec3(), Vec3(), 0.0f, 0.0f);
}

void ClearZeppelin()
{
    world.zeppelin = Zeppelin();
    if (zeppelinSound >= 0)
        audio::DestroyEngine(zeppelinSound);
    zeppelinSound = -1;
    if (fallSound >= 0)
        audio::DestroyEngine(fallSound);
    fallSound = -1;
}

void SpawnZeppelin()
{
    Zeppelin& z = world.zeppelin;
    if (zeppelinSound < 0)
        zeppelinSound = audio::CreateEngine(audio::EngineType::PIRATE);
    z.state = ZeppelinState::ARRIVING;
    z.turretCount = content::ZeppelinTurretCount();
    if (z.turretCount > MAX_TURRETS)
        z.turretCount = MAX_TURRETS;
    z.engineCount = content::ZeppelinEngineCount();
    if (z.engineCount > MAX_ENGINES)
        z.engineCount = MAX_ENGINES;
    for (int t = 0; t < z.turretCount; t++) {
        z.turretHealth[t] = TURRET_HEALTH;
        z.turretCooldown[t] = 2.0f + t * 0.7f;
        z.flakCooldown[t] = 4.0f + t * 1.1f;
        z.turretYaw[t] = 0.0f;
        z.turretBurst[t] = 0;
        z.turretFlash[t] = 0.0f;
    }
    for (int e = 0; e < z.engineCount; e++) {
        z.engineHealth[e] = ENGINE_HEALTH;
        z.engineFlash[e] = 0.0f;
    }
    // Circle the volcano close in (the fight happens 150-600 m from the airship) at 400 m, or higher where the
    // slopes under the circle need it.
    z.circleCenter = Vec3(assets.volcano.x, 0.0f, assets.volcano.z);
    z.circleRadius = CIRCLE_RADIUS;
    float highest = 0.0f;
    for (int i = 0; i < 64; i++) {
        float a = i * (2.0f * PI / 64.0f);
        float x = z.circleCenter.x + cosf(a) * z.circleRadius;
        float zz = z.circleCenter.z + sinf(a) * z.circleRadius;
        highest = fmaxf(highest, GroundHeight(x, zz));
    }
    z.altitude = fmaxf(CRUISE_ALTITUDE, highest + 150.0f);
    // Out of the cloud bank in the north-east, heading for the circle.
    Vec3 base = Normalize(Vec3(assets.pirateBase.x, 0.0f, assets.pirateBase.z) - z.circleCenter);
    if (LengthSq(base) < 0.01f)
        base = Normalize(Vec3(1.0f, 0.0f, -1.0f));
    z.arrivePosition = z.circleCenter + base * (z.circleRadius + ARRIVE_DISTANCE) + Vec3(0.0f, z.altitude, 0.0f);
    z.circleAngle = atan2f(base.z, base.x);
    // First the arrival cut: out from behind the pirate tower (content's emerge path).
    z.emergeTime = 0.0f;
    z.position = content::ZeppelinEmergePathPoint(0);
    Vec3 to = content::ZeppelinEmergePathPoint(1) - z.position;
    z.heading = atan2f(to.x, -to.z);
    z.rotation = ZeppelinRotation(z);
    z.prevPosition = z.position;
    z.prevRotation = z.rotation;
    z.launchTimer = 6.0f;
}

// --- Damage

// The death dive (further down) starts when the last engine goes; escorts launch in pairs (further down).
static void StartZeppelinDive(Zeppelin& z);
static int LaunchPair(Zeppelin& z, PlaneKind kind, int* slots);

// The last engine's pulse, 0..1 (2 Hz).
static float LastEnginePulse()
{
    return 0.5f + 0.5f * sinf(world.time * 2.0f * PI * LAST_ENGINE_PULSE);
}

// The hull's burn-away. Drawing each hull section as its own mesh showed nothing on the Xbox (2026-11-11, xemu:
// only the body and the girders appeared), so the skin is drawn as one merged mesh per stage: stage k holds the
// sections that have not burnt through after k of them have. The sections burn outward from the last engine
// killed: the front pair or the rear pair, so there are two orders and two sets of stage meshes.
constexpr int BURN_ORDERS = 2;
static int burnSequence[BURN_ORDERS][MAX_HULL_SECTIONS];
static render::MeshId burnStages[BURN_ORDERS][MAX_HULL_SECTIONS];

// The sections in the order they burn, nearest the origin (a z along the hull) first.
static void SortOutward(float origin, int* sequence)
{
    bool done[MAX_HULL_SECTIONS] = {};
    for (int order = 0; order < assets.hullSectionCount; order++) {
        int best = 0;
        float bestDistance = 1e9f;
        for (int i = 0; i < assets.hullSectionCount; i++) {
            float d = fabsf(assets.hullSectionCenter[i].z - origin);
            if (!done[i] && d < bestDistance) {
                bestDistance = d;
                best = i;
            }
        }
        done[best] = true;
        sequence[order] = best;
    }
}

// The z of the front engines (order 0) or the rear ones (order 1).
static float EngineOrigin(int order)
{
    float front = 1e9f;
    float rear = -1e9f;
    for (int e = 0; e < content::ZeppelinEngineCount(); e++) {
        front = fminf(front, content::ZeppelinEngine(e).z);
        rear = fmaxf(rear, content::ZeppelinEngine(e).z);
    }
    return order == 0 ? front : rear;
}

void BuildZeppelinStages()
{
    MeshData sections[MAX_HULL_SECTIONS];
    for (int i = 0; i < assets.hullSectionCount; i++)
        sections[i] = content::MakeZeppelinHullSection(i);
    for (int order = 0; order < BURN_ORDERS; order++) {
        SortOutward(EngineOrigin(order), burnSequence[order]);
        burnStages[order][0] = render::NO_MESH;
        for (int stage = 1; stage < assets.hullSectionCount; stage++) {
            MeshData skin;
            for (int k = stage; k < assets.hullSectionCount; k++) {
                const MeshData& part = sections[burnSequence[order][k]];
                uint16_t base = static_cast<uint16_t>(skin.vertices.size());
                skin.vertices.insert(skin.vertices.end(), part.vertices.begin(), part.vertices.end());
                for (uint16_t index : part.indices)
                    skin.indices.push_back(static_cast<uint16_t>(base + index));
            }
            burnStages[order][stage] = render::CreateMesh(skin);
        }
    }
}

// Starts the burn-away outward from the last engine killed.
static void ScheduleBurn(Zeppelin& z, int lastEngine)
{
    float engine = content::ZeppelinEngine(lastEngine).z;
    z.burnOrder = fabsf(engine - EngineOrigin(0)) <= fabsf(engine - EngineOrigin(1)) ? 0 : 1;
    for (int k = 0; k < assets.hullSectionCount; k++)
        z.burnAt[burnSequence[z.burnOrder][k]] = SECTION_FIRST + k * SECTION_GAP;
}

void DamageEngine(int index, float amount, const Vec3& at)
{
    Zeppelin& z = world.zeppelin;
    if (z.engineHealth[index] <= 0.0f)
        return;
    if (LastEngineArmoured()) {
        // Shots spark off the armour.
        Sparks(at, Normalize(at - EngineWorldPosition(index)), 6);
        audio::Play3D(audio::Sfx::HIT, at, 0.6f, world.rng.Range(1.4f, 1.6f));
        return;
    }
    z.engineHealth[index] -= amount;
    // Holding on until ENGINE_GAP after the previous engine, so every kill gets its moment.
    if (z.engineHealth[index] <= 0.0f && world.time - z.lastEngineDown < ENGINE_GAP)
        z.engineHealth[index] = 1.0f;
    z.engineFlash[index] = 1.0f;
    Sparks(at, Normalize(at - EngineWorldPosition(index)), 3);
    HudHitMarker();
    audio::Play3D(audio::Sfx::HIT, at, 0.8f, world.rng.Range(0.8f, 0.95f));
    AddScore(10, nullptr);
    if (z.engineHealth[index] > 0.0f)
        return;
    z.engineHealth[index] = 0.0f;
    z.lastEngineDown = world.time;
    Explosion(EngineWorldPosition(index), 26.0f, z.velocity);
    world.slowMo = 0.5f;
    Rumble(1.0f, 1.0f, 400);
    int left = EnginesLeft();
    AddScore(1500, "ENGINE DOWN!");
    LogEvent("engine %d destroyed, %d left", index, left);
    if (left == 3)
        Radio(Speaker::HARBOUR, "One engine down! Three to go!");
    else if (left == 2)
        Radio(Speaker::PIRATE, "Keep her in the air, you apes!");
    if (left == 1) {
        z.lastStand = true;
        LaunchPair(z, PlaneKind::ACE, z.lastAces);
        Radio(Speaker::HARBOUR, "Last engine's armoured! Down her two aces first!");
        LogEvent("last engine: aces launched");
    }
    if (left > 0)
        return;
    z.state = ZeppelinState::CRASHING;
    z.crashTime = 0.0f;
    ScheduleBurn(z, index);
    StartZeppelinDive(z);
    z.explosionTimer = 0.0f;
    world.slowMo = 1.2f;
    LogEvent("zeppelin going down");
}

void DamageTurret(int index, float amount, const Vec3& at)
{
    Zeppelin& z = world.zeppelin;
    if (z.turretHealth[index] <= 0.0f)
        return;
    z.turretHealth[index] -= amount;
    z.turretFlash[index] = 1.0f;
    Sparks(at, Normalize(at - z.position), 3);
    HudHitMarker();
    audio::Play3D(audio::Sfx::HIT, at, 0.7f, 1.2f);
    AddScore(10, nullptr);
    if (z.turretHealth[index] > 0.0f)
        return;
    z.turretHealth[index] = 0.0f;
    Explosion(TurretWorldPosition(index), 12.0f, z.velocity);
    AddScore(300, "TURRET");
}

// --- Behaviour

// The arrival cut: along the emerge path from behind the tower at EMERGE_SPEED, nose along the path. When the
// camera cuts back to the player she is put at the start of her arrival course, out of his sight.
static void Emerge(Zeppelin& z, float dt)
{
    z.emergeTime += dt;
    if (z.emergeTime >= EMERGE_TIME) {
        z.emergeTime = -1.0f;
        z.position = z.arrivePosition;
        z.prevPosition = z.position;
        Vec3 entry = z.circleCenter + Vec3(cosf(z.circleAngle), 0.0f, sinf(z.circleAngle)) * z.circleRadius;
        z.heading = atan2f(entry.x - z.position.x, -(entry.z - z.position.z));
        return;
    }
    float along = z.emergeTime * EMERGE_SPEED;
    int count = content::ZeppelinEmergePathCount();
    Vec3 from = content::ZeppelinEmergePathPoint(0);
    for (int i = 1; i < count; i++) {
        Vec3 to = content::ZeppelinEmergePathPoint(i);
        float length = Length(to - from);
        if (along <= length || i == count - 1) {
            Vec3 dir = (to - from) / fmaxf(length, 1.0f);
            z.position = from + dir * fminf(along, length);
            z.velocity = dir * EMERGE_SPEED;
            z.heading = atan2f(dir.x, -dir.z);
            return;
        }
        along -= length;
        from = to;
    }
}

static void Move(Zeppelin& z, float dt)
{
    if (z.emergeTime >= 0.0f) {
        Emerge(z, dt);
        return;
    }
    if (z.state == ZeppelinState::ARRIVING) {
        Vec3 entry = z.circleCenter + Vec3(cosf(z.circleAngle), 0.0f, sinf(z.circleAngle)) * z.circleRadius;
        entry.y = z.altitude;
        Vec3 to = entry - z.position;
        float distance = Length(to);
        z.heading = atan2f(to.x, -to.z);
        z.velocity = to / fmaxf(distance, 1.0f) * ARRIVE_SPEED;
        if (distance < 60.0f) {
            z.state = ZeppelinState::CIRCLING;
            LogEvent("zeppelin circling at %d m", static_cast<int>(z.altitude));
        }
    } else {
        // Counter-clockwise seen from above; heading along the tangent.
        float speed = z.lastStand ? LAST_STAND_SPEED : CRUISE_SPEED;
        z.circleAngle -= speed / z.circleRadius * dt;
        Vec3 target = z.circleCenter + Vec3(cosf(z.circleAngle), 0.0f, sinf(z.circleAngle)) * z.circleRadius;
        target.y = z.altitude;
        Vec3 tangent(sinf(z.circleAngle), 0.0f, -cosf(z.circleAngle));
        Vec3 pull = (target - z.position) * 0.05f;
        z.velocity = tangent * speed + pull;
        float want = atan2f(z.velocity.x, -z.velocity.z);
        z.heading += WrapPi(want - z.heading) * Clamp(dt * 0.5f, 0.0f, 1.0f);
    }
    z.position += z.velocity * dt;
}

static void FireTurrets(Zeppelin& z, float dt)
{
    const Plane& player = Player();
    for (int t = 0; t < z.turretCount; t++) {
        z.turretFlash[t] = Clamp(z.turretFlash[t] - dt / 0.08f, 0.0f, 1.0f);
        if (z.turretHealth[t] <= 0.0f || !player.alive || world.titleMode)
            continue;
        Vec3 from = TurretWorldPosition(t);
        Vec3 to = player.flight.position - from;
        float distance = Length(to);
        if (distance > TURRET_RANGE)
            continue;
        Vec3 local = Rotate(QuatConjugate(z.rotation), to);
        z.turretYaw[t] = atan2f(-local.x, -local.z);
        Vec3 aim = LeadPoint(from, z.velocity, player.flight.position, player.flight.velocity, TURRET_BULLET_SPEED);
        z.turretCooldown[t] -= dt;
        if (z.turretCooldown[t] <= 0.0f) {
            Vec3 error(world.rng.Range(-1, 1), world.rng.Range(-1, 1), world.rng.Range(-1, 1));
            Vec3 dir = Normalize(aim - from + error * (distance * 0.05f));
            FireTurretBullet(from + dir * 4.0f, z.velocity + dir * TURRET_BULLET_SPEED);
            audio::Play3D(audio::Sfx::GUN, from, 0.7f, 0.7f);
            z.turretBurst[t]++;
            if (z.turretBurst[t] >= 6) {
                z.turretBurst[t] = 0;
                z.turretCooldown[t] = world.rng.Range(2.2f, 3.2f);
            } else {
                z.turretCooldown[t] = 0.12f;
            }
        }
        z.flakCooldown[t] -= dt;
        if (z.flakCooldown[t] <= 0.0f) {
            z.flakCooldown[t] = world.rng.Range(4.5f, 6.5f);
            Vec3 flakAim = LeadPoint(from, z.velocity, player.flight.position, player.flight.velocity, FLAK_SPEED);
            flakAim += Vec3(world.rng.Range(-1, 1), world.rng.Range(-1, 1), world.rng.Range(-1, 1)) * 30.0f;
            Vec3 dir = Normalize(flakAim - from);
            FireFlak(from, dir * FLAK_SPEED, Length(flakAim - from) / FLAK_SPEED);
        }
    }
}

static int EscortsAlive()
{
    int count = 0;
    for (int i = 1; i < MAX_PLANES; i++) {
        if (world.planes[i].active && world.planes[i].escort)
            count++;
    }
    return count;
}

// Two escorts drop from the belly side by side and go for the player; their plane slots go to slots (2, may be
// nullptr). Returns how many launched.
static int LaunchPair(Zeppelin& z, PlaneKind kind, int* slots)
{
    Vec3 dir = Normalize(QuatForward(z.rotation) + Vec3(0.0f, -0.4f, 0.0f));
    for (int k = 0; k < 2; k++) {
        float side = k == 0 ? -14.0f : 14.0f;
        Vec3 belly = z.position + Rotate(z.rotation, Vec3(side, -HULL_RADII.y - 12.0f, -20.0f));
        int index = SpawnPlane(kind, belly, dir, 55.0f);
        if (index < 0)
            return k;
        world.planes[index].escort = true;
        world.planes[index].ai.mode = AiMode::ATTACK;
        if (slots)
            slots[k] = index;
        z.launched++;
        LogEvent("escort %d launched", z.launched);
    }
    return 2;
}

bool LastEngineArmoured()
{
    const Zeppelin& z = world.zeppelin;
    if (!z.lastStand || z.armourOff)
        return false;
    for (int slot : z.lastAces) {
        if (slot > 0 && world.planes[slot].active && world.planes[slot].alive && world.planes[slot].escort)
            return true;
    }
    return false;
}

// Escorts come in pairs, a new pair as soon as the last one is down (the sky is never empty), until she has launched
// them all; the aces sent out with one engine left are the last.
static void LaunchEscorts(Zeppelin& z, float dt)
{
    z.launchTimer -= dt;
    if (z.launchTimer > 0.0f || z.launched >= ESCORTS_TOTAL || EscortsAlive() > 0 || z.lastStand)
        return;
    z.launchTimer = LAUNCH_INTERVAL;
    if (z.launched == 0)
        Radio(Speaker::HARBOUR, "Fighters dropping from her belly!");
    LaunchPair(z, PlaneKind::FIGHTER, nullptr);
}

// Smoke from damaged engines, fire from dead ones.
static void EngineTrails(const Zeppelin& z)
{
    for (int e = 0; e < z.engineCount; e++) {
        float health = z.engineHealth[e] / ENGINE_HEALTH;
        if (health > 0.6f)
            continue;
        Vec3 at = EngineWorldPosition(e);
        if (world.rng.Uniform() < 0.5f)
            SmokePuff(at, z.velocity + Vec3(0, 2, 0), 4.0f, health <= 0.0f ? 0.95f : 0.6f);
        if (health <= 0.0f && world.rng.Uniform() < 0.6f)
            FirePuff(at, z.velocity, 6.0f);
    }
}

// True when there is open sea under the point and for SEA_CLEARANCE all round it.
static bool OpenSea(const Vec3& at)
{
    if (GroundHeight(at.x, at.z) > 0.01f)
        return false;
    for (int i = 0; i < 8; i++) {
        float a = i * (PI / 4.0f);
        if (GroundHeight(at.x + cosf(a) * SEA_CLEARANCE, at.z + sinf(a) * SEA_CLEARANCE) > 0.01f)
            return false;
    }
    return true;
}

// A point on the hull in local space: along -1 (tail) .. 1 (nose), around 0 (top) .. 2 pi.
static Vec3 HullPoint(float along, float around)
{
    float girth = sqrtf(fmaxf(0.0f, 1.0f - along * along));
    return Vec3(sinf(around) * HULL_RADII.x * girth, cosf(around) * HULL_RADII.y * girth, -along * HULL_RADII.z);
}

// --- The death dive

constexpr float DIVE_TIME = 15.0f;        // seconds from the last engine to the splash (6 of them burning off)
constexpr float DIVE_PITCH = 30.0f * DEG; // nose-down angle at the end of the dive
constexpr int MAX_PIECES = 6;
constexpr float PIECE_PUFF_GAP = 4.0f;    // metres between a falling piece's smoke puffs (a solid trail)
constexpr float PIECE_FIRE_TIME = 1.5f;   // seconds a broken-off piece burns
constexpr int SEA_FIRES = 5;              // burning wreckage left on the water
constexpr float SEA_FIRE_TIME = 12.0f;

// Parts that break off and fall on their own, trailing fire and smoke.
struct Piece {
    bool active;
    render::MeshId mesh;
    Vec3 position;
    Vec3 velocity;
    Quat rotation;
    Vec3 spinAxis;
    float spin;
    float age;
    Vec3 lastPuff;      // where the last smoke puff was left
};

static Piece pieces[MAX_PIECES];

// The nearest open sea from a point: the first of 16 directions (from the island's middle outward first) that
// reaches water SEA_CLEARANCE from any land.
static Vec3 NearestSea(const Vec3& from, const Vec3& center)
{
    Vec3 best = from;
    float bestDistance = 1e9f;
    for (int i = 0; i < 16; i++) {
        float a = i * (PI / 8.0f);
        Vec3 dir(cosf(a), 0.0f, sinf(a));
        for (float d = 100.0f; d < 3000.0f; d += 100.0f) {
            Vec3 at = from + dir * d;
            if (!OpenSea(at))
                continue;
            // Prefer heading away from the island (it falls with the island behind it in the last shot).
            float away = Dot(dir, Normalize(Vec3(from.x - center.x, 0.0f, from.z - center.z)));
            float score = d - away * 300.0f;
            if (score < bestDistance) {
                bestDistance = score;
                best = at;
            }
            break;
        }
    }
    best.y = 0.0f;
    return best;
}

// Sets the death dive's line: from here to the nearest open sea.
static void StartZeppelinDive(Zeppelin& z)
{
    if (fallSound < 0)
        fallSound = audio::CreateEngine(audio::EngineType::ZEPPELIN_FALL);
    z.crashStart = z.position;
    z.crashTarget = NearestSea(z.position, z.circleCenter);
    for (Piece& p : pieces)
        p.active = false;
}

static void BreakOff(Zeppelin& z, render::MeshId mesh, const Vec3& local)
{
    for (Piece& p : pieces) {
        if (p.active)
            continue;
        p.active = true;
        p.mesh = mesh;
        p.position = z.position + Rotate(z.rotation, local);
        Vec3 out = Normalize(Rotate(z.rotation, Vec3(local.x, 0.0f, 0.0f)) + Vec3(0.0f, -0.3f, 0.0f));
        p.velocity = z.velocity + out * 12.0f;
        p.rotation = z.rotation;
        p.spinAxis = Normalize(Vec3(world.rng.Range(-1, 1), world.rng.Range(-1, 1), world.rng.Range(-1, 1)));
        p.spin = world.rng.Range(1.0f, 2.5f);
        p.age = 0.0f;
        p.lastPuff = p.position;
        Explosion(p.position, 16.0f, z.velocity);
        return;
    }
}

static void UpdatePieces(float dt)
{
    for (Piece& p : pieces) {
        if (!p.active)
            continue;
        p.velocity.y -= GRAVITY * dt;
        p.position += p.velocity * dt;
        p.rotation = QuatNormalize(QuatAxisAngle(p.spinAxis, p.spin * dt) * p.rotation);
        p.age += dt;
        // A thick trail: a puff every few metres of the fall, so it stays solid at any speed; fire at first.
        Vec3 path = p.position - p.lastPuff;
        float length = Length(path);
        int puffs = length >= PIECE_PUFF_GAP ? static_cast<int>(length / PIECE_PUFF_GAP) : 0;
        for (int k = 1; k <= puffs && k <= 8; k++)
            PieceSmoke(p.lastPuff + path * (k * PIECE_PUFF_GAP / length));
        if (puffs > 0)
            p.lastPuff = p.position;
        if (p.age < PIECE_FIRE_TIME)
            FirePuff(p.position, p.velocity * 0.5f, 7.0f);
        if (p.position.y < GroundHeight(p.position.x, p.position.z)) {
            p.active = false;
            if (GroundHeight(p.position.x, p.position.z) <= 0.01f)
                Splash(p.position, 14.0f);
            else
                Explosion(p.position, 14.0f, Vec3());
        }
    }
}

static void DrawPieces(const render::Material& material)
{
    for (const Piece& p : pieces) {
        if (p.active)
            render::DrawMesh(p.mesh, MatTRS(p.position, p.rotation, Vec3(1, 1, 1)), material);
    }
}

// The last engine down: 15 s from the circle to open sea, burning off for the first 6, nose going down to 30
// degrees, engine pods and turrets breaking off on the way, an explosion along the keel every 1.2-1.8 s.
static void Crash(Zeppelin& z, float dt)
{
    z.crashTime += dt;
    float t = Clamp(z.crashTime / DIVE_TIME, 0.0f, 1.0f);
    Vec3 path = z.crashTarget - z.crashStart;
    Vec3 flat = Normalize(Vec3(path.x, 0.0f, path.z));
    Vec3 previous = z.position;
    // Along the line to the sea; height falls slowly at first, then fast (t squared).
    z.position = z.crashStart + Vec3(path.x * t, 0.0f, path.z * t);
    z.position.y = z.crashStart.y * (1.0f - t * t) + 2.0f;
    // Over the island's slopes on the way it stays clear of the ground.
    if (t < 0.85f)
        z.position.y = fmaxf(z.position.y, GroundHeight(z.position.x, z.position.z) + 60.0f);
    z.velocity = (z.position - previous) / fmaxf(dt, 1e-4f);
    z.heading += WrapPi(atan2f(flat.x, -flat.z) - z.heading) * Clamp(dt * 1.5f, 0.0f, 1.0f);
    z.pitch = -DIVE_PITCH * Clamp(t * 1.4f, 0.0f, 1.0f);
    z.roll = sinf(z.crashTime * 0.5f) * 8.0f * DEG;

    // Breakaway pieces: two engine pods, then the other two, then a turret.
    static const float BREAK_TIMES[3] = {7.0f, 9.5f, 11.5f};
    for (int k = 0; k < 3; k++) {
        if (z.crashTime - dt >= BREAK_TIMES[k] || z.crashTime < BREAK_TIMES[k])
            continue;
        if (k < 2) {
            for (int e = k * 2; e < k * 2 + 2 && e < z.engineCount; e++) {
                z.engineGone[e] = true;
                BreakOff(z, assets.zeppelinEngine, content::ZeppelinEngine(e));
            }
        } else if (z.turretCount > 0) {
            z.turretHealth[0] = 0.0f;
            BreakOff(z, assets.turret, content::ZeppelinTurret(0));
        }
    }

    // Fire and smoke at every section that has started to burn: big fireballs, an explosion as each section goes,
    // and a black smoke column per section; explosions along the keel.
    z.explosionTimer -= dt;
    bool puff = z.explosionTimer <= 0.0f;
    if (puff)
        z.explosionTimer = 0.6f;
    for (int i = 0; i < assets.hullSectionCount; i++) {
        float burning = z.crashTime - z.burnAt[i];
        if (burning < 0.0f)
            continue;
        Vec3 center = z.position + Rotate(z.rotation, assets.hullSectionCenter[i]);
        // While a section burns through, flames rush over its skin.
        if (burning < SECTION_GLOW) {
            Vec3 at = center + Rotate(z.rotation, HullPoint(0.0f, world.rng.Range(-1.4f, 1.4f)) * 0.9f -
                                                      Vec3(0.0f, 0.0f, HullPoint(0.0f, 0.0f).z));
            FirePuff(at, z.velocity + Vec3(0.0f, 6.0f, 0.0f), world.rng.Range(14.0f, 20.0f));
        }
        if (burning - dt < SECTION_GLOW && burning >= SECTION_GLOW)
            Explosion(center + Vec3(0.0f, HULL_RADII.y * 0.6f, 0.0f), 20.0f, z.velocity);
        if (world.rng.Uniform() < 0.06f) {
            Vec3 at = center + Rotate(z.rotation, Vec3(world.rng.Range(-1.0f, 1.0f) * HULL_RADII.x,
                                                       world.rng.Range(-0.3f, 1.0f) * HULL_RADII.y, 0.0f));
            FirePuff(at, z.velocity + Vec3(0.0f, 4.0f, 0.0f), world.rng.Range(15.0f, 22.0f));
        }
        if (puff && i % 2 == 0)
            SmokeColumnPuff(center + Vec3(0.0f, HULL_RADII.y, 0.0f), z.velocity * 0.3f);
    }
    z.burnTimer -= dt;
    if (z.burnTimer <= 0.0f) {
        z.burnTimer = world.rng.Range(1.2f, 1.8f);
        Vec3 keel = z.position + Rotate(z.rotation, Vec3(0.0f, -HULL_RADII.y, world.rng.Range(-0.7f, 0.7f) *
                                                                                 HULL_RADII.z));
        Explosion(keel, world.rng.Range(18.0f, 26.0f), z.velocity);
    }

    if (fallSound >= 0)
        audio::SetEngine(fallSound, z.position, z.velocity, t, 1.0f);
    if (t >= 1.0f) {
        if (fallSound >= 0)
            audio::DestroyEngine(fallSound);
        fallSound = -1;
        z.state = ZeppelinState::SUNK;
        z.crashTime = 0.0f;
        audio::Play(audio::Sfx::BIG_EXPLOSION);
        audio::Play(audio::Sfx::ZEPPELIN_DEATH);
        ZeppelinSplash(z.position);
        OnZeppelinSunk();
    }
}

// Wreckage burning on the water after the splash, along where the hull went in: fire and smoke for 12 s.
static void SeaFires(const Zeppelin& z)
{
    if (z.crashTime > SEA_FIRE_TIME)
        return;
    float fade = 1.0f - z.crashTime / SEA_FIRE_TIME;
    for (int i = 0; i < SEA_FIRES; i++) {
        float along = (i - (SEA_FIRES - 1) * 0.5f) * 30.0f;
        float across = (i % 2 == 0 ? 1.0f : -1.0f) * 18.0f;
        Vec3 at = z.position + Rotate(z.rotation, Vec3(across, 0.0f, along));
        at.y = 1.0f;
        if (world.rng.Uniform() < 0.5f * fade)
            FirePuff(at, Vec3(0.0f, 2.0f, 0.0f), world.rng.Range(5.0f, 8.0f));
        if (world.rng.Uniform() < 0.12f * fade)
            SmokePuff(at + Vec3(0.0f, 3.0f, 0.0f), Vec3(1.5f, 4.0f, 0.5f), 5.0f, 0.9f);
    }
}

void UpdateZeppelin(float dt)
{
    Zeppelin& z = world.zeppelin;
    z.prevPosition = z.position;
    z.prevRotation = z.rotation;
    if (z.state == ZeppelinState::ABSENT)
        return;
    z.propAngle += dt * 12.0f;
    for (int e = 0; e < z.engineCount; e++)
        z.engineFlash[e] = Clamp(z.engineFlash[e] - dt / 0.08f, 0.0f, 1.0f);
    UpdatePieces(dt);
    if (z.state == ZeppelinState::SUNK) {
        z.crashTime += dt;
        SeaFires(z);
        z.position.y -= (HULL_RADII.y * 2.5f / SINK_TIME) * dt;
        if (zeppelinSound >= 0)
            audio::SetEngine(zeppelinSound, z.position, Vec3(), 0.0f, 0.0f);
        return;
    }
    if (z.state == ZeppelinState::CRASHING) {
        Crash(z, dt);
    } else {
        Move(z, dt);
        FireTurrets(z, dt);
        if (!world.titleMode)
            LaunchEscorts(z, dt);
    }
    z.rotation = ZeppelinRotation(z);
    EngineTrails(z);
    // The last engine: an orange pulse while armoured; red-hot once its aces are down.
    if (z.lastStand && !z.armourOff && z.state == ZeppelinState::CIRCLING && !LastEngineArmoured()) {
        z.armourOff = true;
        Radio(Speaker::HARBOUR, "Her aces are down! Hit that engine!");
        LogEvent("last engine: armour off");
    }
    for (int e = 0; e < z.engineCount && z.lastStand && z.state == ZeppelinState::CIRCLING; e++) {
        if (z.engineHealth[e] <= 0.0f)
            continue;
        if (z.armourOff)
            Glow(EngineWorldPosition(e), 22.0f, RED_HOT_GLOW);
        else
            Glow(EngineWorldPosition(e), 14.0f + 10.0f * LastEnginePulse(), LAST_ENGINE_GLOW);
    }
    if (zeppelinSound >= 0)
        audio::SetEngine(zeppelinSound, z.position, z.velocity, 0.1f + 0.1f * EnginesLeft(), 1.0f);
}

// --- Drawing

void DrawZeppelin()
{
    const Zeppelin& z = world.zeppelin;
    if (z.state == ZeppelinState::ABSENT || (z.state == ZeppelinState::SUNK && z.crashTime > SINK_TIME))
        return;
    Vec3 position = Lerp(z.prevPosition, z.position, world.drawAlpha);
    Quat rotation = QuatSlerp(z.prevRotation, z.rotation, world.drawAlpha);
    Mat4 model = MatTRS(position, rotation, Vec3(1, 1, 1));
    render::Material hull;
    hull.texture = assets.zeppelinTexture;
    hull.specular = 0.35f;
    // Its own sky-and-sea ambient, so the silver hull, fins and engine pods stay light when backlit.
    hull.hemisphere = true;
    if (z.state == ZeppelinState::CRASHING || z.state == ZeppelinState::SUNK) {
        hull.tint = 0xFF8A8078;
        hull.blend = render::Blend::ALPHA_TEST;
        hull.underwaterFade = WRECK_UNDERWATER;
    }
    bool distant = LengthSq(position - CurrentCamera().position) > ZEPPELIN_LOD_DISTANCE * ZEPPELIN_LOD_DISTANCE;
    if (z.state != ZeppelinState::CRASHING && z.state != ZeppelinState::SUNK) {
        render::DrawMesh(distant ? assets.zeppelinDistant : assets.zeppelin, model, hull);
    } else {
        // Burning apart: the body, the girders, and the skin of the sections not yet burnt through.
        render::Material frame = hull;
        frame.tint = 0xFF3A332E;
        frame.hemisphere = false;
        int gone = 0;
        for (int i = 0; i < assets.hullSectionCount; i++) {
            if (z.state == ZeppelinState::SUNK || z.crashTime - z.burnAt[i] >= SECTION_GLOW)
                gone++;
        }
        if (gone == 0) {
            render::DrawMesh(assets.zeppelin, model, hull);
        } else {
            render::DrawMesh(assets.zeppelinBody, model, hull);
            render::DrawMesh(assets.zeppelinFrame, model, frame);
            if (gone < assets.hullSectionCount)
                render::DrawMesh(burnStages[z.burnOrder][gone], model, hull);
        }
    }

    DrawPieces(hull);
    for (int e = 0; e < z.engineCount; e++) {
        if (z.engineGone[e])
            continue;
        render::Material m = hull;
        if (z.engineHealth[e] <= 0.0f)
            m.tint = 0xFF3A3430;
        float flash = z.engineFlash[e];
        m.emissive = Vec3(flash, flash, flash);
        // The last engine glows and pulses: the target.
        if (z.lastStand && z.engineHealth[e] > 0.0f && z.state == ZeppelinState::CIRCLING) {
            Vec3 glow = z.armourOff ? RED_HOT : LAST_ENGINE_EMISSIVE * (0.35f + 0.65f * LastEnginePulse());
            m.emissive = Vec3(fmaxf(glow.x, flash), fmaxf(glow.y, flash), fmaxf(glow.z, flash));
        }
        Mat4 local = MatTranslate(content::ZeppelinEngine(e));
        render::DrawMesh(assets.zeppelinEngine, local * model, m);
    }
    for (int t = 0; t < z.turretCount && !distant; t++) {
        render::Material m = hull;
        if (z.turretHealth[t] <= 0.0f)
            m.tint = 0xFF3A3430;
        float flash = z.turretFlash[t];
        m.emissive = Vec3(flash, flash, flash);
        Mat4 local = MatRotate(QuatAxisAngle(WORLD_UP, z.turretYaw[t])) * MatTranslate(content::ZeppelinTurret(t));
        render::DrawMesh(assets.turret, local * model, m);
    }
}

} // namespace game
