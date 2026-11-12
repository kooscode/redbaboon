// The mission script "Raid on Isla Platano" (docs/design.md §2, creative brief §8): intro, wave 1 (with an easy first
// kill crossing the player's nose), a breather pointing at the canyon rings, wave 2 with two named aces, the zeppelin
// boss, its crash and the victory; lives and respawns; rings, crates, score and radio messages.
#include "game/world.h"

#include <stdarg.h>
#include <stdio.h>

namespace game {

constexpr float INTRO_RUN = 150.0f;       // the plane starts this far along the waterfront before the pier
constexpr float INTRO_HEIGHT = 30.0f;     // above the quay: 15 m over the roofs
constexpr float INTRO_CAMERA = 80.0f;     // the first shot's camera: this far along the wharf past the pier
constexpr float WAVE1_RANGE = 1300.0f;    // wave 1's first raider pair: the wingman starts this far out
constexpr float WAVE1_SECOND_PAIR = 30.0f; // the second pair comes after the first kill, not before this
constexpr float WAVE1_SECOND_RANGE = 1500.0f;
constexpr float BREATHER_TIME = 25.0f;
constexpr float BREATHER_MAX = 75.0f;     // the breather waits for a ring run in progress, up to this long
constexpr float BOSS_DELAY = 4.0f;
constexpr float WAVE3_DELAY = 6.0f;       // seconds from the radio call to the raiders' arrival
constexpr int WAVE3_SIZE = 4;
constexpr float WAVE3_RAID_TIME = 25.0f;  // wave 3 strafes the harbour this long before turning on the player
constexpr float WHARF_FALL_RANGE = 900.0f; // a wave 3 raider shot down this close to the wharf falls in beside it
constexpr float STRAFER_START = 700.0f;   // wave 3's last raider comes in along the waterfront from this far
constexpr float RESPAWN_TIME = 3.0f;
constexpr float BOSS_RESPAWN_RANGE = 800.0f; // a respawn during the boss starts this far from the zeppelin
// After the splash (game seconds; the victory runs at half speed, so 2 real seconds each): the column peaks at 2,
// "VICTORY!" 1 real second later, the results 2 real seconds after that.
constexpr float VICTORY_BANNER = 2.5f;
constexpr float VICTORY_TIME = 3.5f;
constexpr float VICTORY_HOLD = 2.0f;      // real seconds "VICTORY!" stays, up to the results
constexpr float DEFEAT_TIME = 3.5f;
constexpr float INVULNERABLE_TIME = 2.0f;
constexpr float RADIO_TIME = 4.0f;
constexpr int MAX_RADIO = 6;
constexpr int MAX_RINGS = 16;
constexpr float RING_RADIUS = 15.0f;
constexpr float RING_LOD_DISTANCE = 400.0f; // rings farther than this draw their coarse mesh
constexpr int MAX_CRATES = 8;
constexpr float CRATE_PICKUP = 10.0f;
constexpr float BARK_GAP = 3.0f;          // seconds between the pilot's cheers

constexpr int SCORE_FIGHTER = 500;
constexpr int SCORE_ACE = 1000;
constexpr int SCORE_RING = 250;
constexpr int SCORE_ALL_RINGS = 2000;
constexpr int SCORE_STUNT = 100;         // a stunt flown in a fight
constexpr int SCORE_STUNT_CALM = 10;     // with no enemy near and nobody shooting
constexpr float STUNT_FIGHT_RANGE = 400.0f;
constexpr float STUNT_SHOT_AT = 2.0f;    // seconds since enemy fire passed close that still count as a fight
constexpr float STUNT_REPEAT = 5.0f;     // the same stunt again within this scores nothing
constexpr int SCORE_ZEPPELIN = 5000;
constexpr int SCORE_CRATE = 100;

enum class CrateKind {
    REPAIR,
    ROCKETS,
};

struct Crate {
    bool active;
    CrateKind kind;
    Vec3 position;
    float spin;
};

// Mission progress.
struct Script {
    Phase phase = Phase::INTRO;
    float phaseTime = 0.0f;
    bool breather = false;     // between wave 1 and wave 2
    int spawned = 0;           // planes of the current wave put in the air
    float spawnTimer = 0.0f;
    int waveKills = 0;
    int waveSize = 0;
    float endTimer = 0.0f;     // victory or defeat countdown
    bool zeppelinSpawned = false;
    float barkTimer = 0.0f;
    float lastRing = -99.0f;   // mission time of the last ring flown through
    float radioHold = INTRO_TIME + 0.5f; // seconds until radio messages may show (the intro's title card)
    bool wharfSplash = false;  // a wave 3 raider has been sent into the sea by the wharf
    float lastStunt[5] = {-99.0f, -99.0f, -99.0f, -99.0f, -99.0f}; // per Stunt: mission time of its last points
    char objective[64] = {};
};

// The script's state.
static Script script;

// The radio queue; the first one is on screen.
static RadioMessage radio[MAX_RADIO];
static int radioCount = 0;

// Rings flown through.
static bool ringPassed[MAX_RINGS];
static int ringCount = 0;

static Crate crates[MAX_CRATES];

// Where log lines go (main.cpp's serial log, the host tests' stdout).
static LogFunction logFunction = nullptr;

// --- Log and radio

void SetLog(LogFunction log)
{
    logFunction = log;
}

void LogEvent(const char* format, ...)
{
    if (!logFunction)
        return;
    char line[160];
    int seconds = static_cast<int>(world.time);
    int used = snprintf(line, sizeof(line), "[%02d:%02d] ", seconds / 60, seconds % 60);
    va_list args;
    va_start(args, format);
    vsnprintf(line + used, sizeof(line) - used, format, args);
    va_end(args);
    logFunction(line);
}

void Radio(Speaker speaker, const char* text)
{
    if (world.titleMode || radioCount >= MAX_RADIO)
        return;
    RadioMessage& m = radio[radioCount++];
    m.speaker = speaker;
    m.text = text;
    m.time = 0.0f;
}

// A message coming on screen: the radio squelch, and the music ducks under it.
static void RadioStart()
{
    audio::Play(audio::Sfx::RADIO_BLIP, 1.0f);
    audio::DuckMusic(RADIO_TIME - 1.0f);
}

const RadioMessage* CurrentRadio()
{
    return radioCount > 0 && script.radioHold <= 0.0f ? &radio[0] : nullptr;
}

static void UpdateRadio(float dt)
{
    // Nothing over the intro's title card, and half a second after it.
    script.radioHold -= dt;
    if (radioCount == 0 || script.radioHold > 0.0f)
        return;
    if (radio[0].time == 0.0f)
        RadioStart();
    radio[0].time += dt;
    if (radio[0].time < RADIO_TIME)
        return;
    for (int i = 1; i < radioCount; i++)
        radio[i - 1] = radio[i];
    radioCount--;
}

// The pilot's cheer after a kill, at most one every few seconds.
static void Bark()
{
    static const char* const LINES[] = {
        "Splashed one!",
        "Banana split!",
        "Down he goes!",
        "Ook ook! Got him!",
        "Go bananas!",
    };
    if (script.barkTimer > 0.0f)
        return;
    script.barkTimer = BARK_GAP;
    audio::Play(audio::Sfx::MONKEY_CHEER, 0.9f);
    Radio(Speaker::RED, LINES[world.rng.Next() % 5]);
}

// --- Score

void AddScore(int points, const char* label)
{
    world.stats.score += points;
    if (!label)
        return;
    char text[48];
    snprintf(text, sizeof(text), "%s +%d", label, points);
    HudPopup(text, 0xFFF4E4C1);
}

// In a fight: an enemy plane (or the zeppelin's hull) within STUNT_FIGHT_RANGE, or enemy fire close by just now.
static bool InFight()
{
    if (world.shotAt < STUNT_SHOT_AT)
        return true;
    const Vec3& at = Player().flight.position;
    for (int i = 1; i < MAX_PLANES; i++) {
        const Plane& p = world.planes[i];
        if (p.active && p.alive && LengthSq(p.flight.position - at) < STUNT_FIGHT_RANGE * STUNT_FIGHT_RANGE)
            return true;
    }
    const Zeppelin& z = world.zeppelin;
    float hull = STUNT_FIGHT_RANGE + HULL_RADII.z;
    return z.state == ZeppelinState::CIRCLING && LengthSq(z.position - at) < hull * hull;
}

// Stunts score in a fight; a show-off stunt with nobody around gets a token, the same stunt again within 5 s nothing.
void OnStuntDone(Stunt stunt)
{
    world.stats.stunts++;
    float& last = script.lastStunt[static_cast<int>(stunt)];
    bool fight = InFight();
    LogEvent("stunt %s%s", StuntName(stunt), fight ? " in a fight" : "");
    if (world.time - last < STUNT_REPEAT)
        return;
    last = world.time;
    AddScore(fight ? SCORE_STUNT : SCORE_STUNT_CALM, StuntName(stunt));
}

// --- Rings

int RingCountTotal()
{
    return ringCount;
}

int RingsPassed()
{
    return world.stats.rings;
}

Vec3 RingWorldPosition(int index)
{
    return content::RingPosition(index);
}

int NextRing()
{
    for (int i = 0; i < ringCount; i++) {
        if (!ringPassed[i])
            return i;
    }
    return -1;
}

static void CheckRings()
{
    const Plane& p = Player();
    if (!p.alive)
        return;
    for (int i = 0; i < ringCount; i++) {
        if (ringPassed[i])
            continue;
        Vec3 center = content::RingPosition(i);
        if (LengthSq(p.flight.position - center) > 60.0f * 60.0f)
            continue;
        Quat inv = QuatConjugate(content::RingRotation(i));
        Vec3 a = Rotate(inv, p.prevPosition - center);
        Vec3 b = Rotate(inv, p.flight.position - center);
        if ((a.z > 0.0f) == (b.z > 0.0f))
            continue;
        float t = a.z / (a.z - b.z);
        Vec3 hit = Lerp(a, b, t);
        if (hit.x * hit.x + hit.y * hit.y > RING_RADIUS * RING_RADIUS)
            continue;
        ringPassed[i] = true;
        world.stats.rings++;
        script.lastRing = world.time;
        audio::Play(audio::Sfx::RING, 1.0f);
        AddScore(SCORE_RING, "RING");
        LogEvent("ring %d (%d/%d)", i, world.stats.rings, ringCount);
        if (world.stats.rings == ringCount) {
            AddScore(SCORE_ALL_RINGS, "ALL RINGS");
            Radio(Speaker::HARBOUR, "Every ring! What a show!");
        }
    }
}

void DrawRingsAndCrates()
{
    int next = NextRing();
    for (int i = 0; i < ringCount; i++) {
        render::Material m;
        m.texture = assets.propsTexture;
        m.specular = 0.6f;
        if (ringPassed[i]) {
            m.tint = 0xFF8A7A50;
        } else {
            m.tint = 0xFFFFD23F;
            float glow = i == next ? 0.35f + 0.25f * sinf(world.time * 6.0f) : 0.15f;
            m.emissive = Vec3(glow, glow * 0.8f, glow * 0.2f);
        }
        Mat4 model = MatTRS(content::RingPosition(i), content::RingRotation(i), Vec3(1, 1, 1));
        float distanceSq = LengthSq(content::RingPosition(i) - CurrentCamera().position);
        bool distant = distanceSq > RING_LOD_DISTANCE * RING_LOD_DISTANCE;
        render::DrawMesh(distant ? assets.ringDistant : assets.ring, model, m);
    }
    for (const Crate& c : crates) {
        if (!c.active)
            continue;
        render::Material m;
        m.texture = assets.propsTexture;
        m.tint = c.kind == CrateKind::REPAIR ? 0xFFFFFFFF : 0xFFFF9080;
        m.emissive = Vec3(0.15f, 0.12f, 0.05f);
        Quat rot = QuatAxisAngle(WORLD_UP, c.spin);
        render::DrawMesh(assets.crate, MatTRS(c.position, rot, Vec3(1.6f, 1.6f, 1.6f)), m);
    }
}

// --- Crates

static void DropCrate(const Vec3& at, bool certain)
{
    const Plane& player = Player();
    // A badly hurt player always gets a crate (a repair one) from the next kill.
    bool critical = player.alive && player.health < player.maxHealth * 0.25f;
    float chance = certain || critical ? 1.0f : 0.35f;
    if (world.rng.Uniform() > chance)
        return;
    for (Crate& c : crates) {
        if (c.active)
            continue;
        c.active = true;
        bool hurt = player.health < player.maxHealth * 0.6f;
        bool lowAmmo = player.rockets < 3;
        c.kind = hurt || (!lowAmmo && world.rng.Uniform() < 0.6f) ? CrateKind::REPAIR : CrateKind::ROCKETS;
        c.position = at;
        c.spin = 0.0f;
        LogEvent("crate dropped (%s)", c.kind == CrateKind::REPAIR ? "repair" : "rockets");
        HudCrateTip();
        return;
    }
}

static void UpdateCrates(float dt)
{
    Plane& player = Player();
    for (Crate& c : crates) {
        if (!c.active)
            continue;
        c.position.y -= 3.0f * dt;
        c.spin += dt;
        if (c.position.y < GroundHeight(c.position.x, c.position.z) + 1.0f) {
            c.active = false;
            continue;
        }
        if (!player.alive || LengthSq(player.flight.position - c.position) > CRATE_PICKUP * CRATE_PICKUP)
            continue;
        c.active = false;
        audio::Play(audio::Sfx::PICKUP, 1.0f);
        if (c.kind == CrateKind::REPAIR) {
            player.health = fminf(player.maxHealth, player.health + player.maxHealth * 0.35f);
            AddScore(SCORE_CRATE, "REPAIR");
        } else {
            player.rockets = player.rockets + 4 > PLAYER_ROCKETS ? PLAYER_ROCKETS : player.rockets + 4;
            AddScore(SCORE_CRATE, "ROCKETS");
        }
        LogEvent("crate picked up");
    }
}

// --- Phases

// The harbour's ground crew patches the player up between phases (fraction of full health).
static void Repair(float fraction)
{
    Plane& p = Player();
    if (!p.alive)
        return;
    p.health = fminf(p.maxHealth, p.health + p.maxHealth * fraction);
    p.rockets = p.rockets > PLAYER_ROCKETS / 2 ? p.rockets : PLAYER_ROCKETS / 2;
    HudPopup("PATCHED UP", 0xFFF4E4C1);
}

static void SetObjective(const char* text)
{
    snprintf(script.objective, sizeof(script.objective), "%s", text);
}

const char* ObjectiveText()
{
    return script.objective;
}

Phase CurrentPhase()
{
    return script.phase;
}

static void SetPhase(Phase phase)
{
    script.phase = phase;
    script.phaseTime = 0.0f;
    script.spawned = 0;
    script.spawnTimer = 0.0f;
    script.waveKills = 0;
}

static void UpdateWaveObjective()
{
    char text[64];
    const char* what = script.phase == Phase::WAVE3 ? "Defend the harbour" : "Down the raiders";
    snprintf(text, sizeof(text), "%s %d/%d", what, script.waveKills, script.waveSize);
    SetObjective(text);
}

// A pirate heading in from a point at the given bearing from the island (radians, 0 = north) and range.
static int SpawnFrom(PlaneKind kind, float bearing, float range, float altitude, const Vec3& toward)
{
    Vec3 at(sinf(bearing) * range, altitude, -cosf(bearing) * range);
    at.y = fmaxf(altitude, GroundHeight(at.x, at.z) + 150.0f);
    return SpawnPlane(kind, at, toward - at, kind == PlaneKind::ACE ? 72.0f : 64.0f);
}

// A wave 1 raider at a distance in a direction from the player, coming for him; it breaks away once.
static void SpawnWave1Raider(const Vec3& direction, float range)
{
    const Plane& player = Player();
    Vec3 at = player.flight.position + direction * range;
    at.y = fmaxf(220.0f, GroundHeight(at.x, at.z) + 150.0f);
    int index = SpawnPlane(PlaneKind::FIGHTER, at, player.flight.position - at, 64.0f);
    if (index <= 0)
        return;
    AiBrain& brain = world.planes[index].ai;
    brain.mode = AiMode::ATTACK;
    brain.evadeOnce = true;
}

static void StartWave1()
{
    SetPhase(Phase::WAVE1);
    script.waveSize = 4;
    UpdateWaveObjective();
    audio::SetMusic(audio::Music::COMBAT);
    LogEvent("wave 1");
    // The easy first kill: a raider that flies straight across the player's nose.
    const Plane& player = Player();
    Vec3 fwd = QuatForward(player.flight.rotation);
    Vec3 flat = Normalize(Vec3(fwd.x, 0.0f, fwd.z));
    Vec3 side = Cross(flat, WORLD_UP);
    Vec3 start = player.flight.position + flat * 650.0f + side * 380.0f;
    start.y = fmaxf(start.y, GroundHeight(start.x, start.z) + 150.0f);
    int easy = SpawnPlane(PlaneKind::FIGHTER, start, -side, 60.0f);
    if (easy > 0) {
        Plane& p = world.planes[easy];
        p.ai.mode = AiMode::RAID;
        p.ai.passive = true;
        p.ai.skill = 0.1f;
        p.ai.waypoint = start - side * 3000.0f;
        p.health = p.maxHealth * 0.6f;
    }
    // Its wingman ahead and to the right, in view, coming for the player. The second pair comes later
    // (UpdateWave1).
    SpawnWave1Raider(flat * cosf(45.0f * DEG) + side * sinf(45.0f * DEG), WAVE1_RANGE);
    script.spawned = 2;
    Radio(Speaker::HARBOUR, "One crossing your nose, Red! Fire!");
}

// Wave 1's second pair: after the first kill, but not before WAVE1_SECOND_PAIR seconds, from the player's left.
static void UpdateWave1()
{
    if (script.spawned >= script.waveSize || script.waveKills == 0 || script.phaseTime < WAVE1_SECOND_PAIR)
        return;
    const Plane& player = Player();
    Vec3 fwd = QuatForward(player.flight.rotation);
    Vec3 flat = Normalize(Vec3(fwd.x, 0.0f, fwd.z));
    Vec3 side = Cross(flat, WORLD_UP);
    for (int i = 0; i < 2; i++) {
        float angle = (i == 0 ? -70.0f : -100.0f) * DEG;
        SpawnWave1Raider(flat * cosf(angle) + side * sinf(angle), WAVE1_SECOND_RANGE + i * 120.0f);
    }
    script.spawned = 4;
    Radio(Speaker::HARBOUR, "Two more coming in on your left!");
    LogEvent("wave 1: second pair");
}

static void UpdateWave2(float dt)
{
    static const char* const ACE_NAMES[2] = {"KNUCKLES", "BIG MO"};
    script.spawnTimer -= dt;
    if (script.spawned >= script.waveSize || script.spawnTimer > 0.0f)
        return;
    Vec3 harbour = assets.harbour + Vec3(0.0f, 200.0f, 0.0f);
    Vec3 base = assets.pirateBase;
    float bearing = atan2f(base.x, -base.z);
    if (script.spawned < 3) {
        for (int i = 0; i < 3; i++)
            SpawnFrom(PlaneKind::FIGHTER, bearing + (i - 1) * 0.25f, 3000.0f, 400.0f, harbour);
        script.spawned = 3;
        script.spawnTimer = 9.0f;
        Radio(Speaker::HARBOUR, "More raiders from the tower, north-east!");
        return;
    }
    if (script.spawned < 5) {
        for (int i = 0; i < 2; i++) {
            float side = i ? 0.2f : -0.2f;
            int index = SpawnFrom(PlaneKind::ACE, bearing + side, 2800.0f, 500.0f, Player().flight.position);
            if (index > 0)
                world.planes[index].name = ACE_NAMES[i];
        }
        script.spawned = 5;
        script.spawnTimer = 10.0f;
        audio::Play(audio::Sfx::ENEMY_TAUNT, 1.0f);
        Radio(Speaker::PIRATE, "Knuckles and Big Mo here. Say your prayers, monkey!");
        Radio(Speaker::HARBOUR, "Aces, Red! Purple tails. Watch them!");
        LogEvent("aces arrive");
        return;
    }
    SpawnFrom(PlaneKind::FIGHTER, bearing + 0.5f, 3000.0f, 350.0f, harbour);
    script.spawned = 6;
}

// Wave 3: a radio call, then raiders from the sea that go for the harbour itself.
static void StartWave3()
{
    SetPhase(Phase::WAVE3);
    script.waveSize = WAVE3_SIZE;
    UpdateWaveObjective();
    Radio(Speaker::HARBOUR, "Raiders coming in low for the harbour!");
    LogEvent("wave 3");
}

static void SpawnWave3()
{
    Vec3 harbour = assets.harbour + Vec3(0.0f, 140.0f, 0.0f);
    // From the open sea south of the harbour, so they fly straight at it.
    float bearing = atan2f(assets.harbour.x, -assets.harbour.z);
    for (int i = 0; i < WAVE3_SIZE - 1; i++) {
        PlaneKind kind = i == 2 ? PlaneKind::ACE : PlaneKind::FIGHTER;
        float range = Length(Vec3(assets.harbour.x, 0.0f, assets.harbour.z)) + 1800.0f + i * 120.0f;
        int index = SpawnFrom(kind, bearing + (i - 1.5f) * 0.12f, range, 250.0f, harbour);
        if (index <= 0)
            continue;
        Plane& p = world.planes[index];
        p.ai.mode = AiMode::RAID;
        p.ai.raidAngle = i * (2.0f * PI / WAVE3_SIZE);
        p.ai.raidTime = WAVE3_RAID_TIME;
        if (kind == PlaneKind::ACE)
            p.name = "SCAR";
    }
    script.spawned = WAVE3_SIZE - 1;
    audio::Play(audio::Sfx::ENEMY_TAUNT, 1.0f, 0.0f, 0.9f);
    Radio(Speaker::PIRATE, "Scar here. Burn their banana boats!");
    UpdateWaveObjective();
}

// Wave 3's last raider, once the others are down: in low along the waterfront and circling the wharf until shot
// down, so it falls into the harbour (FallByWharf).
static void SpawnStrafer()
{
    Vec3 sea = Normalize(Vec3(assets.harbour.x - assets.wharf.x, 0.0f, assets.harbour.z - assets.wharf.z));
    Vec3 along(-sea.z, 0.0f, sea.x);
    Vec3 at = assets.wharf - along * STRAFER_START;
    at.y = GroundHeight(at.x, at.z) + 120.0f;
    int index = SpawnPlane(PlaneKind::FIGHTER, at, along, 64.0f);
    script.spawned = WAVE3_SIZE;
    if (index <= 0)
        return;
    AiBrain& brain = world.planes[index].ai;
    brain.mode = AiMode::RAID;
    brain.strafer = true;
    brain.raidAngle = atan2f(-along.z, -along.x);
    Radio(Speaker::HARBOUR, "Another one, strafing the wharf! Get him, Red!");
    LogEvent("wave 3: a raider strafes the wharf");
}

static void StartBoss()
{
    SetPhase(Phase::BOSS);
    SpawnZeppelin();
    Repair(1.0f);
    world.revealTimer = EMERGE_TIME;
    audio::SetMusic(audio::Music::BOSS);
    audio::Play(audio::Sfx::ENEMY_TAUNT, 1.0f, 0.0f, 0.8f);
    Radio(Speaker::HARBOUR, "There she is - the Silverback!");
    Radio(Speaker::PIRATE, "Meet the Silverback, little monkey!");
    Radio(Speaker::HARBOUR, "Her four engines, Red! Hit the engines!");
    SetObjective("Destroy the engines 0/4");
    LogEvent("boss: zeppelin arrives");
}

static void UpdatePhase(float dt)
{
    script.phaseTime += dt;
    switch (script.phase) {
    case Phase::INTRO:
        if (script.phaseTime >= INTRO_TIME)
            StartWave1();
        break;
    case Phase::WAVE1: {
        UpdateWave1();
        if (CountEnemies() == 0 && !script.breather && script.spawned >= script.waveSize) {
            script.breather = true;
            script.phaseTime = 0.0f;
            SetObjective("Fly the canyon rings");
            Radio(Speaker::HARBOUR, "Harbour's safe! Try the canyon rings.");
            LogEvent("wave 1 cleared");
        }
        bool ringRun = world.time - script.lastRing < 6.0f && script.phaseTime < BREATHER_MAX;
        if (script.breather && script.phaseTime > BREATHER_TIME && !ringRun) {
            SetPhase(Phase::WAVE2);
            script.waveSize = 6;
            Repair(0.5f);
            UpdateWaveObjective();
            LogEvent("wave 2");
        }
        break;
    }
    case Phase::WAVE2:
        UpdateWave2(dt);
        if (script.spawned >= script.waveSize && CountEnemies() == 0) {
            LogEvent("wave 2 cleared");
            StartWave3();
        }
        break;
    case Phase::WAVE3:
        if (script.phaseTime > WAVE3_DELAY && script.spawned == 0)
            SpawnWave3();
        if (script.spawned == WAVE3_SIZE - 1 && CountEnemies() == 0)
            SpawnStrafer();
        if (script.spawned >= WAVE3_SIZE && CountEnemies() == 0) {
            if (!script.zeppelinSpawned) {
                script.zeppelinSpawned = true;
                script.endTimer = 0.0f;
                SetObjective("");
                Radio(Speaker::HARBOUR, "Big shape in the clouds, north-east...");
                LogEvent("wave 3 cleared");
            }
            script.endTimer += dt;
            if (script.endTimer > BOSS_DELAY)
                StartBoss();
        }
        break;
    case Phase::BOSS: {
        int down = world.zeppelin.engineCount - EnginesLeft();
        char text[64];
        snprintf(text, sizeof(text), "Destroy the engines %d/%d", down, world.zeppelin.engineCount);
        SetObjective(text);
        if (world.zeppelin.state == ZeppelinState::CRASHING) {
            SetPhase(Phase::CRASH);
            world.cinematic = true;
            SetObjective("");
            Radio(Speaker::HARBOUR, "She's going down! Get clear, Red!");
        }
        break;
    }
    case Phase::CRASH:
        break;
    case Phase::VICTORY:
        if (script.endTimer < VICTORY_BANNER && script.endTimer + dt >= VICTORY_BANNER)
            HudBanner("VICTORY!", 0xFFFFD23F, VICTORY_HOLD);
        script.endTimer += dt;
        world.timeScale = 0.5f;
        break;
    case Phase::DEFEAT:
        script.endTimer += dt;
        break;
    }
}

// --- Events

// Once per mission, a wave 3 raider shot down near the wharf falls into the sea just off it, in view of the
// chase camera: the harbour is part of the fight.
static void FallByWharf(Plane& p)
{
    if (script.phase != Phase::WAVE3 || script.wharfSplash)
        return;
    Vec3 sea = Normalize(Vec3(assets.harbour.x - assets.wharf.x, 0.0f, assets.harbour.z - assets.wharf.z));
    Vec3 target = assets.wharf + sea * 55.0f;
    target.y = 0.0f;
    Vec3 to = target - p.flight.position;
    if (Length(Vec3(to.x, 0.0f, to.z)) > WHARF_FALL_RANGE)
        return;
    script.wharfSplash = true;
    p.steerFall = true;
    p.fallTarget = target;
    LogEvent("raider falls by the wharf");
}

void OnPlaneDestroyed(int index, bool byPlayer)
{
    Plane& p = world.planes[index];
    FallByWharf(p);
    LogEvent("%s %d down%s", p.kind == PlaneKind::ACE ? "ace" : "fighter", index, byPlayer ? " (player)" : "");
    if (script.phase == Phase::WAVE1 || script.phase == Phase::WAVE2 || script.phase == Phase::WAVE3) {
        script.waveKills++;
        UpdateWaveObjective();
    }
    if (!byPlayer)
        return;
    world.stats.kills++;
    if (p.kind == PlaneKind::ACE) {
        world.stats.aces++;
        AddScore(SCORE_ACE, p.name ? p.name : "ACE");
        audio::Play(audio::Sfx::ENEMY_TAUNT, 0.8f, 0.0f, 1.3f);
    } else {
        AddScore(SCORE_FIGHTER, "KILL");
    }
    HudBanner("SPLASHED!", 0xFFF4E4C1);
    audio::Play(audio::Sfx::KILL_STING);
    Rumble(0.6f, 0.6f, 300);
    float distance = Length(p.flight.position - Player().flight.position);
    if (distance < 300.0f)
        HudFlash(0.25f);
    Bark();
    DropCrate(p.flight.position, p.kind == PlaneKind::ACE);
}

// During the boss: 800 m off the zeppelin's beam at its height, heading straight at it, so the fight goes on.
static bool RespawnAtZeppelin()
{
    const Zeppelin& z = world.zeppelin;
    if (script.phase != Phase::BOSS || z.state != ZeppelinState::CIRCLING)
        return false;
    Vec3 side = QuatRight(z.rotation);
    side = Normalize(Vec3(side.x, 0.0f, side.z));
    // The side away from the volcano: open air.
    Vec3 out = Normalize(Vec3(z.position.x - z.circleCenter.x, 0.0f, z.position.z - z.circleCenter.z));
    if (Dot(side, out) < 0.0f)
        side = -side;
    Vec3 start = z.position + side * BOSS_RESPAWN_RANGE;
    start.y = fmaxf(z.position.y, GroundHeight(start.x, start.z) + 150.0f);
    SpawnPlane(PlaneKind::PLAYER, start, z.position - start, PLAYER_FLIGHT.cruiseSpeed);
    return true;
}

static void RespawnPlayer()
{
    if (RespawnAtZeppelin()) {
        Player().invulnerable = INVULNERABLE_TIME;
        world.target = TargetRef();
        LogEvent("player respawned by the zeppelin, %d lives", world.lives);
        return;
    }
    Vec3 harbour = assets.harbour;
    Vec3 inland = Normalize(Vec3(assets.volcano.x - harbour.x, 0.0f, assets.volcano.z - harbour.z));
    if (LengthSq(inland) < 0.01f)
        inland = Vec3(0.0f, 0.0f, -1.0f);
    // Offshore of the harbour, flying along the coast, above anything in the next two kilometres.
    Vec3 along = Cross(inland, WORLD_UP);
    Vec3 start = harbour - inland * 500.0f;
    float highest = 0.0f;
    for (int i = 0; i <= 10; i++) {
        Vec3 at = start + along * (i * 200.0f);
        highest = fmaxf(highest, GroundHeight(at.x, at.z));
    }
    start.y = fmaxf(320.0f, highest + 200.0f);
    SpawnPlane(PlaneKind::PLAYER, start, along, PLAYER_FLIGHT.cruiseSpeed);
    Player().invulnerable = INVULNERABLE_TIME;
    world.target = TargetRef();
    LogEvent("player respawned, %d lives", world.lives);
}

void OnPlayerDown()
{
    if (world.titleMode)
        return;
    world.lives--;
    LogEvent("player down, %d lives left", world.lives);
    audio::Play(audio::Sfx::MONKEY_HURT, 1.0f, 0.0f, 0.8f);
    Rumble(1.0f, 1.0f, 600);
    if (world.lives > 0) {
        world.respawnTimer = RESPAWN_TIME;
        Radio(Speaker::HARBOUR, "Red's hit! Bail out! ...Spare plane coming up.");
        return;
    }
    SetPhase(Phase::DEFEAT);
    script.endTimer = 0.0f;
    audio::SetMusic(audio::Music::NONE);
    audio::SetMusic(audio::Music::DEFEAT);
    Radio(Speaker::PIRATE, "Ha! Monkey goes splash!");
}

void OnZeppelinSunk()
{
    AddScore(SCORE_ZEPPELIN, "SILVERBACK");
    SetPhase(Phase::VICTORY);
    script.endTimer = 0.0f;
    audio::SetMusic(audio::Music::NONE);
    audio::SetMusic(audio::Music::VICTORY);
    // The title theme follows the fanfare at once (fading in under its last chord), so the results never start
    // in silence.
    audio::SetMusicNext(audio::Music::TITLE);
    audio::Play(audio::Sfx::MONKEY_CHEER, 1.0f);
    Radio(Speaker::HARBOUR, "The Silverback is down! Isla Platano is free!");
    LogEvent("zeppelin sunk: victory");
}

// --- Mission

void ResetScript()
{
    script = Script();
    radioCount = 0;
    ringCount = content::RingCount();
    if (ringCount > MAX_RINGS)
        ringCount = MAX_RINGS;
    for (bool& passed : ringPassed)
        passed = false;
    for (Crate& c : crates)
        c.active = false;
    world.lives = 3;
    // Start over the quay, 15 m above the roofs, flying along the waterfront toward the pier (cranes, steamer); the
    // camera watches from 30 m over the wharf's far end with the village beside it. Past the pier the plane turns
    // out over the bay toward the lighthouse at the end of the mole; wave 1 appears around it when the intro ends.
    Vec3 village = content::HarbourVillagePosition();
    Vec3 bay = content::HarbourPosition();
    Vec3 sea = Normalize(Vec3(bay.x - village.x, 0.0f, bay.z - village.z));
    Vec3 shore = village + sea * 60.0f;
    Vec3 lamp = assets.lighthouseLamp;
    Vec3 along(-sea.z, 0.0f, sea.x);
    if (Dot(along, lamp - shore) < 0.0f)
        along = -along;
    Vec3 start = shore - along * INTRO_RUN - sea * 2.0f;
    float highest = 0.0f;
    for (int i = 0; i <= 8; i++) {
        Vec3 at = start + along * (i * 40.0f);
        highest = fmaxf(highest, GroundHeight(at.x, at.z));
    }
    start.y = highest + INTRO_HEIGHT;
    SpawnPlane(PlaneKind::PLAYER, start, along, PLAYER_FLIGHT.cruiseSpeed);
    world.introCamera = shore + along * INTRO_CAMERA + sea * 15.0f;
    world.introCamera.y = GroundHeight(world.introCamera.x, world.introCamera.z) + 30.0f;
    world.introLook = shore - along * 60.0f - sea * 50.0f;
    world.introLook.y = GroundHeight(world.introLook.x, world.introLook.z) + 6.0f;
    world.introExit = Vec3(lamp.x, lamp.y + 40.0f, lamp.z);
    world.introTimer = INTRO_TIME;
    SetObjective("");
    Radio(Speaker::HARBOUR, "Pirates over the harbour, Red! Get 'em!");
    audio::SetMusic(audio::Music::COMBAT);
    LogEvent("mission start, %d rings", ringCount);
}

void UpdateScript(float dt)
{
    script.barkTimer -= dt;
    UpdatePhase(dt);
    UpdateRadio(dt);
    CheckRings();
    UpdateCrates(dt);

    if (world.respawnTimer > 0.0f && script.phase != Phase::DEFEAT) {
        world.respawnTimer -= dt;
        if (world.respawnTimer <= 0.0f)
            RespawnPlayer();
    }
}

Outcome MissionOutcome()
{
    if (script.phase == Phase::VICTORY && script.endTimer > VICTORY_TIME)
        return Outcome::VICTORY;
    if (script.phase == Phase::DEFEAT && script.endTimer > DEFEAT_TIME)
        return Outcome::DEFEAT;
    return Outcome::PLAYING;
}

const char* RankName(int rank)
{
    static const char* const NAMES[4] = {"CHIMP", "MONKEY", "BABOON", "SILVERBACK"};
    return NAMES[rank < 0 ? 0 : (rank > 3 ? 3 : rank)];
}

Results MissionResults()
{
    Results r;
    const Stats& s = world.stats;
    r.kills = s.kills;
    r.aces = s.aces;
    r.rings = s.rings;
    r.ringCount = ringCount;
    r.stunts = s.stunts;
    r.shotsFired = s.shotsFired;
    r.shotsHit = s.shotsHit;
    r.livesLeft = world.lives;
    r.time = world.time;
    bool won = script.phase == Phase::VICTORY;
    r.timeBonus = won ? static_cast<int>(fmaxf(0.0f, 720.0f - world.time) * 5.0f) + world.lives * 1000 : 0;
    r.score = s.score + r.timeBonus;
    static const int RANK_SCORES[3] = {12000, 20000, 30000};
    r.rank = 0;
    for (int i = 0; i < 3; i++) {
        if (won && r.score >= RANK_SCORES[i])
            r.rank = i + 1;
    }
    return r;
}

} // namespace game
