// Arcade flight model (docs/design.md §8, creative brief §5): bank-to-turn with automatic coordinated yaw, wings
// that level themselves when the stick is released, speed traded against height, no real stall, and scripted stunts
// (loop, Immelmann turn, barrel roll) that cost a slice of the meter shared with boost.
#include "game/world.h"

namespace game {

// --- Plane types

// The Red Baboon: roll 200 deg/s, pitch 75 deg/s at the best-turn speed of 70 m/s.
const FlightParams PLAYER_FLIGHT = {
    75.0f,          // cruise
    122.0f,         // boost
    52.0f,          // brake
    75.0f * DEG,    // pitch
    64.0f * DEG,    // turn at full bank
    200.0f * DEG,   // roll
    25.0f * DEG,    // rudder
    78.0f * DEG,    // bank
    0.25f,          // boost drain: 4 s of boost on a full meter
    0.14f,          // recharge
};

// Pirate fighters: slower and heavier than the player.
const FlightParams FIGHTER_FLIGHT = {
    64.0f, 92.0f, 50.0f, 52.0f * DEG, 40.0f * DEG, 140.0f * DEG, 15.0f * DEG, 70.0f * DEG, 0.2f, 0.12f,
};

// Pirate aces: nearly as agile as the player.
const FlightParams ACE_FLIGHT = {
    72.0f, 112.0f, 52.0f, 66.0f * DEG, 54.0f * DEG, 190.0f * DEG, 20.0f * DEG, 78.0f * DEG, 0.25f, 0.14f,
};

// --- Helpers

// Turn rates scale with speed: 1 at 70 m/s, 35/75 at 45 m/s, 45/75 at 140 m/s (brief §5).
static float PitchFactor(float speed)
{
    if (speed < 70.0f)
        return Clamp((35.0f + (speed - 45.0f) * (40.0f / 25.0f)) / 75.0f, 0.4f, 1.0f);
    return Clamp((75.0f - (speed - 70.0f) * (30.0f / 70.0f)) / 75.0f, 0.55f, 1.0f);
}

// Bank-to-turn keeps its rate at low speed (braking tightens the turn) and loses it at high speed.
static float TurnFactor(float speed)
{
    if (speed < 70.0f)
        return Clamp(1.0f + (70.0f - speed) * 0.006f, 1.0f, 1.12f);
    return Clamp(1.0f - (speed - 70.0f) * 0.0055f, 0.6f, 1.0f);
}

float BankAngle(const Quat& rotation)
{
    Vec3 right = QuatRight(rotation);
    Vec3 up = QuatUp(rotation);
    return atan2f(-right.y, up.y);
}

// Rotates about a local axis (applied in the plane's own frame).
static void RotateLocal(Quat& rotation, const Vec3& axis, float angle)
{
    rotation = rotation * QuatAxisAngle(axis, angle);
}

bool StuntDodging(const FlightState& state)
{
    return state.stunt == Stunt::ROLL_LEFT || state.stunt == Stunt::ROLL_RIGHT;
}

const char* StuntName(Stunt stunt)
{
    switch (stunt) {
    case Stunt::LOOP:
        return "LOOP";
    case Stunt::IMMELMANN:
        return "IMMELMANN";
    case Stunt::ROLL_LEFT:
    case Stunt::ROLL_RIGHT:
        return "BARREL ROLL";
    default:
        return "";
    }
}

void ResetFlight(FlightState& state, const Vec3& position, const Quat& rotation, float speed)
{
    state = FlightState();
    state.position = position;
    state.rotation = rotation;
    state.speed = speed;
    state.velocity = QuatForward(rotation) * speed;
}

// --- Stunts

constexpr float LOOP_TIME = 2.6f;
constexpr float IMMELMANN_PITCH_TIME = 1.3f;
constexpr float IMMELMANN_ROLL_TIME = 0.5f;
constexpr float ROLL_TIME = 0.8f;
constexpr float ROLL_SIDESTEP = 16.0f; // metres a barrel roll moves the plane sideways

// Turns by rate * dt, but never past the phase's total angle. Returns the angle to apply this step.
static float StuntStep(FlightState& state, float total, float time, float dt)
{
    float step = total / time * dt;
    if (state.stuntAngle + step > total)
        step = total - state.stuntAngle;
    state.stuntAngle += step;
    return step;
}

static void EndStunt(FlightState& state)
{
    state.stunt = Stunt::NONE;
    state.stuntAngle = 0.0f;
    state.stuntPhase = 0;
    state.stuntsDone++;
    state.rotation = QuatNormalize(state.rotation);
}

static void UpdateStunt(FlightState& state, float dt)
{
    state.stuntTime += dt;
    if (state.stunt == Stunt::LOOP) {
        RotateLocal(state.rotation, Vec3(1, 0, 0), StuntStep(state, 2.0f * PI, LOOP_TIME, dt));
        if (state.stuntAngle >= 2.0f * PI - 1e-4f)
            EndStunt(state);
        return;
    }
    if (state.stunt == Stunt::IMMELMANN) {
        if (state.stuntPhase == 0) {
            RotateLocal(state.rotation, Vec3(1, 0, 0), StuntStep(state, PI, IMMELMANN_PITCH_TIME, dt));
            if (state.stuntAngle >= PI - 1e-4f) {
                state.stuntPhase = 1;
                state.stuntAngle = 0.0f;
            }
            return;
        }
        RotateLocal(state.rotation, Vec3(0, 0, -1), StuntStep(state, PI, IMMELMANN_ROLL_TIME, dt));
        if (state.stuntAngle >= PI - 1e-4f)
            EndStunt(state);
        return;
    }
    float side = state.stunt == Stunt::ROLL_RIGHT ? 1.0f : -1.0f;
    float before = state.stuntAngle;
    float step = StuntStep(state, 2.0f * PI, ROLL_TIME, dt);
    // Sideways hop that follows a sine-eased curve; measured in the level heading so the roll does not bend it.
    Vec3 fwd = QuatForward(state.rotation);
    Vec3 side3 = Normalize(Cross(fwd, WORLD_UP));
    float eased0 = 0.5f - 0.5f * cosf(before * 0.5f);
    float eased1 = 0.5f - 0.5f * cosf((before + step) * 0.5f);
    state.position += side3 * (side * ROLL_SIDESTEP * (eased1 - eased0));
    RotateLocal(state.rotation, Vec3(0, 0, -1), side * step);
    if (state.stuntAngle >= 2.0f * PI - 1e-4f)
        EndStunt(state);
}

static void StartStunt(FlightState& state, Stunt stunt)
{
    if (state.stunt != Stunt::NONE || stunt == Stunt::NONE || state.meter < STUNT_COST)
        return;
    state.stunt = stunt;
    state.stuntTime = 0.0f;
    state.stuntAngle = 0.0f;
    state.stuntPhase = 0;
    state.meter -= STUNT_COST;
    state.meterDelay = 1.0f;
    state.rollRate = 0.0f;
    // Loops and Immelmanns start from level wings so they stay in the vertical plane.
    if (stunt == Stunt::LOOP || stunt == Stunt::IMMELMANN) {
        Vec3 fwd = QuatForward(state.rotation);
        if (fabsf(fwd.y) < 0.9f)
            state.rotation = QuatLook(fwd, WORLD_UP);
    }
}

// --- Normal flight

// Roll, pitch and turn from the stick, with auto-levelling.
static void Steer(FlightState& state, const FlightParams& params, const FlightControls& controls, float dt)
{
    Vec3 fwd = QuatForward(state.rotation);
    float bank = BankAngle(state.rotation);
    float pitchFactor = PitchFactor(state.speed);
    bool steep = fabsf(fwd.y) > 0.85f;
    bool turning = fabsf(controls.turn) > 0.05f;
    state.bankHold = turning ? 0.0f : state.bankHold + dt;

    // Commanded roll rate: hold the stick's bank; near vertical the stick rolls directly; when released the
    // wings level (eased: the rate falls as the bank does, about 1 s from a full bank). A strong pull with the
    // stick released does not level the wings, so the player can loop by hand.
    float rollCommand = 0.0f;
    if (steep) {
        rollCommand = controls.turn * params.rollRate;
    } else if (turning) {
        rollCommand = Clamp((controls.turn * params.maxBank - bank) * 7.0f, -params.rollRate, params.rollRate);
    } else if (fabsf(controls.pitch) < 0.6f) {
        float level = Clamp(state.bankHold * 4.0f, 0.0f, 1.0f);
        rollCommand = Clamp(-bank * 3.5f * level, -params.rollRate, params.rollRate);
    }
    // Full roll rate is reached in about 0.12 s.
    state.rollRate = Approach(state.rollRate, rollCommand, 25.0f, dt);

    float pitchRate = controls.pitch * params.pitchRate * pitchFactor;

    float horizontal = sqrtf(Clamp(1.0f - fwd.y * fwd.y, 0.0f, 1.0f));
    float yawWorld = -sinf(bank) / sinf(params.maxBank) * params.turnRate * TurnFactor(state.speed) * horizontal;

    state.rotation = QuatAxisAngle(WORLD_UP, yawWorld * dt) * state.rotation;
    RotateLocal(state.rotation, Vec3(0, 1, 0), -controls.rudder * params.yawRate * dt);
    RotateLocal(state.rotation, Vec3(1, 0, 0), pitchRate * dt);
    RotateLocal(state.rotation, Vec3(0, 0, -1), state.rollRate * dt);
    // Near the minimum speed the nose falls toward the ground instead of the plane stalling.
    float slow = MIN_SPEED + 8.0f - state.speed;
    if (slow > 0.0f) {
        Vec3 nose = QuatForward(state.rotation);
        Vec3 axis = Cross(nose, Vec3(0.0f, -1.0f, 0.0f));
        if (LengthSq(axis) < 1e-4f)
            axis = QuatRight(state.rotation);
        state.rotation = QuatAxisAngle(axis, slow * 0.12f * dt) * state.rotation;
    }
    state.rotation = QuatNormalize(state.rotation);

    // Load factor in "g-equivalents": the arcade turn rates are far beyond a real plane's, so the centripetal part
    // is scaled down to put a full stick-only turn at cruise near 4 and a hard pull near 6.
    constexpr float LOAD_SCALE = 0.4f;
    float turnRate = sqrtf(yawWorld * yawWorld + pitchRate * pitchRate);
    state.turnLoad = 1.0f + LOAD_SCALE * turnRate * state.speed / GRAVITY;
}

// Throttle, boost meter and energy: climbing costs speed, diving gains it.
static void UpdateSpeed(FlightState& state, const FlightParams& params, float throttle, float dt)
{
    float target = params.cruiseSpeed;
    state.boosting = throttle > 0.3f && state.meter > 0.0f && state.stunt == Stunt::NONE;
    if (state.boosting) {
        target = Lerp(params.cruiseSpeed, params.boostSpeed, Clamp(throttle * 1.25f, 0.0f, 1.0f));
        state.meter = Clamp(state.meter - params.boostDrain * dt, 0.0f, 1.0f);
        state.meterDelay = 0.6f;
    } else if (throttle < -0.2f) {
        target = Lerp(params.cruiseSpeed, params.brakeSpeed, Clamp(-throttle * 1.25f, 0.0f, 1.0f));
    }
    if (state.stunt != Stunt::NONE)
        target = fmaxf(target, params.cruiseSpeed);
    state.meterDelay -= dt;
    if (state.meterDelay <= 0.0f)
        state.meter = Clamp(state.meter + params.meterRecharge * dt, 0.0f, 1.0f);

    // Engine thrust is limited (a climb loses speed even at full power) except while boosting; above the target
    // speed the drag is weak, so a dive builds speed, unless the brake is held.
    float rate = target > state.speed ? 0.7f : (throttle < -0.2f ? 1.2f : 0.35f);
    float accel = (target - state.speed) * rate;
    accel = fminf(accel, state.boosting ? 15.0f : 4.0f);
    // Gravity along the flight path; stunts keep most of their energy so they always finish.
    float energy = state.stunt == Stunt::NONE ? 0.85f : 0.25f;
    accel -= GRAVITY * QuatForward(state.rotation).y * energy;
    state.speed = Clamp(state.speed + accel * dt, MIN_SPEED, MAX_SPEED);
    state.throttle = Approach(state.throttle, state.boosting ? 1.0f : (throttle < -0.2f ? 0.25f : 0.6f), 4.0f, dt);
}

void UpdateFlight(FlightState& state, const FlightParams& params, const FlightControls& controls, float dt)
{
    if (state.stunt == Stunt::NONE)
        StartStunt(state, controls.stunt);
    if (state.stunt != Stunt::NONE) {
        UpdateStunt(state, dt);
        state.turnLoad = 4.0f;
    } else {
        Steer(state, params, controls, dt);
    }
    UpdateSpeed(state, params, controls.throttle, dt);

    // The velocity follows the nose quickly but not instantly: a little slide in hard manoeuvres.
    Vec3 want = QuatForward(state.rotation) * state.speed;
    state.velocity = Lerp(state.velocity, want, 1.0f - expf(-7.0f * dt));
    state.position += state.velocity * dt;
}

} // namespace game
