// The game folder's shared state and the functions its files offer each other: the flight model, planes, weapons,
// effects, the zeppelin, the mission script, the camera, the HUD and the GPU resources (docs/design.md §2, §8).
#pragma once

#include "audio/audio.h"
#include "content/content.h"
#include "core/vmath.h"
#include "game/game.h"
#include "platform/input.h"
#include "render/render.h"

namespace game {

// --- World limits

constexpr float FLIGHT_HALF_SIZE = 5000.0f; // the flight area is a 10 km square around the island
constexpr float WARN_HALF_SIZE = 4500.0f;   // past this the radio calls you back
constexpr float CEILING = 2000.0f;
constexpr float GRAVITY = 9.81f;
constexpr Vec3 WORLD_UP = Vec3(0.0f, 1.0f, 0.0f);

// --- Flight model (flight.cpp)

enum class Stunt {
    NONE,
    LOOP,
    IMMELMANN,
    ROLL_LEFT,
    ROLL_RIGHT,
};

// What the pilot (the pad or the AI) asks of the plane this step.
struct FlightControls {
    float pitch = 0.0f;        // -1 nose down .. 1 nose up
    float turn = 0.0f;         // -1 left .. 1 right: banks and turns
    float rudder = 0.0f;       // -1 .. 1 flat yaw
    float throttle = 0.0f;     // -1 brake .. 0 cruise .. 1 boost
    Stunt stunt = Stunt::NONE; // starts this stunt (ignored while one runs)
};

// How a type of plane flies. Rates in radians per second, speeds in m/s.
struct FlightParams {
    float cruiseSpeed;
    float boostSpeed;
    float brakeSpeed;
    float pitchRate;
    float turnRate;      // heading change at full bank
    float rollRate;
    float yawRate;       // rudder
    float maxBank;
    float boostDrain;    // meter used per second of boost
    float meterRecharge; // meter regained per second
};

// Boost and stunts share one recharging meter (0..1); each stunt costs a slice of it.
constexpr float STUNT_COST = 0.3f;

extern const FlightParams PLAYER_FLIGHT;
extern const FlightParams FIGHTER_FLIGHT;
extern const FlightParams ACE_FLIGHT;

constexpr float MIN_SPEED = 42.0f;
constexpr float MAX_SPEED = 140.0f;

struct FlightState {
    Vec3 position;
    Vec3 velocity;
    Quat rotation;
    float speed = 0.0f;
    float meter = 1.0f;     // boost and stunt meter 0..1
    float meterDelay = 0.0f; // seconds until the meter recharges again
    bool boosting = false;
    float throttle = 0.5f;  // engine output 0..1 (sound, propeller)
    float turnLoad = 0.0f;  // load factor in g-equivalents (vapour trails above 3.5)
    float rollRate = 0.0f;  // current roll rate (rad/s), eased toward the commanded one
    float bankHold = 0.0f;  // seconds since the turn stick was released
    Stunt stunt = Stunt::NONE;
    float stuntTime = 0.0f;  // seconds into the stunt
    float stuntAngle = 0.0f; // radians turned in the stunt's current phase
    int stuntPhase = 0;
    int stuntsDone = 0;      // stunts completed (scoring)
};

void ResetFlight(FlightState& state, const Vec3& position, const Quat& rotation, float speed);
void UpdateFlight(FlightState& state, const FlightParams& params, const FlightControls& controls, float dt);
// Bank angle in radians: positive with the right wing down, 0 level, +-PI upside down.
float BankAngle(const Quat& rotation);
// True during a barrel roll: bullets miss the plane.
bool StuntDodging(const FlightState& state);
const char* StuntName(Stunt stunt);

// --- Planes (planes.cpp, ai.cpp, player.cpp)

enum class PlaneKind {
    PLAYER,
    FIGHTER,
    ACE,
};

enum class AiMode {
    RAID,   // flies to the harbour and circles it
    ATTACK, // lead pursuit of the player
    EVADE,  // break turn or stunt away from the player's guns
    EXTEND, // flies away to get room for another pass
    AVOID,  // pulls up from the ground or the zeppelin, or turns back into the flight area
};

struct AiBrain {
    AiMode mode = AiMode::ATTACK;
    float modeTime = 0.0f;     // seconds in the current mode
    float thinkTimer = 0.0f;   // until the next re-evaluation
    float aimTimer = 0.0f;     // until the next aim error is rolled
    Vec3 aimError;             // unit offset direction of this pass's aim error
    float burstTimer = 0.0f;   // > 0: firing; < 0: pausing between bursts
    float evadeCooldown = 0.0f;
    float evadeTurn = 0.0f;    // break-turn direction
    Vec3 extendDirection;
    float skill = 0.5f;        // 0 poor .. 1 sharp
    float raidAngle = 0.0f;    // position on the circle around the harbour
    float raidTime = 10.0f;    // seconds of raiding before it hunts the player anyway
    bool passive = false;      // flies straight on (the easy first kill) until hit or attacked
    bool evadeOnce = false;    // wave 1: breaks away once when the player is on its tail, then commits
    bool strafer = false;      // wave 3's last raider: circles low over the wharf until shot down
    Vec3 waypoint;             // where a passive or raiding plane heads
};

constexpr int MAX_PLANES = 16;
constexpr int PLAYER_PLANE = 0;

struct Plane {
    bool active = false;     // in use (alive or falling)
    bool alive = false;      // flying; false while falling after being shot down
    PlaneKind kind = PlaneKind::FIGHTER;
    const FlightParams* params = nullptr;
    FlightState flight;
    Vec3 prevPosition;       // state before the last step, for drawing between steps
    Quat prevRotation;
    FlightControls controls;
    bool trigger = false;    // guns held this step
    bool rocketTrigger = false;
    float health = 0.0f;
    float maxHealth = 0.0f;
    float hitFlash = 0.0f;   // 0..1 fades after a hit
    float invulnerable = 0.0f;
    float gunHeat = 0.0f;    // 0..1
    float overheat = 0.0f;   // seconds left of an overheat lock-out
    float gunCooldown = 0.0f;
    int gunSide = 0;
    int rockets = 0;
    float rocketCooldown = 0.0f;
    float smokeTimer = 0.0f;
    float fireFlare = 0.0f;  // seconds left of a flare-up of the engine fire after a hit
    float vapourTimer = 0.0f; // until the next wingtip vapour puff
    float propAngle = 0.0f;
    float dyingTime = 0.0f;  // seconds since shot down
    float dyingSpin = 0.0f;  // roll rate while falling
    float lastHit = 99.0f;   // seconds since last hit
    bool escort = false;     // launched by the zeppelin
    bool steerFall = false;  // shot down: falls toward fallTarget (the wave 3 raider that lands by the wharf)
    Vec3 fallTarget;
    const char* name = nullptr; // aces' names, shown on the target bracket
    AiBrain ai;
};

void ClearPlanes();
// Puts a plane in the air. Returns its index, or -1 when all slots are used.
int SpawnPlane(PlaneKind kind, const Vec3& position, const Vec3& direction, float speed);
void UpdatePlanes(float dt);
void DamagePlane(int index, float amount, bool byPlayer, const Vec3& at);
int CountEnemies();
// Dims the plane engine voices (paused game, menus).
void SilenceEngines();

void UpdateAi(int index, float dt);
// Steers the plane's controls toward a world point; aggression 0..1 scales how hard it turns.
void SteerToward(const Plane& plane, const Vec3& point, float aggression, FlightControls& controls);
// The point to aim at to hit a target moving with velocity from shooter, with bullets of bulletSpeed.
Vec3 LeadPoint(const Vec3& shooter, const Vec3& shooterVelocity, const Vec3& target, const Vec3& targetVelocity,
               float bulletSpeed);

// Turns this step's pad state into the player's flight controls and actions.
void UpdatePlayerInput(const input::State& pad, float dt);

// --- Targets (planes and the zeppelin's engines and turrets)

enum class TargetKind {
    NONE,
    PLANE,
    ENGINE,
    TURRET,
};

struct TargetRef {
    TargetKind kind = TargetKind::NONE;
    int index = 0;
};

bool TargetValid(const TargetRef& target);
Vec3 TargetPosition(const TargetRef& target);
Vec3 TargetVelocity(const TargetRef& target);
float TargetHealth(const TargetRef& target); // 0..1
float TargetRadius(const TargetRef& target);
// The best target in front of the player after the current one (B cycles).
TargetRef NextTarget(const TargetRef& current);

// --- Weapons (weapons.cpp)

constexpr float BULLET_SPEED = 600.0f;
constexpr float BULLET_LIFE = 0.65f; // with the plane's speed: an effective range of about 450 m
constexpr int PLAYER_ROCKETS = 8;

void ClearProjectiles();
// Gun heat, fire rate and muzzle; fires when the plane's trigger is held.
void UpdateGuns(int index, float dt);
bool FireRocket(int index, const TargetRef& target);
// A tracer from a zeppelin turret.
void FireTurretBullet(const Vec3& from, const Vec3& velocity);
// A flak shell that bursts after fuse seconds.
void FireFlak(const Vec3& from, const Vec3& velocity, float fuse);
void UpdateProjectiles(float dt);
void DrawProjectiles();

// --- Effects (effects.cpp)

void ClearEffects();
void UpdateEffects(float dt);
void DrawEffects(); // alpha particles first, then additive ones
void Explosion(const Vec3& position, float size, const Vec3& velocity);
void SmokePuff(const Vec3& position, const Vec3& velocity, float size, float darkness);
void FirePuff(const Vec3& position, const Vec3& velocity, float size);
void Sparks(const Vec3& position, const Vec3& normal, int count);
void MuzzleFlash(const Vec3& position, const Vec3& velocity);
void Splash(const Vec3& position, float size);
void FlakBurst(const Vec3& position);
void Spray(const Vec3& position, const Vec3& velocity);
// Damage smoke behind a plane: 3 m puffs growing to 10 m over 2 s; black and to 12 m when burning.
void TrailPuff(const Vec3& position, const Vec3& velocity, bool burning);
// One puff of a falling zeppelin piece's trail: thick black smoke, 6 m growing to 18 m.
void PieceSmoke(const Vec3& position);
// The zeppelin hitting the sea: a water column up to 80 m with a fire blast inside, a spray ring 120 m across,
// lingering mist and foam.
void ZeppelinSplash(const Vec3& position);
void SmokeColumnPuff(const Vec3& position, const Vec3& velocity);
// A puff of coloured wingtip vapour (pirates' hard turns, so they read at range), lasting 1.5 s.
void Vapour(const Vec3& position, uint32_t color);
// A short-lived flame that moves with a burning engine (size in metres).
void EngineFire(const Vec3& position, const Vec3& velocity, float size);
void CloudWisp(const Vec3& position, const Vec3& velocity);
void Glow(const Vec3& position, float size, uint32_t color);

// --- Zeppelin (zeppelin.cpp)

constexpr int MAX_TURRETS = 8;
constexpr int MAX_ENGINES = 4;
constexpr int MAX_HULL_SECTIONS = 12;

enum class ZeppelinState {
    ABSENT,
    ARRIVING,
    CIRCLING,
    CRASHING,
    SUNK,
};

struct Zeppelin {
    ZeppelinState state = ZeppelinState::ABSENT;
    Vec3 position;
    Vec3 velocity;
    Quat rotation;
    Vec3 prevPosition;         // state before the last step, for drawing between steps
    Quat prevRotation;
    Vec3 circleCenter;
    float circleRadius = 0.0f;
    float altitude = 0.0f;     // cruising height of the circle
    float heading = 0.0f;      // radians, 0 = flying toward -Z
    float circleAngle = 0.0f;
    float pitch = 0.0f;        // nose-down tilt while crashing
    float roll = 0.0f;
    float crashTime = 0.0f;
    float explosionTimer = 0.0f;
    float burnTimer = 0.0f;    // until the next explosion along the keel in the death dive
    float launchTimer = 0.0f;
    int launched = 0;          // escorts launched so far
    float emergeTime = -1.0f;  // >= 0: seconds into the arrival cut, sliding out from behind the pirate tower
    Vec3 arrivePosition;       // where her arrival course starts after that cut
    bool lastStand = false;    // one engine left: she speeds up, the last engine glows, two aces are out
    int lastAces[2] = {-1, -1}; // those aces' plane slots
    bool armourOff = false;    // both aces are down: the last engine can be hit
    float lastEngineDown = -99.0f; // mission time of the last engine kill
    int turretCount = 0;
    int engineCount = 0;
    float turretHealth[MAX_TURRETS];
    float turretCooldown[MAX_TURRETS];
    float turretYaw[MAX_TURRETS];
    int turretBurst[MAX_TURRETS];
    float flakCooldown[MAX_TURRETS];
    float engineHealth[MAX_ENGINES];
    float engineFlash[MAX_ENGINES];
    float turretFlash[MAX_TURRETS];
    float propAngle = 0.0f;
    float burnAt[MAX_HULL_SECTIONS] = {}; // crash time at which each hull section starts to burn through
    int burnOrder = 0;         // which of the two burn orders (outward from the front or the rear engines)
    Vec3 crashStart;           // where the death dive began and where it ends (open sea)
    Vec3 crashTarget;
    bool engineGone[MAX_ENGINES] = {}; // engine pods that have broken off and fall on their own
};

constexpr float ENGINE_HEALTH = 152.0f;
constexpr float TURRET_HEALTH = 45.0f;
// Hull as an ellipsoid (half-sizes in local metres), for collisions and bullet hits.
constexpr Vec3 HULL_RADII = Vec3(22.0f, 22.0f, 92.0f);

void ClearZeppelin();
// Builds the hull skins for each stage of the burn-away (called once while loading, after the section centres).
void BuildZeppelinStages();
void ClearZeppelinSound();
void SpawnZeppelin();
void UpdateZeppelin(float dt);
void DrawZeppelin();
Vec3 EngineWorldPosition(int index);
Vec3 TurretWorldPosition(int index);
void DamageEngine(int index, float amount, const Vec3& at);
void DamageTurret(int index, float amount, const Vec3& at);
int EnginesLeft();
// The last engine is armoured (shots spark off) until the two aces launched for it are down.
bool LastEngineArmoured();
// True when the point is inside the hull ellipsoid scaled by margin.
bool InsideHull(const Vec3& point, float margin);

// --- Mission (mission.cpp): phases, waves, rings, crates, score, radio

enum class Phase {
    INTRO,
    WAVE1,
    WAVE2,
    WAVE3,  // raiders go for the harbour
    BOSS,
    CRASH,
    VICTORY,
    DEFEAT,
};

enum class Speaker {
    RED,     // the player
    HARBOUR, // the harbourmaster on the radio
    PIRATE,  // the Silverback Syndicate
};

struct RadioMessage {
    Speaker speaker = Speaker::HARBOUR;
    const char* text = nullptr;
    float time = 0.0f; // seconds shown so far
};

struct Stats {
    int score = 0;
    int kills = 0;
    int aces = 0;
    int rings = 0;
    int stunts = 0;
    int shotsFired = 0;
    int shotsHit = 0;
};

void ResetScript();
void UpdateScript(float dt);
Phase CurrentPhase();
const char* ObjectiveText();
const RadioMessage* CurrentRadio();
void Radio(Speaker speaker, const char* text);
void AddScore(int points, const char* label);
void OnPlaneDestroyed(int index, bool byPlayer);
void OnPlayerDown();
void OnZeppelinSunk();
void OnStuntDone(Stunt stunt);
// Logs one formatted line through the game's log function, prefixed with the mission time.
void LogEvent(const char* format, ...);
void DrawRingsAndCrates();
int RingCountTotal();
int RingsPassed();
// Index of the next ring to fly through, or -1.
int NextRing();
Vec3 RingWorldPosition(int index);

// --- Camera (camera.cpp)

enum class CameraMode {
    CHASE,
    CLOSE,
};

void ResetCamera();
void UpdateCamera(float dt);
void CameraShake(float amount);
const render::Camera& CurrentCamera();

// --- HUD (hud.cpp)

void ResetHud();
void UpdateHud(float dt);
void DrawHud();
void HudHitMarker();
void HudDamage(float amount);
void HudPopup(const char* text, uint32_t color);
// Big centred text that scales in ("SPLASHED!") and stays for the given seconds.
void HudBanner(const char* text, uint32_t color, float seconds = 1.0f);
// Full-screen white flash (near explosions), 0..1.
void HudFlash(float amount);
// The boost bar flashes red: boost or a stunt was refused for want of meter.
void HudMeterRefused();
// First-mission tips, each shown once per power-on at its moment (the HUD notices the others itself).
void HudCrateTip();
// Text with an ink drop shadow at an integer scale (also used by the menus).
void HudText(float x, float y, int scale, uint32_t color, const char* text);
void HudTextCentered(float centerX, float y, int scale, uint32_t color, const char* text);
// The controller diagram and the button glyphs (controls.cpp): made once at start-up.
void CreateControlsArt();
// A coloured Xbox face button ('A' green, 'B' red, 'X' blue, 'Y' yellow) with its letter, centred at (cx, cy).
void DrawButtonGlyph(char button, float cx, float cy, float size);
// The controls diagram: the controller in the middle, labels to both sides, within a band of the screen.
void DrawControls(float top, float height);
// Draws the HUD's own art texture (dials, radar, reticle, portraits); called once at start-up.
void CreateHudArt();

// --- World drawing (draw.cpp)

void DrawWorld(const render::Camera& camera);
render::Environment WorldEnvironment();
// True when a sphere is (partly) inside the camera's view.
bool SphereVisible(const render::Camera& camera, const Vec3& center, float radius);
void BuildClouds();
// Cloud fly-through (white fade and wisps) for the camera; a fixed-step update.
void UpdateClouds(float dt);
// 2D effects over the 3D scene: lens flare, cloud fade.
void DrawScreenEffects(const render::Camera& camera);

// --- GPU resources (assets.cpp)

constexpr int MAX_TILES = 256;

constexpr int LIVERIES = 3;          // player, pirate fighter, pirate ace (content::Livery order)
constexpr int MAX_SCARF = 8;
constexpr int MAX_PROP_GROUPS = 32;
constexpr int MAX_PALM_CLUSTERS = 64;
constexpr int MAX_PALMS = 512;

struct Assets {
    // Planes, per livery.
    render::MeshId plane[LIVERIES];
    render::MeshId planeDistant[LIVERIES];   // coarse copies for planes far from the camera
    render::MeshId canopy[LIVERIES];         // canopy glass, Blend::ALPHA
    render::MeshId propeller[LIVERIES];
    render::MeshId propellerDisc[LIVERIES];
    render::TextureId planeTexture[LIVERIES];
    Vec3 propellerHub[LIVERIES];
    Vec3 gunMuzzle[LIVERIES][2];
    render::TextureId propellerTexture = render::NO_TEXTURE;
    // The player's scarf: segments chained from the anchor along +Z.
    int scarfCount = 0;
    float scarfLength = 0.0f;
    Vec3 scarfAnchor;
    render::MeshId scarf[MAX_SCARF];
    // The zeppelin: whole, or body + hull sections + frame while it burns apart.
    render::MeshId zeppelin = render::NO_MESH;
    render::MeshId zeppelinDistant = render::NO_MESH;
    render::MeshId zeppelinBody = render::NO_MESH;
    render::MeshId zeppelinFrame = render::NO_MESH;
    int hullSectionCount = 0;
    Vec3 hullSectionCenter[MAX_HULL_SECTIONS];
    render::MeshId turret = render::NO_MESH;
    render::MeshId zeppelinEngine = render::NO_MESH;
    render::TextureId zeppelinTexture = render::NO_TEXTURE;
    // Pickups and weapons.
    render::MeshId rocket = render::NO_MESH;
    render::MeshId crate = render::NO_MESH;
    render::MeshId ring = render::NO_MESH;
    render::MeshId ringDistant = render::NO_MESH;
    // The level: terrain tiles and prop groups (lighting baked into vertex colours), the sea.
    int tileCount = 0;
    render::MeshId tiles[MAX_TILES];
    render::MeshId tileLod[MAX_TILES];       // half resolution, for distant tiles
    Vec3 tileCenter[MAX_TILES];
    float tileRadius[MAX_TILES];
    int propGroupCount = 0;
    render::MeshId propGroup[MAX_PROP_GROUPS];
    Vec3 propGroupCenter[MAX_PROP_GROUPS];
    float propGroupRadius[MAX_PROP_GROUPS];
    int palmClusterCount = 0;   // palms: a mesh per cluster up close, one billboard per palm farther out
    render::MeshId palmCluster[MAX_PALM_CLUSTERS];
    Vec3 palmClusterCenter[MAX_PALM_CLUSTERS];
    float palmClusterRadius[MAX_PALM_CLUSTERS];
    int palmClusterFirst[MAX_PALM_CLUSTERS];   // the cluster's palms in palmBoard
    int palmClusterSize[MAX_PALM_CLUSTERS];
    render::Billboard palmBoard[MAX_PALMS];    // each palm's impostor (position and size)
    render::TextureId palmImpostor = render::NO_TEXTURE;
    render::MeshId sea = render::NO_MESH;
    render::TextureId propsTexture = render::NO_TEXTURE;
    render::TextureId terrainTexture = render::NO_TEXTURE;
    render::TextureId waterTexture = render::NO_TEXTURE;
    render::TextureId terrainDetail = render::NO_TEXTURE;
    render::TextureId seaDetail = render::NO_TEXTURE;
    // Effects and 2D.
    render::TextureId particles = render::NO_TEXTURE;
    render::TextureId clouds = render::NO_TEXTURE;
    render::TextureId hud = render::NO_TEXTURE;
    render::TextureId title = render::NO_TEXTURE;
    render::TextureId streak = render::NO_TEXTURE; // soft-edged white ribbon for tracers and speed lines
    render::TextureId shadow = render::NO_TEXTURE; // blob shadow
    render::MeshId shadowQuad = render::NO_MESH;   // unit quad in the XZ plane, for shadows
    // Landmarks.
    const content::Terrain* terrain = nullptr;
    Vec3 harbour;              // the harbour bay
    Vec3 wharf;                // the middle of the wharf's deck
    Vec3 pirateBase;           // the pirates' rock tower
    Vec3 volcano;              // the crater (smoke plume)
    Vec3 lighthouseLamp;
    Vec3 canyonArch;           // the centre of the arch's opening over the canyon
};

extern Assets assets;

// The boss's arrival cut: the camera at the pirate tower while the zeppelin slides out from behind it (seconds).
constexpr float EMERGE_TIME = 2.5f;

// Height of the ground or the sea (whichever is higher) under a point.
float GroundHeight(float x, float z);

// --- Shared state (game.cpp)

struct World {
    Plane planes[MAX_PLANES];
    Zeppelin zeppelin;
    Stats stats;
    float time = 0.0f;        // mission seconds
    float timeScale = 1.0f;   // slow motion in the victory cinematic
    float slowMo = 0.0f;      // real seconds left of 0.35x slow motion (engine pod kills, zeppelin death)
    float drawAlpha = 1.0f;   // how far drawing is between the previous step and the current one (0..1)
    float revealTimer = 0.0f; // seconds left of the zeppelin reveal camera cut
    float introTimer = 0.0f;  // seconds left of the intro
    float shotAt = 99.0f;     // seconds since enemy fire last passed close to the player
    Vec3 introCamera;         // the intro's first shot: the camera over the wharf
    Vec3 introLook;           // the part of the village that shot frames with the plane
    Vec3 introExit;           // where the plane turns to after passing the cranes: out over the mole
    bool cinematic = false;   // the zeppelin's crash and the victory: the player's plane flies itself, untouchable
    int lives = 0;            // lives left, counting the current one
    float respawnTimer = 0.0f;
    TargetRef target;         // the player's locked target
    CameraMode cameraMode = CameraMode::CHASE;
    bool lookBack = false;
    bool targetView = false;
    bool showObjective = false;
    bool outOfBounds = false;
    bool invertY = false;
    bool titleMode = false;   // the title fly-by: no player, no HUD
    Rng rng;
};

extern World world;

// The intro: the first shot (the plane flying along the waterfront) lasts this long, then the chase camera.
constexpr float INTRO_TIME = 6.0f;
constexpr float INTRO_SHOT = 2.7f;

inline Plane& Player()
{
    return world.planes[PLAYER_PLANE];
}

// Player's rumble, scaled for the event (no-op in tests).
void Rumble(float low, float high, int milliseconds);

} // namespace game
