// Headless mission simulation (docs/design.md §9): flies "Raid on Isla Platano" at fixed steps with the null
// renderer and audio. The autopilot (game::AutopilotInput, the same input path as a pad) must win, with every plane
// finite and inside the world, waves progressing and the zeppelin sunk; with no input at all the player must be
// shot down, respawn and reach the defeat screen. Prints the mission timeline of each run.
#include "game/world.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace game;

constexpr float MAX_MINUTES = 15.0f;     // the autopilot must win within this
constexpr float IDLE_MINUTES = 25.0f;    // a player who does nothing must lose within this
constexpr int DRAW_EVERY = 20;           // steps between drawn frames (exercises the drawing and HUD code)
constexpr int SEEDS = 5;

// Failed checks so far, and whether to print the timeline.
static int failures = 0;
static bool printLog = true;

// --- Helpers

static void PrintLine(const char* line)
{
    if (printLog)
        printf("    %s\n", line);
}

static void Check(bool ok, const char* what)
{
    printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

static bool Finite(const Vec3& v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

// Every active plane and the zeppelin finite and inside the world.
static bool WorldSane(const char*& problem)
{
    for (int i = 0; i < MAX_PLANES; i++) {
        const Plane& p = world.planes[i];
        if (!p.active)
            continue;
        const Vec3& pos = p.flight.position;
        if (!Finite(pos) || !Finite(p.flight.velocity) || !isfinite(p.flight.rotation.w)) {
            problem = "non-finite plane state";
            return false;
        }
        if (fabsf(pos.x) > 6000.0f || fabsf(pos.z) > 6000.0f || pos.y > CEILING + 400.0f || pos.y < -50.0f) {
            problem = "plane out of bounds";
            return false;
        }
    }
    if (!Finite(world.zeppelin.position)) {
        problem = "non-finite zeppelin";
        return false;
    }
    return true;
}

struct RunResult {
    Outcome outcome = Outcome::PLAYING;
    float seconds = 0.0f;
    Phase lastPhase = Phase::INTRO;
    bool sane = true;
    int livesLost = 0;
    bool respawned = false;
    float phaseTime[8] = {};
};

static RunResult Run(bool autopilot, uint32_t seed, float minutes)
{
    RunResult r;
    StartMission(seed);
    int steps = static_cast<int>(minutes * 60.0f / STEP);
    int lives = world.lives;
    bool wasDead = false;
    for (int i = 0; i < steps; i++) {
        input::State pad = autopilot ? AutopilotInput() : input::State();
        UpdateMission(pad, STEP);
        if (i % DRAW_EVERY == 0)
            DrawMission(0.5f);
        Phase phase = CurrentPhase();
        if (phase != r.lastPhase) {
            r.phaseTime[static_cast<int>(phase)] = world.time;
            r.lastPhase = phase;
        }
        if (world.lives < lives) {
            r.livesLost += lives - world.lives;
            lives = world.lives;
        }
        if (!Player().alive)
            wasDead = true;
        else if (wasDead)
            r.respawned = true;
        const char* problem = nullptr;
        if (!WorldSane(problem)) {
            printf("  step %d: %s\n", i, problem);
            r.sane = false;
            break;
        }
        r.outcome = MissionOutcome();
        if (r.outcome != Outcome::PLAYING)
            break;
    }
    r.seconds = world.time;
    return r;
}

static void PrintResults()
{
    Results res = MissionResults();
    int accuracy = res.shotsFired ? res.shotsHit * 100 / res.shotsFired : 0;
    printf("  score %d (bonus %d), kills %d (aces %d), rings %d/%d, stunts %d, accuracy %d%%, lives %d, rank %s\n",
           res.score, res.timeBonus, res.kills, res.aces, res.rings, res.ringCount, res.stunts, accuracy,
           res.livesLeft, RankName(res.rank));
}

// --- Tests

static void TestAutopilotVictory()
{
    printf("autopilot flies the mission\n");
    float total = 0.0f;
    int wins = 0;
    for (int s = 0; s < SEEDS; s++) {
        printLog = s == 0;
        // The first run is the fixed seed of the test recordings (autoplay.txt seed=fixed), checked in detail.
        uint32_t seed = s == 0 ? FIXED_SEED : 0x1000u + s * 7919u;
        RunResult r = Run(true, seed, MAX_MINUTES);
        int m = static_cast<int>(r.seconds) / 60;
        int sec = static_cast<int>(r.seconds) % 60;
        printf("  seed %08x: %s at %d:%02d (wave2 %.0f s, wave3 %.0f s, boss %.0f s, crash %.0f s), lives lost %d\n",
               seed,
               r.outcome == Outcome::VICTORY ? "VICTORY" : (r.outcome == Outcome::DEFEAT ? "DEFEAT" : "UNFINISHED"),
               m, sec, r.phaseTime[static_cast<int>(Phase::WAVE2)], r.phaseTime[static_cast<int>(Phase::WAVE3)],
               r.phaseTime[static_cast<int>(Phase::BOSS)],
               r.phaseTime[static_cast<int>(Phase::CRASH)], r.livesLost);
        PrintResults();
        Check(r.sane, "planes finite and in bounds");
        if (r.outcome == Outcome::VICTORY) {
            wins++;
            total += r.seconds;
        }
        if (s == 0) {
            Check(world.stats.kills >= 14, "at least the fourteen wave fighters shot down");
            Check(r.phaseTime[static_cast<int>(Phase::WAVE2)] > 0.0f, "wave 2 reached");
            Check(r.phaseTime[static_cast<int>(Phase::WAVE3)] > 0.0f, "wave 3 reached");
            Check(r.phaseTime[static_cast<int>(Phase::BOSS)] > 0.0f, "boss reached");
            Check(world.zeppelin.state == ZeppelinState::SUNK, "zeppelin sunk");
        }
    }
    printLog = true;
    printf("  victories %d/%d, mean time %.0f s\n", wins, SEEDS, wins ? total / wins : 0.0f);
    Check(wins >= SEEDS - 1, "autopilot wins (all but at most one seed)");
}

static void TestIdleDefeat()
{
    printf("player does nothing\n");
    RunResult r = Run(false, 0x2222u, IDLE_MINUTES);
    int m = static_cast<int>(r.seconds) / 60;
    int sec = static_cast<int>(r.seconds) % 60;
    printf("  %s at %d:%02d, lives lost %d\n", r.outcome == Outcome::DEFEAT ? "DEFEAT" : "no defeat", m, sec,
           r.livesLost);
    Check(r.sane, "planes finite and in bounds");
    Check(r.respawned, "the player respawned");
    Check(r.livesLost == 3, "all three lives lost");
    Check(r.outcome == Outcome::DEFEAT, "defeat screen reached");
}

int main()
{
    SetLog(PrintLine);
    if (!LoadAssets(nullptr)) {
        printf("asset loading failed\n");
        return 1;
    }
    TestAutopilotVictory();
    TestIdleDefeat();
    printf("%s (%d failed)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
