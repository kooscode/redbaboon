// The autopilot: an AI pilot for the player's plane that produces controller input, so it flies through exactly the
// same path as a real pad. Used by the host simulation tests and the attract / overnight demo mode: it hunts the
// nearest pirate or zeppelin engine with lead pursuit, fires rockets at aces and engines, barrel-rolls away when a
// pirate sits on its tail, flies the canyon rings between fights and throws in a loop for show.
#include "game/world.h"

namespace game {

constexpr float FIRE_CONE = 5.0f * DEG;
constexpr float FIRE_RANGE = 320.0f;     // it closes in like a good pilot before it fires
constexpr float ROCKET_CONE = 6.0f * DEG;
constexpr float ROCKET_RANGE = 500.0f;
constexpr float RING_APPROACH = 700.0f;      // run-in distance before the first ring of a pass
constexpr float SHOW_STUNT_GAP = 18.0f;
constexpr float ROLL_GAP = 4.0f;           // seconds between dodging rolls at least
constexpr float ENGINE_FOCUS_RANGE = 250.0f; // in the boss fight, planes farther than this wait for the engines
constexpr float LAST_ENGINE_FOCUS = 120.0f;   // ... and with only the glowing last engine left

struct Autopilot {
    bool aDown = false;         // A held in the previous step (presses are edges)
    bool leftTriggerDown = false;
    float showTimer = SHOW_STUNT_GAP;
    float rocketTimer = 0.0f;
    float breakTimer = 0.0f;    // > 0: flying away from the zeppelin after a pass
    float rollTimer = 0.0f;     // > 0: too soon for another dodging roll
    int ringAim = -1;           // the ring the course is flying to (-1: not on the course)
};

// The autopilot's memory between steps.
static Autopilot pilot;

// --- Helpers

static float InverseExpo(float v)
{
    return Sign(v) * powf(Clamp(fabsf(v), 0.0f, 1.0f), 1.0f / 1.6f);
}

// Pulls up from terrain and away from the zeppelin hull. Returns true when it steers.
static bool Avoid(const Plane& p, float clearance, FlightControls& c)
{
    const FlightState& f = p.flight;
    Vec3 fwd = QuatForward(f.rotation);
    Vec3 flat = Normalize(Vec3(fwd.x, 0.0f, fwd.z));
    for (int i = 1; i <= 3; i++) {
        Vec3 ahead = f.position + f.velocity * (i * 0.9f);
        if (ahead.y < GroundHeight(ahead.x, ahead.z) + clearance) {
            SteerToward(p, f.position + flat * 120.0f + Vec3(0.0f, 150.0f, 0.0f), 1.0f, c);
            c.turn = 0.0f;
            return true;
        }
    }
    const Zeppelin& z = world.zeppelin;
    if (z.state == ZeppelinState::CIRCLING || z.state == ZeppelinState::ARRIVING) {
        bool closing = false;
        for (int i = 1; i <= 5; i++)
            closing = closing || InsideHull(f.position + f.velocity * (i * 0.5f), 1.6f);
        if (closing) {
            Vec3 away = Normalize(f.position - z.position);
            SteerToward(p, f.position + away * 300.0f + Vec3(0.0f, 100.0f, 0.0f), 1.0f, c);
            return true;
        }
    }
    if (f.position.y > CEILING - 200.0f) {
        SteerToward(p, f.position + flat * 300.0f - Vec3(0.0f, 200.0f, 0.0f), 1.0f, c);
        return true;
    }
    return false;
}

// Lowest height above the ground over the next three seconds of straight flight.
static float Clearance(const Plane& p)
{
    float lowest = 1e9f;
    for (int i = 0; i <= 6; i++) {
        Vec3 at = p.flight.position + p.flight.velocity * (i * 0.5f);
        lowest = fminf(lowest, at.y - GroundHeight(at.x, at.z));
    }
    return lowest;
}

// True when the canyon walls rise above the plane on both sides (turning to fight in there hits a wall).
static bool InCanyon(const Plane& p)
{
    Vec3 fwd = QuatForward(p.flight.rotation);
    Vec3 side = Normalize(Vec3(-fwd.z, 0.0f, fwd.x));
    Vec3 left = p.flight.position - side * 200.0f;
    Vec3 right = p.flight.position + side * 200.0f;
    float y = p.flight.position.y;
    return GroundHeight(left.x, left.z) > y + 10.0f && GroundHeight(right.x, right.z) > y + 10.0f;
}

// The nearest pirate plane, or failing that the nearest zeppelin engine.
static TargetRef PickTarget(const Plane& p)
{
    TargetRef best;
    float bestDistance = 1e9f;
    for (int i = 1; i < MAX_PLANES; i++) {
        TargetRef t;
        t.kind = TargetKind::PLANE;
        t.index = i;
        if (!TargetValid(t))
            continue;
        float d = Length(TargetPosition(t) - p.flight.position);
        if (d < bestDistance) {
            bestDistance = d;
            best = t;
        }
    }
    // Planes first, except that in the boss fight the engines come first unless a plane is close.
    // While the last engine is armoured its aces come first, wherever they are.
    float focus = world.zeppelin.lastStand ? LAST_ENGINE_FOCUS : ENGINE_FOCUS_RANGE;
    if (LastEngineArmoured())
        focus = 1e9f;
    if (best.kind != TargetKind::NONE && bestDistance < focus)
        return best;
    TargetRef plane = best;
    best = TargetRef();
    bestDistance = 1e9f;
    for (int e = 0; e < world.zeppelin.engineCount; e++) {
        TargetRef t;
        t.kind = TargetKind::ENGINE;
        t.index = e;
        if (!TargetValid(t))
            continue;
        float d = Length(TargetPosition(t) - p.flight.position);
        if (d < bestDistance) {
            bestDistance = d;
            best = t;
        }
    }
    return best.kind != TargetKind::NONE ? best : plane;
}

// True when a pirate is close behind and pointing at the plane.
static bool Tailed(const Plane& p)
{
    for (int i = 1; i < MAX_PLANES; i++) {
        const Plane& e = world.planes[i];
        if (!e.alive)
            continue;
        Vec3 to = p.flight.position - e.flight.position;
        float d = Length(to);
        if (d < 250.0f && Dot(QuatForward(e.flight.rotation), to / fmaxf(d, 1.0f)) > 0.95f)
            return true;
    }
    return false;
}

// --- Behaviours

static void Hunt(const Plane& p, const TargetRef& target, FlightControls& c, bool& fire, bool& rocket)
{
    Vec3 targetPos = TargetPosition(target);
    Vec3 aim = LeadPoint(p.flight.position, p.flight.velocity, targetPos, TargetVelocity(target), BULLET_SPEED);
    SteerToward(p, aim, 1.0f, c);
    Vec3 to = aim - p.flight.position;
    float distance = Length(to);
    float angle = acosf(Clamp(Dot(QuatForward(p.flight.rotation), to / fmaxf(distance, 1.0f)), -1.0f, 1.0f));
    // Engines hang on the hull: break off close in and come round for another pass.
    if (target.kind == TargetKind::ENGINE && distance < 140.0f)
        pilot.breakTimer = 3.0f;
    fire = angle < FIRE_CONE && distance < FIRE_RANGE && p.gunHeat < 0.92f;
    bool worthy = target.kind == TargetKind::ENGINE ||
                  (target.kind == TargetKind::PLANE && world.planes[target.index].kind == PlaneKind::ACE);
    rocket = worthy && angle < ROCKET_CONE && distance < ROCKET_RANGE && pilot.rocketTimer <= 0.0f;
    if (distance > 280.0f && p.flight.meter > 0.45f)
        c.throttle = 1.0f;
    else if (distance < 110.0f && target.kind == TargetKind::PLANE)
        c.throttle = -0.8f;
}

// Climbs away from the zeppelin, out to the side, to line up the next pass.
static void BreakAway(const Plane& p, FlightControls& c)
{
    const Zeppelin& z = world.zeppelin;
    Vec3 away = p.flight.position - z.position;
    away.y = 0.0f;
    away = Normalize(away);
    SteerToward(p, z.position + away * 600.0f + Vec3(0.0f, 60.0f, 0.0f), 1.0f, c);
    c.throttle = p.flight.meter > 0.5f ? 1.0f : 0.0f;
}

// After the rings: straight out of the canyon first, then a scenic circle round the island.
static void Patrol(const Plane& p, FlightControls& c)
{
    int count = RingCountTotal();
    if (count >= 2) {
        Vec3 last = RingWorldPosition(count - 1);
        Vec3 course = Normalize(last - RingWorldPosition(count - 2));
        float along = Dot(p.flight.position - last, course);
        if (along > -50.0f && along < 600.0f && Length(p.flight.position - last) < 700.0f) {
            SteerToward(p, last + course * 900.0f + Vec3(0.0f, 60.0f, 0.0f), 1.0f, c);
            return;
        }
    }
    Vec3 center = assets.volcano;
    Vec3 rel = p.flight.position - center;
    float angle = atan2f(rel.z, rel.x) + 0.25f;
    Vec3 point(center.x + cosf(angle) * 2500.0f, assets.volcano.y * 0.5f + 250.0f, center.z + sinf(angle) * 2500.0f);
    SteerToward(p, point, 0.7f, c);
}

// A ring's axis, pointing the way the course runs (toward the next ring).
static Vec3 RingAxis(int ring)
{
    int count = RingCountTotal();
    Vec3 axis = Rotate(content::RingRotation(ring), Vec3(0.0f, 0.0f, 1.0f));
    Vec3 course = ring + 1 < count ? RingWorldPosition(ring + 1) - RingWorldPosition(ring)
                                   : RingWorldPosition(ring) - RingWorldPosition(ring - 1);
    return Dot(axis, course) >= 0.0f ? axis : -axis;
}

// Horizontal distance from a point to the line through `from` along `dir`, and the distance along it.
static void LineFix(const Vec3& point, const Vec3& from, const Vec3& dir, float& along, float& lateral)
{
    Vec3 rel = point - from;
    along = Dot(rel, dir);
    Vec3 off = rel - dir * along;
    lateral = sqrtf(off.x * off.x + off.z * off.z);
}

// True while the plane is on the ring course: the run-in to ring 0, or a leg between two rings.
static bool OnCourse(const Plane& p)
{
    int aim = pilot.ringAim;
    if (aim < 0 || aim >= RingCountTotal())
        return false;
    float along = 0.0f;
    float lateral = 0.0f;
    if (aim == 0) {
        LineFix(p.flight.position, RingWorldPosition(0), RingAxis(0), along, lateral);
        return along > -RING_APPROACH * 1.3f && along < 10.0f && lateral < Clamp(-along * 0.08f, 15.0f, 60.0f);
    }
    Vec3 from = RingWorldPosition(aim - 1);
    Vec3 dir = Normalize(RingWorldPosition(aim) - from);
    LineFix(p.flight.position, from, dir, along, lateral);
    return along > -60.0f && lateral < 80.0f;
}

// Inside the canyon off the ring course: fly on along it, through the next ring ahead (passed or not), until
// out at its end.
static void FollowCanyon(const Plane& p, FlightControls& c)
{
    Vec3 fwd = QuatForward(p.flight.rotation);
    int best = -1;
    float bestDistance = 1e9f;
    for (int i = 0; i < RingCountTotal(); i++) {
        Vec3 axis = RingAxis(i);
        Vec3 to = RingWorldPosition(i) - p.flight.position;
        float sign = Dot(axis, fwd) >= 0.0f ? 1.0f : -1.0f;
        float ahead = Dot(to, axis) * sign;
        if (ahead > 30.0f && Length(to) < bestDistance) {
            bestDistance = Length(to);
            best = i;
        }
    }
    if (best < 0) {
        SteerToward(p, p.flight.position + Normalize(Vec3(fwd.x, 0.0f, fwd.z)) * 300.0f, 1.0f, c);
        return;
    }
    Vec3 axis = RingAxis(best) * (Dot(RingAxis(best), fwd) >= 0.0f ? 1.0f : -1.0f);
    SteerToward(p, RingWorldPosition(best) + axis * 30.0f, 1.0f, c);
}

// Flies the ring course from ring 0 along the canyon, ring to ring: a carrot point slides along each leg ahead of
// the plane, and the last stretch before a ring lines up with its axis. Off the course (after a fight, or a miss
// that took it out of the canyon) it goes back round to the run-in to ring 0; flying through rings already passed
// is harmless.
static void FlyRings(const Plane& p, FlightControls& c)
{
    int count = RingCountTotal();
    if (NextRing() < 0) {
        pilot.ringAim = -1;
        Patrol(p, c);
        return;
    }
    if (pilot.ringAim < 0 || pilot.ringAim >= count || !OnCourse(p))
        pilot.ringAim = 0;
    int aim = pilot.ringAim;
    Vec3 ring = RingWorldPosition(aim);
    Vec3 axis = RingAxis(aim);
    float along = 0.0f;
    float lateral = 0.0f;
    LineFix(p.flight.position, ring, axis, along, lateral);
    // Past the ring's plane: on to the next leg.
    if (along > 0.0f && lateral < 60.0f) {
        pilot.ringAim = aim + 1 < count ? aim + 1 : -1;
        if (pilot.ringAim < 0) {
            Patrol(p, c);
            return;
        }
        aim = pilot.ringAim;
        ring = RingWorldPosition(aim);
        axis = RingAxis(aim);
        LineFix(p.flight.position, ring, axis, along, lateral);
    }
    if (aim == 0 && !OnCourse(p)) {
        // To the start of the run-in, at ring height near it and clear of the ground on the way.
        Vec3 entry = ring - axis * RING_APPROACH;
        float distance = Length(entry - p.flight.position);
        entry.y = fmaxf(entry.y + 15.0f, GroundHeight(entry.x, entry.z) + 50.0f);
        if (distance > 500.0f)
            entry.y = fmaxf(entry.y, GroundHeight(p.flight.position.x, p.flight.position.z) + 150.0f);
        SteerToward(p, entry, 1.0f, c);
        return;
    }
    // The last 120 m: line up with the ring's axis. Before that: the carrot on the leg.
    if (along > -120.0f) {
        SteerToward(p, ring + axis * 30.0f, 1.0f, c);
        return;
    }
    Vec3 from = aim == 0 ? ring - axis * RING_APPROACH : RingWorldPosition(aim - 1);
    Vec3 dir = Normalize(ring - from);
    float legAlong = 0.0f;
    float legLateral = 0.0f;
    LineFix(p.flight.position, from, dir, legAlong, legLateral);
    float legLength = Length(ring - from);
    float ahead = fminf(legAlong + 130.0f, legLength - 60.0f);
    Vec3 carrot = Lerp(from, ring, Clamp(ahead / legLength, 0.0f, 1.0f));
    SteerToward(p, carrot, 1.0f, c);
}

// --- Input

input::State AutopilotInput()
{
    input::State pad;
    pad.connected = true;
    if (world.time < STEP * 1.5f)
        pilot = Autopilot();
    const Plane& p = Player();
    pilot.rocketTimer -= STEP;
    pilot.showTimer -= STEP;
    pilot.breakTimer -= STEP;
    pilot.rollTimer -= STEP;
    if (!p.alive || world.introTimer > 0.0f || world.titleMode) {
        pilot.aDown = false;
        return pad;
    }

    FlightControls c;
    bool fire = false;
    bool rocket = false;
    Stunt stunt = Stunt::NONE;
    TargetRef target = PickTarget(p);
    // While rings are left, far-off enemies wait (the canyon run first: it also films well), except the harbour
    // raid and the zeppelin;
    // a ring pass once started is always finished (turning away inside the canyon would hit its walls).
    if (target.kind != TargetKind::NONE && NextRing() >= 0) {
        float distance = Length(TargetPosition(target) - p.flight.position);
        // Wave 3 (the harbour raid) and the boss are fought where they are, rings or not.
        bool focus = CurrentPhase() == Phase::BOSS || CurrentPhase() == Phase::WAVE3;
        if ((distance > 1400.0f && !focus) || OnCourse(p) || InCanyon(p))
            target = TargetRef();
    }
    // In the canyon the course line keeps clear of its walls; pulling straight up there would hit them.
    float clearance = target.kind == TargetKind::NONE ? 30.0f : 60.0f;
    bool inCanyon = InCanyon(p);
    if (inCanyon && p.flight.position.y > GroundHeight(p.flight.position.x, p.flight.position.z) + 15.0f) {
        if (NextRing() >= 0 && OnCourse(p))
            FlyRings(p, c);
        else
            FollowCanyon(p, c);
    } else if (!Avoid(p, clearance, c)) {
        if (pilot.breakTimer > 0.0f && target.kind == TargetKind::ENGINE)
            BreakAway(p, c);
        else if (target.kind != TargetKind::NONE)
            Hunt(p, target, c, fire, rocket);
        else
            FlyRings(p, c);
        bool steady = p.flight.stunt == Stunt::NONE && p.flight.meter > STUNT_COST + 0.1f;
        // Rolls only to dodge fire that is coming close; a loop now and then when there is nothing to fight.
        if (steady && pilot.rollTimer <= 0.0f && Tailed(p) && world.shotAt < 0.5f && Clearance(p) > 100.0f) {
            stunt = world.rng.Uniform() < 0.5f ? Stunt::ROLL_LEFT : Stunt::ROLL_RIGHT;
            pilot.rollTimer = ROLL_GAP;
        } else if (steady && pilot.showTimer <= 0.0f && target.kind == TargetKind::NONE && p.flight.meter > 0.9f &&
                   Clearance(p) > 200.0f) {
            stunt = Stunt::LOOP;
            pilot.showTimer = SHOW_STUNT_GAP;
        }
    }

    float pitchStick = InverseExpo(c.pitch);
    pad.leftY = world.invertY ? -pitchStick : pitchStick;
    pad.leftX = InverseExpo(c.turn);
    pad.rightX = c.rudder;
    pad.rightY = -c.throttle;
    pad.rightTrigger = fire ? 1.0f : 0.0f;

    // Stunts: A pressed with the stick held in the stunt's direction.
    int a = static_cast<int>(input::Button::A);
    bool pressA = stunt != Stunt::NONE && !pilot.aDown;
    if (pressA) {
        pad.leftX = stunt == Stunt::ROLL_RIGHT ? 1.0f : (stunt == Stunt::ROLL_LEFT ? -1.0f : 0.0f);
        pad.leftY = stunt == Stunt::IMMELMANN ? 1.0f : 0.0f;
    }
    pad.down[a] = pressA;
    pad.pressed[a] = pressA;
    pilot.aDown = pressA;

    // Rockets: the left trigger pulled for one step.
    pad.leftTrigger = rocket && !pilot.leftTriggerDown ? 1.0f : 0.0f;
    pilot.leftTriggerDown = pad.leftTrigger > 0.5f;
    if (pad.leftTrigger > 0.5f)
        pilot.rocketTimer = 1.2f;
    return pad;
}

} // namespace game
