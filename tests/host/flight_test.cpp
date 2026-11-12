// Unit tests of the flight model (src/game/flight.cpp, creative brief §5): speed limits and energy, turn and pitch
// rates, auto-levelling, the three stunts and the shared boost/stunt meter. Prints the measured numbers.
#include "game/world.h"

#include <stdio.h>

using namespace game;

// Failed checks so far.
static int failures = 0;

// --- Helpers

static void Check(bool ok, const char* what, float value)
{
    printf("  %-44s %9.2f  %s\n", what, value, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

static FlightState LevelFlight(float speed)
{
    FlightState s;
    ResetFlight(s, Vec3(0.0f, 1000.0f, 0.0f), Quat(), speed);
    return s;
}

static float Heading(const FlightState& s)
{
    Vec3 f = QuatForward(s.rotation);
    return atan2f(f.x, -f.z);
}

static void Fly(FlightState& s, const FlightControls& c, float seconds)
{
    int steps = static_cast<int>(seconds / STEP + 0.5f);
    for (int i = 0; i < steps; i++)
        UpdateFlight(s, PLAYER_FLIGHT, c, STEP);
}

// Seconds until the heading has turned by angle (radians) with these controls, or 99.
static float TimeToTurn(FlightState s, const FlightControls& c, float angle)
{
    float turned = 0.0f;
    float last = Heading(s);
    for (int i = 0; i < 60 * 30; i++) {
        UpdateFlight(s, PLAYER_FLIGHT, c, STEP);
        float h = Heading(s);
        turned += fabsf(WrapPi(h - last));
        last = h;
        if (turned >= angle)
            return (i + 1) * STEP;
    }
    return 99.0f;
}

// --- Tests

static void TestSpeeds()
{
    printf("speeds\n");
    FlightState s = LevelFlight(60.0f);
    Fly(s, FlightControls(), 10.0f);
    Check(fabsf(s.speed - PLAYER_FLIGHT.cruiseSpeed) < 1.5f, "cruise speed (m/s)", s.speed);

    FlightControls boost;
    boost.throttle = 1.0f;
    float top = 0.0f;
    for (int i = 0; i < 60 * 6; i++) {
        UpdateFlight(s, PLAYER_FLIGHT, boost, STEP);
        top = fmaxf(top, s.speed);
    }
    Check(top > 105.0f && top <= PLAYER_FLIGHT.boostSpeed + 1.0f, "boost top speed (m/s)", top);
    Check(s.meter < 0.05f, "meter after 6 s of boost", s.meter);
    Fly(s, boost, 6.0f);
    Check(s.speed < PLAYER_FLIGHT.cruiseSpeed + 8.0f, "speed with an empty meter (m/s)", s.speed);

    FlightControls brake;
    brake.throttle = -1.0f;
    Fly(s, brake, 8.0f);
    Check(fabsf(s.speed - PLAYER_FLIGHT.brakeSpeed) < 3.0f, "brake speed (m/s)", s.speed);

    FlightState dive = LevelFlight(75.0f);
    FlightControls down;
    down.pitch = -1.0f;
    Fly(dive, down, 1.2f);
    Fly(dive, boost, 8.0f);
    Check(dive.speed > 100.0f && dive.speed <= MAX_SPEED, "dive builds speed, capped (m/s)", dive.speed);

    FlightState climb = LevelFlight(75.0f);
    FlightControls up;
    up.pitch = 1.0f;
    Fly(climb, up, 1.2f);
    FlightControls hold;
    float lowest = 999.0f;
    for (int i = 0; i < 60 * 8; i++) {
        UpdateFlight(climb, PLAYER_FLIGHT, hold, STEP);
        lowest = fminf(lowest, climb.speed);
    }
    Check(lowest >= MIN_SPEED, "climb never stalls below min (m/s)", lowest);
    Check(QuatForward(climb.rotation).y < 0.9f, "nose drops when slow (fwd.y)", QuatForward(climb.rotation).y);
}

static void TestTurns()
{
    printf("turns\n");
    FlightControls turn;
    turn.turn = 1.0f;
    float full = TimeToTurn(LevelFlight(75.0f), turn, 2.0f * PI);
    Check(full > 4.0f && full < 8.0f, "360 deg turn at cruise, stick only (s)", full);

    FlightState slow = LevelFlight(52.0f);
    FlightControls brakeTurn;
    brakeTurn.turn = 1.0f;
    brakeTurn.throttle = -1.0f;
    float half = TimeToTurn(slow, brakeTurn, PI);
    Check(half > 2.0f && half < 3.3f, "180 deg, brake + full turn (s, brief 2.6)", half);

    FlightControls hard = brakeTurn;
    hard.pitch = 1.0f;
    float hardHalf = TimeToTurn(LevelFlight(52.0f), hard, PI);
    Check(hardHalf < half, "180 deg, brake + turn + pull (s)", hardHalf);

    // Pitch rate at the best-turn speed: a pull from level, measured over 0.5 s.
    FlightState s = LevelFlight(70.0f);
    FlightControls pull;
    pull.pitch = 1.0f;
    Fly(s, pull, 0.1f);
    float before = asinf(QuatForward(s.rotation).y);
    Fly(s, pull, 0.5f);
    float rate = (asinf(QuatForward(s.rotation).y) - before) / 0.5f / DEG;
    Check(rate > 60.0f && rate < 80.0f, "pitch rate at 70 m/s (deg/s, brief 75)", rate);

    FlightState r = LevelFlight(75.0f);
    FlightControls roll;
    roll.turn = 1.0f;
    float t = 0.0f;
    while (BankAngle(r.rotation) < 60.0f * DEG && t < 3.0f) {
        UpdateFlight(r, PLAYER_FLIGHT, roll, STEP);
        t += STEP;
    }
    Check(t < 0.6f, "time to 60 deg bank (s)", t);
}

static void TestAutoLevel()
{
    printf("auto-level\n");
    FlightState s = LevelFlight(75.0f);
    FlightControls turn;
    turn.turn = 1.0f;
    Fly(s, turn, 2.0f);
    Check(BankAngle(s.rotation) > 60.0f * DEG, "bank while turning (deg)", BankAngle(s.rotation) / DEG);
    Fly(s, FlightControls(), 1.2f);
    float bank = fabsf(BankAngle(s.rotation)) / DEG;
    Check(bank < 6.0f, "bank 1.2 s after release (deg)", bank);
}

static float RunStunt(FlightState& s, Stunt stunt)
{
    FlightControls c;
    c.stunt = stunt;
    UpdateFlight(s, PLAYER_FLIGHT, c, STEP);
    float t = STEP;
    while (s.stunt != Stunt::NONE && t < 10.0f) {
        UpdateFlight(s, PLAYER_FLIGHT, FlightControls(), STEP);
        t += STEP;
    }
    return t;
}

static void TestStunts()
{
    printf("stunts\n");
    FlightState s = LevelFlight(75.0f);
    FlightControls begin;
    begin.stunt = Stunt::LOOP;
    UpdateFlight(s, PLAYER_FLIGHT, begin, STEP);
    Check(fabsf(s.meter - (1.0f - STUNT_COST)) < 0.01f, "meter right after a stunt starts", s.meter);

    s = LevelFlight(75.0f);
    float heading = Heading(s);
    float t = RunStunt(s, Stunt::LOOP);
    Check(t < 3.0f, "loop time (s)", t);
    Check(fabsf(WrapPi(Heading(s) - heading)) < 3.0f * DEG, "loop keeps heading (deg)",
          WrapPi(Heading(s) - heading) / DEG);
    Check(fabsf(BankAngle(s.rotation)) < 2.0f * DEG, "loop ends wings level (deg)", BankAngle(s.rotation) / DEG);
    Check(s.stuntsDone == 1, "loop counted", static_cast<float>(s.stuntsDone));

    s = LevelFlight(75.0f);
    float y = s.position.y;
    t = RunStunt(s, Stunt::IMMELMANN);
    float turned = fabsf(WrapPi(Heading(s) - heading)) / DEG;
    Check(fabsf(turned - 180.0f) < 3.0f, "Immelmann reverses heading (deg)", turned);
    Check(fabsf(BankAngle(s.rotation)) < 2.0f * DEG, "Immelmann ends upright (deg)", BankAngle(s.rotation) / DEG);
    Check(s.position.y - y > 30.0f, "Immelmann gains height (m)", s.position.y - y);
    Check(t < 2.2f, "Immelmann time (s)", t);

    s = LevelFlight(75.0f);
    Vec3 right = QuatRight(s.rotation);
    Vec3 start = s.position;
    t = RunStunt(s, Stunt::ROLL_RIGHT);
    float side = Dot(s.position - start, right);
    Check(fabsf(side - 16.0f) < 2.0f, "barrel roll sidestep (m)", side);
    Check(fabsf(WrapPi(Heading(s) - heading)) < 2.0f * DEG, "barrel roll keeps heading (deg)",
          WrapPi(Heading(s) - heading) / DEG);
    Check(fabsf(BankAngle(s.rotation)) < 2.0f * DEG, "barrel roll ends upright (deg)", BankAngle(s.rotation) / DEG);
    Check(t < 1.0f, "barrel roll time (s)", t);

    // The meter allows three stunts in a row, then refuses.
    s = LevelFlight(75.0f);
    int done = 0;
    for (int i = 0; i < 4; i++) {
        RunStunt(s, Stunt::ROLL_LEFT);
        done = s.stuntsDone;
    }
    Check(done == 3, "stunts from a full meter", static_cast<float>(done));
}

int main()
{
    TestSpeeds();
    TestTurns();
    TestAutoLevel();
    TestStunts();
    printf("%s (%d failed)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
