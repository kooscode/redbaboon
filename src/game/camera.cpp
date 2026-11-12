// Cameras (docs/design.md §8, creative brief §4): the chase camera (critically damped spring, rotation lag, partial
// roll, speed-driven field of view, positional shake, terrain and hull clip avoidance), look-back and target view
// blended in over 0.2 s, and the cinematic cameras: intro sweep, zeppelin reveal, death, crash and title fly-by.
// The camera is stepped with the simulation and interpolated for drawing.
#include "game/world.h"

namespace game {

// Chase: the wingspan fills about a third of the screen width, the plane's centre sits at about 62 % of the height.
constexpr float CHASE_BACK = 21.0f;
constexpr float CHASE_UP = 5.0f;
constexpr float CLOSE_BACK = 12.0f;
constexpr float CLOSE_UP = 2.8f;
constexpr float LOOK_AHEAD = 60.0f;
constexpr float LOOK_DOWN = 3.0f;        // the look point sits this far below the flight path
constexpr float SPRING_OMEGA = 16.0f;    // critically damped, settles in about 0.25 s
constexpr float ROTATION_LAG = 0.12f;
constexpr float ROLL_FOLLOW = 0.7f;
constexpr float ROLL_FOLLOW_STUNT = 0.4f;
constexpr float FOV_CRUISE = 58.0f * DEG;
constexpr float FOV_BOOST = 70.0f * DEG;
constexpr float FOV_BRAKE = 54.0f * DEG;
constexpr float BLEND_RATE = 5.0f;       // look-back and target view blend in over 0.2 s
constexpr float SHAKE_RATE = 25.0f;      // shake noise samples per second
constexpr float CAMERA_CLEARANCE = 2.5f;
constexpr float SHOT_ALONGSIDE = 7.0f;   // the crash cinematic's cuts (seconds into it)
constexpr float SHOT_LOW = 12.0f;
constexpr float SPLASH_DISTANCE = 400.0f; // the splash shot: from the impact
constexpr float SPLASH_ANGLE = 25.0f * DEG; // off the dive's line
constexpr float SPLASH_HEIGHT = 20.0f;
constexpr float SPLASH_FOV = 26.0f * DEG; // the 80 m column fills over 40 % of the screen height at its peak

struct CameraRig {
    Vec3 offset;             // spring: camera position relative to the plane
    Vec3 offsetVelocity;
    Quat rotation;           // lagged plane rotation with partial roll
    float rollCorrection = 0.0f;
    float stuntLook = 0.0f;  // 0 look ahead .. 1 look at the plane (loops)
    float fov = FOV_CRUISE;
    float lookBack = 0.0f;   // blend weights 0..1
    float targetView = 0.0f;
    float shake = 0.0f;      // metres
    float shakeTimer = 0.0f;
    Vec3 shakeFrom;
    Vec3 shakeTo;
    Vec3 deathPosition;      // where the camera watches a falling plane from
    bool deathSet = false;
    Vec3 cinemaPosition;     // where the crash cinematic's camera is
    float cinemaSide = 1.0f; // which side of the zeppelin it films from
    float cinemaAngle = 0.0f; // how far it has swung round toward the nose
    float cinemaTime = 0.0f; // seconds into the crash cinematic (picks the shot)
    bool cut = false;        // the next camera is a hard cut (no blend from the last one)
    bool cinemaSet = false;
    bool introChase = false; // the intro has cut to the chase camera
    bool revealing = false;  // the arrival cut is on (its end is a cut back to the chase)
    float titleAngle = 0.0f;
    bool started = false;    // the chase rotation has been set
    bool placed = false;     // a camera has been computed (otherwise the first one is a cut)
};

// The rig and the cameras of the last two steps (drawing interpolates between them).
static CameraRig rig;
static render::Camera previous;
static render::Camera current;
static render::Camera drawn;

// --- Helpers

static float Smooth(float t)
{
    t = Clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Pulls a camera position out of the ground and out of the zeppelin hull (toward the point it follows).
static Vec3 Unclip(Vec3 position, const Vec3& anchor)
{
    for (int i = 0; i < 8 && InsideHull(position, 1.1f); i++)
        position = Lerp(position, anchor, 0.3f);
    float ground = GroundHeight(position.x, position.z) + CAMERA_CLEARANCE;
    if (position.y < ground)
        position.y = ground;
    return position;
}

static Quat LookAt(const Vec3& from, const Vec3& to, const Vec3& up)
{
    Vec3 dir = Normalize(to - from);
    if (LengthSq(dir) < 1e-6f)
        dir = Vec3(0.0f, 0.0f, -1.0f);
    Vec3 safeUp = fabsf(Dot(dir, up)) > 0.98f ? Vec3(0.0f, 0.0f, 1.0f) : up;
    return QuatLook(dir, safeUp);
}

void CameraShake(float amount)
{
    rig.shake = fmaxf(rig.shake, amount);
}

void ResetCamera()
{
    rig = CameraRig();
}

const render::Camera& CurrentCamera()
{
    float a = world.drawAlpha;
    drawn = current;
    drawn.position = Lerp(previous.position, current.position, a);
    drawn.rotation = QuatSlerp(previous.rotation, current.rotation, a);
    drawn.fovY = Lerp(previous.fovY, current.fovY, a);
    // Never under the water or the ground, whatever the blend between steps does.
    drawn.position.y = fmaxf(drawn.position.y, GroundHeight(drawn.position.x, drawn.position.z) + CAMERA_CLEARANCE);
    return drawn;
}

// --- Chase camera

// Plane rotation with only part of its roll: the horizon tilts, but less than the plane does.
static Quat PartialRoll(const Plane& p, float dt)
{
    const FlightState& f = p.flight;
    Vec3 fwd = QuatForward(f.rotation);
    float target = 0.0f;
    bool vertical = f.stunt == Stunt::LOOP || f.stunt == Stunt::IMMELMANN || fabsf(fwd.y) > 0.7f;
    if (!vertical) {
        float follow = StuntDodging(f) ? ROLL_FOLLOW_STUNT : ROLL_FOLLOW;
        target = -BankAngle(f.rotation) * (1.0f - follow);
    }
    rig.rollCorrection = Approach(rig.rollCorrection, target, 8.0f, dt);
    return QuatNormalize(f.rotation * QuatAxisAngle(Vec3(0.0f, 0.0f, -1.0f), rig.rollCorrection));
}

static void Chase(const Plane& p, float dt, Vec3& position, Quat& rotation)
{
    const FlightState& f = p.flight;
    Quat base = PartialRoll(p, dt);
    if (!rig.started) {
        rig.rotation = base;
        rig.started = true;
    }
    rig.rotation = QuatNormalize(QuatSlerp(rig.rotation, base, 1.0f - expf(-dt / ROTATION_LAG)));
    bool close = world.cameraMode == CameraMode::CLOSE;
    float back = (close ? CLOSE_BACK : CHASE_BACK) + (f.speed - PLAYER_FLIGHT.cruiseSpeed) * 0.03f;
    float up = close ? CLOSE_UP : CHASE_UP;
    Vec3 want = Rotate(rig.rotation, Vec3(0.0f, up, back));
    Vec3 accel = (want - rig.offset) * (SPRING_OMEGA * SPRING_OMEGA) - rig.offsetVelocity * (2.0f * SPRING_OMEGA);
    rig.offsetVelocity += accel * dt;
    rig.offset += rig.offsetVelocity * dt;
    position = f.position + rig.offset;
    // In loops, Immelmann turns and steep climbs the camera looks at the plane itself, so it never slides off the
    // screen.
    bool looping = f.stunt == Stunt::LOOP || f.stunt == Stunt::IMMELMANN || fabsf(QuatForward(f.rotation).y) > 0.7f;
    // Hard pulls lean the same way, part of the way.
    float pull = Clamp((f.turnLoad - 4.0f) / 3.0f, 0.0f, 0.6f);
    rig.stuntLook = Approach(rig.stuntLook, looping ? 1.0f : pull, 6.0f, dt);
    Vec3 ahead = f.position + QuatForward(f.rotation) * LOOK_AHEAD - QuatUp(rig.rotation) * LOOK_DOWN;
    Vec3 look = Lerp(ahead, f.position, rig.stuntLook);
    rotation = LookAt(position, look, QuatUp(rig.rotation));

    // Look back: from ahead of the plane, looking back past it.
    rig.lookBack = Approach(rig.lookBack, world.lookBack ? 1.0f : 0.0f, BLEND_RATE * 3.0f, dt);
    if (rig.lookBack > 0.001f) {
        Vec3 backPos = f.position + Rotate(rig.rotation, Vec3(0.0f, 3.0f, -12.0f));
        Quat backRot = LookAt(backPos, f.position - QuatForward(f.rotation) * 60.0f, QuatUp(rig.rotation));
        position = Lerp(position, backPos, Smooth(rig.lookBack));
        rotation = QuatSlerp(rotation, backRot, Smooth(rig.lookBack));
    }
    // Target view: behind the player, looking at the locked target.
    bool targetOn = world.targetView && TargetValid(world.target);
    rig.targetView = Approach(rig.targetView, targetOn ? 1.0f : 0.0f, BLEND_RATE * 3.0f, dt);
    if (rig.targetView > 0.001f && TargetValid(world.target)) {
        Vec3 target = TargetPosition(world.target);
        Vec3 dir = Normalize(target - f.position);
        Vec3 viewPos = f.position - dir * 16.0f + WORLD_UP * 4.0f;
        Quat viewRot = LookAt(viewPos, target, WORLD_UP);
        position = Lerp(position, viewPos, Smooth(rig.targetView));
        rotation = QuatSlerp(rotation, viewRot, Smooth(rig.targetView));
    }

    // Field of view: wider with boost, narrower when braking (the main sense of speed).
    float fov = FOV_CRUISE;
    if (f.boosting)
        fov = FOV_BOOST;
    else if (p.controls.throttle < -0.2f)
        fov = FOV_BRAKE;
    fov += Clamp((f.speed - 100.0f) * 0.1f, 0.0f, 4.0f) * DEG;
    rig.fov = Approach(rig.fov, fov, 5.0f, dt);
    if (f.boosting)
        CameraShake(0.04f);
}

// --- Cinematic cameras

// Intro: first from 30 m over the wharf, the village beside it, watching the plane come in along the waterfront
// (it stays in frame: the view leans toward the village only part of the way); then a cut to the chase camera as
// it turns out to sea.
static void Intro(const Plane& p, float dt, Vec3& position, Quat& rotation)
{
    if (INTRO_TIME - world.introTimer < INTRO_SHOT) {
        position = world.introCamera;
        rotation = LookAt(position, Lerp(p.flight.position, world.introLook, 0.4f), WORLD_UP);
        rig.introChase = false;
        return;
    }
    if (!rig.introChase) {
        rig.rotation = p.flight.rotation;
        rig.offset = Rotate(p.flight.rotation, Vec3(0.0f, CHASE_UP, CHASE_BACK));
        rig.offsetVelocity = Vec3();
        rig.started = true;
        rig.cut = true;
        rig.introChase = true;
    }
    Chase(p, dt, position, rotation);
}

// The zeppelin's arrival: content's shot of the pirate tower, the airship sliding out from behind its keep.
static void Reveal(Vec3& position, Quat& rotation)
{
    Vec3 target;
    content::PirateTowerShot(position, target);
    rotation = LookAt(position, target, WORLD_UP);
}

// The zeppelin's death in three shots, none held longer than 7 s: alongside it while it burns apart; low over
// the water ahead of it, looking up as it comes down; then the long shot of the splash from the sea side, with
// the island behind.
static void CrashCinema(Vec3& position, Quat& rotation, float dt)
{
    const Zeppelin& z = world.zeppelin;
    if (!rig.cinemaSet) {
        Vec3 side = QuatRight(z.rotation);
        Vec3 toPlayer = Player().flight.position - z.position;
        rig.cinemaSide = Dot(side, toPlayer) < 0.0f ? -1.0f : 1.0f;
        rig.cinemaAngle = 0.0f;
        rig.cinemaTime = 0.0f;
        rig.cinemaSet = true;
    }
    int shot = rig.cinemaTime < SHOT_ALONGSIDE ? 0 : (rig.cinemaTime < SHOT_LOW ? 1 : 2);
    rig.cinemaTime += dt;
    int next = rig.cinemaTime < SHOT_ALONGSIDE ? 0 : (rig.cinemaTime < SHOT_LOW ? 1 : 2);
    rig.cut = rig.cut || next != shot;
    Vec3 path = z.crashTarget - z.crashStart;
    Vec3 flat = Normalize(Vec3(path.x, 0.0f, path.z));
    Vec3 side = Cross(flat, WORLD_UP) * rig.cinemaSide;
    if (rig.cinemaTime < SHOT_ALONGSIDE && z.state == ZeppelinState::CRASHING) {
        rig.cinemaAngle = fminf(rig.cinemaAngle + dt * 0.06f, 0.4f);
        Vec3 offset = side * (cosf(rig.cinemaAngle) * 260.0f) + flat * (60.0f + sinf(rig.cinemaAngle) * 260.0f);
        rig.cinemaPosition = z.position + offset + Vec3(0.0f, -30.0f, 0.0f);
    } else if (rig.cinemaTime < SHOT_LOW) {
        // Low over the water two thirds of the way along the dive, off to the side.
        Vec3 spot = Lerp(z.crashStart, z.crashTarget, 0.75f) + side * 220.0f;
        rig.cinemaPosition = Vec3(spot.x, GroundHeight(spot.x, spot.z) + 12.0f, spot.z);
    } else {
        // Ahead of the impact, 25 degrees off the dive's line, low over the sea: the island behind the column.
        Vec3 dir = flat * cosf(SPLASH_ANGLE) + side * sinf(SPLASH_ANGLE);
        Vec3 spot = z.crashTarget + dir * SPLASH_DISTANCE;
        rig.cinemaPosition = Vec3(spot.x, SPLASH_HEIGHT, spot.z);
    }
    float floor = GroundHeight(rig.cinemaPosition.x, rig.cinemaPosition.z) + 10.0f;
    rig.cinemaPosition.y = fmaxf(rig.cinemaPosition.y, floor);
    position = rig.cinemaPosition;
    Vec3 look = z.state == ZeppelinState::SUNK ? Vec3(z.position.x, 38.0f, z.position.z) : z.position;
    rotation = LookAt(position, look, WORLD_UP);
}

// Watching the player's falling plane from where the camera was when it was hit.
static void Death(const Plane& p, Vec3& position, Quat& rotation)
{
    if (!rig.deathSet) {
        rig.deathPosition = current.position;
        rig.deathSet = true;
    }
    position = rig.deathPosition;
    rotation = LookAt(position, p.flight.position, WORLD_UP);
}

static void Title(float dt, Vec3& position, Quat& rotation)
{
    rig.titleAngle += dt * 0.035f;
    Vec3 center = assets.volcano;
    // Wide and high enough to show the whole island, the lagoon and the sea around it.
    position = Vec3(center.x + cosf(rig.titleAngle) * 3300.0f, 760.0f, center.z + sinf(rig.titleAngle) * 3300.0f);
    Vec3 look(center.x, center.y * 0.3f, center.z);
    rotation = LookAt(position, look, WORLD_UP);
}

// --- Update

static void ApplyShake(Vec3& position, float dt)
{
    rig.shake *= expf(-6.0f * dt);
    rig.shakeTimer -= dt;
    if (rig.shakeTimer <= 0.0f) {
        rig.shakeTimer += 1.0f / SHAKE_RATE;
        rig.shakeFrom = rig.shakeTo;
        rig.shakeTo = Vec3(world.rng.Range(-1, 1), world.rng.Range(-1, 1), world.rng.Range(-1, 1));
    }
    float t = 1.0f - Clamp(rig.shakeTimer * SHAKE_RATE, 0.0f, 1.0f);
    position += Lerp(rig.shakeFrom, rig.shakeTo, t) * rig.shake;
}

void UpdateCamera(float dt)
{
    Vec3 position;
    Quat rotation;
    float fov = rig.fov;
    const Plane& p = Player();
    Phase phase = CurrentPhase();
    bool cinema = phase == Phase::CRASH || phase == Phase::VICTORY;
    if (world.titleMode) {
        Title(dt, position, rotation);
        fov = FOV_CRUISE;
    } else if (cinema && world.zeppelin.state != ZeppelinState::ABSENT) {
        CrashCinema(position, rotation, dt);
        fov = rig.cinemaTime >= SHOT_LOW ? SPLASH_FOV : 50.0f * DEG;
    } else if (!p.alive) {
        Death(p, position, rotation);
    } else if (world.introTimer > 0.0f) {
        Intro(p, dt, position, rotation);
        rig.deathSet = false;
    } else if (world.revealTimer > 0.0f) {
        Reveal(position, rotation);
        rig.revealing = true;
    } else {
        rig.deathSet = false;
        Chase(p, dt, position, rotation);
        fov = rig.fov;
    }
    ApplyShake(position, dt);
    Vec3 anchor = p.active ? p.flight.position : position;
    position = Unclip(position, anchor);

    if (rig.revealing && world.revealTimer <= 0.0f) {
        rig.revealing = false;
        rig.cut = true;
    }
    bool cut = !rig.placed || world.revealTimer > EMERGE_TIME - 0.1f || rig.cut;
    rig.cut = false;
    rig.placed = true;
    previous = current;
    current.position = position;
    current.rotation = rotation;
    current.fovY = fov;
    // A far near plane keeps depth precision at range (close chase is 8 m behind).
    current.nearZ = 2.0f;
    current.farZ = 7000.0f;
    if (cut)
        previous = current;
}

} // namespace game
