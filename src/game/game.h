// Game: the mission "Raid on Isla Platano" (docs/design.md §2, §3, §8). What main.cpp and the host tests use:
// asset loading, the mission's fixed-step update and drawing, the title fly-by, the menu screens and the autopilot.
#pragma once

#include "platform/input.h"

#include <stdint.h>

namespace game {

// The fixed simulation step (docs/design.md §4).
constexpr float STEP = 1.0f / 60.0f;

// --- Assets

// Called between loading steps (what is being built, 0..1 done) so main.cpp can draw a loading screen.
using ProgressFunction = void (*)(const char* what, float fraction);

// Makes the title logo texture, so the loading screen can show it (call right after render::Init).
void LoadLogo();

// Generates every mesh and texture (content) and uploads them (render). Returns false on failure.
bool LoadAssets(ProgressFunction progress);

// --- Mission seeds (seed.cpp)

// The seed of the title fly-by, and of test runs with seed=fixed in autoplay.txt.
constexpr uint32_t FIXED_SEED = 0x5EED1234u;

// autoplay.txt: key=value lines, # comments; seed=fixed, seed=0x1234ABCD or seed=<decimal>. No (valid) seed line:
// seedSet is false and each mission takes a clock seed.
struct AutoplaySettings {
    bool seedSet = false;
    uint32_t seed = 0;
};
AutoplaySettings ParseAutoplaySettings(const char* text, int length);
// A 32-bit seed from two 64-bit readings (clock counters), mixed with splitmix64.
uint32_t MixSeed(uint64_t a, uint64_t b);

// --- Mission

enum class Outcome {
    PLAYING,
    VICTORY, // the zeppelin is down and the victory cinematic has finished
    DEFEAT,  // the last life is lost
};

struct Results {
    int score = 0;
    int kills = 0;
    int aces = 0;
    int rings = 0;
    int ringCount = 0;
    int stunts = 0;
    int shotsFired = 0;
    int shotsHit = 0;
    int livesLeft = 0;
    int timeBonus = 0;
    float time = 0.0f;   // mission time in seconds
    int rank = 0;        // index into RankName
};

// Frames: every Draw function below starts the frame (render::BeginFrame) unless it says it is an overlay;
// main.cpp ends each frame with render::EndFrame.

// Starts a mission whose randomness comes from seed (logged as "seed XXXXXXXX").
void StartMission(uint32_t seed);
// One fixed step of the mission with this frame's controller state.
void UpdateMission(const input::State& pad, float dt);
// Draws the world and the HUD. alpha (0..1): how far the frame is between the last two steps.
void DrawMission(float alpha);
Outcome MissionOutcome();
Results MissionResults();
const char* RankName(int rank);
// Silences the engines while the game is paused (the next step turns them back on).
void PauseMission();
// Stops engine sounds and music started by the mission (when leaving it for a menu).
void StopMission();

// --- Title fly-by (a slow camera around the island)

void StartTitle();
void UpdateTitle(float dt);
void DrawTitleScene(float alpha);

// --- Menus and screens (menus.cpp)

void DrawLoading(const char* what, float fraction);
// Overlay on the title scene: logo and PRESS START.
void DrawTitleScreen(float time);
// Overlay on the title scene: the mission briefing.
void DrawBriefing(float time);
// Overlay on the paused mission; selection: 0 Resume, 1 Invert Y, 2 Controls, 3 Restart, 4 Quit.
void DrawPauseMenu(int selection);
// Overlay on the paused mission: the controls diagram.
void DrawControlsScreen(float time);
// Overlay on the island fly-by (ShowResultsFlyby): values count up, then the rank stamp slams in.
void DrawResults(const Results& results, float time);
// The results' sounds (count-up ticks, the stamp's thud); call once per frame with the screen's time.
void UpdateResults(float time, float dt);
// Switches the scene behind the results to the slow island fly-by (the mission keeps running under it).
void ShowResultsFlyby();
// Overlay; selection: 0 Retry, 1 Quit.
void DrawDefeat(const Results& results, int selection, float time);
// Overlay during the attract demo.
void DrawDemoBanner(float time);

constexpr int PAUSE_ITEMS = 5;
constexpr int DEFEAT_ITEMS = 2;

// --- Settings

void SetInvertY(bool invert);
bool InvertY();

// --- Autopilot and logging (tests, attract mode)

// The controller input an AI pilot gives for the player's plane this step (same path as a real pad).
input::State AutopilotInput();

// Receives one line per mission event (phase changes, kills, deaths); nullptr turns logging off.
using LogFunction = void (*)(const char* line);
void SetLog(LogFunction log);

} // namespace game
