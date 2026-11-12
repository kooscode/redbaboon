// The player's controls (docs/design.md §3, creative brief §5): the pad becomes flight controls (expo curve on the
// left stick, optional inverted Y), guns, rockets, stunts on A with the stick's direction, target cycling, the
// camera buttons, and the turn-back at the edge of the flight area and under the ceiling.
#include "game/world.h"

namespace game {

constexpr float STICK_EXPO = 1.6f;
constexpr float STUNT_STICK = 0.5f;    // stick deflection that picks a stunt direction
constexpr float AUTO_TARGET_RANGE = 1600.0f;

// Seconds until the next turn-back radio call.
static float boundsRadioTimer = 0.0f;

// Left trigger of the previous step (a rocket fires when it is pulled past half way).
static float lastLeftTrigger = 0.0f;

// Seconds until the next boost rumble pulse.
static float boostRumbleTimer = 0.0f;

static int Index(input::Button button)
{
    return static_cast<int>(button);
}

static float Expo(float v)
{
    return Sign(v) * powf(fabsf(v), STICK_EXPO);
}

// The stunt the stick asks for when A is pressed: up = loop, down = Immelmann, sideways = barrel roll.
static Stunt StuntFromStick(const input::State& pad)
{
    if (pad.leftX > STUNT_STICK)
        return Stunt::ROLL_RIGHT;
    if (pad.leftX < -STUNT_STICK)
        return Stunt::ROLL_LEFT;
    if (pad.leftY > STUNT_STICK)
        return Stunt::IMMELMANN;
    return Stunt::LOOP;
}

// Out of the flight area or above the ceiling: the plane turns itself back.
static void KeepInside(Plane& p, float dt)
{
    const Vec3& pos = p.flight.position;
    float edge = fmaxf(fabsf(pos.x), fabsf(pos.z));
    world.outOfBounds = edge > WARN_HALF_SIZE;
    boundsRadioTimer -= dt;
    if (world.outOfBounds && boundsRadioTimer <= 0.0f) {
        boundsRadioTimer = 12.0f;
        Radio(Speaker::HARBOUR, "Turn back, Red! You're leaving the island!");
    }
    // Back toward the island, high enough to clear the volcano.
    if (edge > FLIGHT_HALF_SIZE)
        SteerToward(p, Vec3(0.0f, fmaxf(pos.y, assets.volcano.y + 200.0f), 0.0f), 1.0f, p.controls);
    if (pos.y > CEILING) {
        p.controls.pitch = fminf(p.controls.pitch, -0.5f);
        world.outOfBounds = true;
    }
}

static void AutoTarget()
{
    if (TargetValid(world.target))
        return;
    TargetRef next = NextTarget(TargetRef());
    if (next.kind == TargetKind::NONE)
        return;
    if (Length(TargetPosition(next) - Player().flight.position) < AUTO_TARGET_RANGE)
        world.target = next;
}

void UpdatePlayerInput(const input::State& pad, float dt)
{
    Plane& p = Player();
    world.lookBack = pad.down[Index(input::Button::Y)];
    world.targetView = pad.down[Index(input::Button::X)];
    world.showObjective = pad.down[Index(input::Button::BACK)];
    if (pad.pressed[Index(input::Button::WHITE)])
        world.cameraMode = CameraMode::CHASE;
    if (pad.pressed[Index(input::Button::BLACK)])
        world.cameraMode = CameraMode::CLOSE;
    if (!p.alive)
        return;

    FlightControls& c = p.controls;
    c = FlightControls();
    p.trigger = false;
    // During the intro the plane flies itself: level along the waterfront, then out toward the lighthouse.
    if (world.introTimer > 0.0f) {
        if (INTRO_TIME - world.introTimer > INTRO_SHOT - 0.2f)
            SteerToward(p, world.introExit, 0.5f, c);
        return;
    }
    // During the crash cinematic it climbs away on its own.
    if (world.cinematic) {
        Vec3 fwd = QuatForward(p.flight.rotation);
        Vec3 ahead = p.flight.position + Normalize(Vec3(fwd.x, 0.0f, fwd.z)) * 600.0f;
        ahead.y = fmaxf(p.flight.position.y, GroundHeight(ahead.x, ahead.z) + 300.0f);
        SteerToward(p, ahead, 0.6f, c);
        p.invulnerable = 1.0f;
        return;
    }

    float pitch = Expo(pad.leftY);
    c.pitch = world.invertY ? -pitch : pitch;
    c.turn = Expo(pad.leftX);
    c.rudder = pad.rightX;
    c.throttle = -pad.rightY;
    if (pad.pressed[Index(input::Button::A)]) {
        if (p.flight.meter >= STUNT_COST && p.flight.stunt == Stunt::NONE)
            c.stunt = StuntFromStick(pad);
        else if (p.flight.stunt == Stunt::NONE)
            HudMeterRefused();
    }
    p.trigger = pad.rightTrigger > 0.3f;

    if (pad.pressed[Index(input::Button::B)]) {
        world.target = NextTarget(world.target);
        audio::Play(audio::Sfx::MENU_MOVE, 0.5f);
    }
    AutoTarget();
    if (pad.leftTrigger > 0.5f && lastLeftTrigger <= 0.5f)
        FireRocket(PLAYER_PLANE, world.target);
    lastLeftTrigger = pad.leftTrigger;

    // Boost held with an empty meter: the bar says why nothing happens.
    if (c.throttle > 0.3f && p.flight.meter <= 0.01f)
        HudMeterRefused();
    boostRumbleTimer -= dt;
    if (p.flight.boosting && boostRumbleTimer <= 0.0f) {
        boostRumbleTimer = 0.1f;
        Rumble(0.15f, 0.0f, 120);
    }
    KeepInside(p, dt);
}

} // namespace game
