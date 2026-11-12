// RedBaboon: start-up and the main loop (docs/design.md §8). Initialises the GPU, SDL3 (the controller and timers
// only: the GPU is pbkit's and the sound goes to the AC'97 directly), the sound bank and the content, then runs a
// fixed 1/60 s update with an accumulator and draws once per frame, interpolated between steps. The screens: title
// (island fly-by), briefing, mission, pause, results, defeat, and the attract demo (the autopilot flies the mission
// after 20 s on the title). Autoplay (a D:\autoplay.txt file or a REDBABOON_AUTOPLAY=1 build) skips the title and
// flies the mission on autopilot forever, logging each run's timeline to the debug output (the serial log).
// Every mission takes a seed from the clock (logged as "seed XXXXXXXX"), unless autoplay.txt names one: its
// key=value lines (# comments) know seed=fixed (5eed1234, repeatable test runs), seed=0x1234ABCD and seed=<decimal>;
// an empty file, or no seed line, keeps the clock seed.
#include "audio/audio.h"
#include "game/game.h"
#include "platform/input.h"
#include "render/render.h"

#include <SDL3/SDL.h>
#include <windows.h>

#include <math.h>
#include <stdio.h>

#ifndef REDBABOON_AUTOPLAY
#define REDBABOON_AUTOPLAY 0
#endif

namespace {

constexpr float MAX_FRAME_TIME = 0.1f;    // longer frames are clamped (no spiral of death after a hitch)
constexpr float ATTRACT_DELAY = 20.0f;    // seconds of no input on the title before the demo starts
constexpr float RESULTS_MIN_TIME = 3.0f;  // seconds before the results can be dismissed
constexpr float AUTOPLAY_RESULTS_TIME = 10.0f; // autoplay shows the results this long before the next mission
constexpr float AUTOPLAY_BRIEFING_TIME = 6.0f; // and the briefing (controls diagram) this long before each mission
constexpr float STICK_MENU = 0.5f;        // stick deflection that moves a menu cursor
constexpr const char* AUTOPLAY_FILE = "D:\\autoplay.txt";
constexpr int AUTOPLAY_FILE_MAX = 1024;   // bytes of autoplay.txt read (settings beyond are ignored)
constexpr int STATS_FRAMES = 600;         // autoplay logs the frame rate every this many frames

enum class Screen {
    TITLE,
    BRIEFING,
    PLAYING,
    PAUSED,
    RESULTS,
    DEFEAT,
    DEMO,     // attract mode: the autopilot flies until any input
    AUTOPLAY, // the autopilot flies mission after mission (overnight tests)
    AUTOPLAY_RESULTS, // autoplay holds the results (or defeat) screen, then flies again
    AUTOPLAY_BRIEFING, // autoplay shows the briefing before each mission, so recordings include it
    CONTROLS,         // the controls diagram, from the pause menu
};

struct App {
    Screen screen = Screen::TITLE;
    float screenTime = 0.0f;  // seconds on the current screen
    float idleTime = 0.0f;    // seconds without input (title attract)
    int selection = 0;
    float lastStickY = 0.0f;  // menu stick of the previous frame (moves are edges)
    bool pending[static_cast<int>(input::Button::COUNT)] = {}; // presses not yet seen by a step
    game::Results results;
    int runs = 0;             // autoplay missions flown
    bool victory = false;     // how the last mission ended (autoplay results)
    int statsFrames = 0;      // frames and seconds since the last frame-rate line
    float statsTime = 0.0f;
    float slowest = 0.0f;     // longest frame in that time
    game::AutoplaySettings autoplay; // autoplay.txt's settings (a fixed seed for test runs)
    uint32_t missions = 0;    // missions started (mixed into the clock seed)
};

// The application's state.
App app;

// --- Helpers

void DebugLine(const char* line)
{
    char text[200];
    snprintf(text, sizeof(text), "RedBaboon: %s\n", line);
    OutputDebugStringA(text);
}

// Logs a start-up step with the milliseconds since the title started.
void LogTime(const char* what)
{
    char line[96];
    snprintf(line, sizeof(line), "%s, %llu ms", what, static_cast<unsigned long long>(SDL_GetTicks()));
    DebugLine(line);
}

// Starts SDL's gamepad support (only that: never SDL's video or audio, which would take over pbkit's GPU and the
// AC'97) and opens the pad (a thread of its own at start-up).
DWORD WINAPI InputStartup(LPVOID parameter)
{
    (void)parameter;
    if (!SDL_Init(SDL_INIT_GAMEPAD))
        DebugLine("SDL init failed");
    input::Init();
    LogTime("controller start-up done");
    return 0;
}

// Draws the loading screen and logs how long the step before took (start-up time on the console).
void LoadingProgress(const char* what, float fraction)
{
    unsigned long long now = SDL_GetTicks();
    char line[96];
    snprintf(line, sizeof(line), "loading %s (%d%%), %llu ms", what, static_cast<int>(fraction * 100.0f), now);
    DebugLine(line);
    game::DrawLoading(what, fraction);
    render::EndFrame();
}

bool Pressed(const input::State& pad, input::Button button)
{
    return pad.pressed[static_cast<int>(button)];
}

bool AnyInput(const input::State& pad)
{
    for (int i = 0; i < static_cast<int>(input::Button::COUNT); i++) {
        if (pad.pressed[i])
            return true;
    }
    float stick = fabsf(pad.leftX) + fabsf(pad.leftY) + fabsf(pad.rightX) + fabsf(pad.rightY);
    return stick > 0.5f || pad.leftTrigger > 0.3f || pad.rightTrigger > 0.3f;
}

// Menu cursor moves from the d-pad or the left stick. Returns -1, 0 or 1.
int MenuMove(const input::State& pad)
{
    int move = 0;
    if (Pressed(pad, input::Button::DPAD_UP) || (pad.leftY < -STICK_MENU && app.lastStickY >= -STICK_MENU))
        move = -1;
    if (Pressed(pad, input::Button::DPAD_DOWN) || (pad.leftY > STICK_MENU && app.lastStickY <= STICK_MENU))
        move = 1;
    app.lastStickY = pad.leftY;
    if (move != 0)
        audio::Play(audio::Sfx::MENU_MOVE, 0.6f);
    return move;
}

bool Confirm(const input::State& pad)
{
    bool yes = Pressed(pad, input::Button::A) || Pressed(pad, input::Button::START);
    if (yes)
        audio::Play(audio::Sfx::MENU_SELECT, 0.8f);
    return yes;
}

void Go(Screen screen)
{
    app.screen = screen;
    app.screenTime = 0.0f;
    app.selection = 0;
}

void ToTitle()
{
    game::StopMission();
    game::StartTitle();
    app.idleTime = 0.0f;
    Go(Screen::TITLE);
}

// The next mission's seed: autoplay.txt's, or else the clock (the performance counter, the CPU's time-stamp counter
// and SDL's nanosecond ticks, mixed).
uint32_t MissionSeed()
{
    if (app.autoplay.seedSet)
        return app.autoplay.seed;
    app.missions++;
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    uint64_t clock = static_cast<uint64_t>(counter.QuadPart) ^ (static_cast<uint64_t>(app.missions) << 48);
    return game::MixSeed(clock ^ __builtin_ia32_rdtsc(), SDL_GetTicksNS());
}

void ToMission(Screen screen)
{
    game::StartMission(MissionSeed());
    Go(screen);
}

void ToAutoplayBriefing()
{
    game::StopMission();
    game::StartTitle();
    Go(Screen::AUTOPLAY_BRIEFING);
}

// --- Screens (once per frame)

void FrameTitle(const input::State& pad, float dt)
{
    app.idleTime = AnyInput(pad) ? 0.0f : app.idleTime + dt;
    if (app.screen == Screen::TITLE) {
        if (Confirm(pad))
            Go(Screen::BRIEFING);
        else if (app.idleTime > ATTRACT_DELAY)
            ToMission(Screen::DEMO);
        return;
    }
    if (Pressed(pad, input::Button::B)) {
        Go(Screen::TITLE);
        return;
    }
    if (app.screenTime > 0.3f && Confirm(pad))
        ToMission(Screen::PLAYING);
}

void FramePaused(const input::State& pad)
{
    app.selection = (app.selection + MenuMove(pad) + game::PAUSE_ITEMS) % game::PAUSE_ITEMS;
    if (Pressed(pad, input::Button::B) || Pressed(pad, input::Button::START)) {
        Go(Screen::PLAYING);
        return;
    }
    if (!Pressed(pad, input::Button::A))
        return;
    audio::Play(audio::Sfx::MENU_SELECT, 0.8f);
    switch (app.selection) {
    case 0:
        Go(Screen::PLAYING);
        break;
    case 1:
        game::SetInvertY(!game::InvertY());
        break;
    case 2:
        Go(Screen::CONTROLS);
        break;
    case 3:
        ToMission(Screen::PLAYING);
        break;
    default:
        ToTitle();
        break;
    }
}

void FrameDefeat(const input::State& pad)
{
    app.selection = (app.selection + MenuMove(pad) + game::DEFEAT_ITEMS) % game::DEFEAT_ITEMS;
    if (app.screenTime < 1.0f || !Confirm(pad))
        return;
    if (app.selection == 0)
        ToMission(Screen::PLAYING);
    else
        ToTitle();
}

// Checks how a flown mission ended and moves on.
void CheckOutcome()
{
    game::Outcome outcome = game::MissionOutcome();
    if (outcome == game::Outcome::PLAYING)
        return;
    app.results = game::MissionResults();
    if (app.screen == Screen::AUTOPLAY) {
        char line[160];
        snprintf(line, sizeof(line), "run %d: %s, score %d, kills %d, rings %d, time %d s, rank %s", ++app.runs,
                 outcome == game::Outcome::VICTORY ? "VICTORY" : "DEFEAT", app.results.score, app.results.kills,
                 app.results.rings, static_cast<int>(app.results.time), game::RankName(app.results.rank));
        DebugLine(line);
        app.victory = outcome == game::Outcome::VICTORY;
        if (app.victory) {
            game::ShowResultsFlyby();
            audio::SetMusicNext(audio::Music::TITLE);
        }
        Go(Screen::AUTOPLAY_RESULTS);
        return;
    }
    if (app.screen == Screen::DEMO) {
        ToTitle();
        return;
    }
    // The results show over the island fly-by, and the title theme follows the victory fanfare.
    if (outcome == game::Outcome::VICTORY) {
        game::ShowResultsFlyby();
        audio::SetMusicNext(audio::Music::TITLE);
    }
    Go(outcome == game::Outcome::VICTORY ? Screen::RESULTS : Screen::DEFEAT);
}

void Frame(const input::State& pad, float dt)
{
    app.screenTime += dt;
    switch (app.screen) {
    case Screen::TITLE:
    case Screen::BRIEFING:
        FrameTitle(pad, dt);
        break;
    case Screen::PLAYING:
        if (Pressed(pad, input::Button::START)) {
            game::PauseMission();
            audio::Play(audio::Sfx::MENU_SELECT, 0.8f);
            Go(Screen::PAUSED);
        }
        CheckOutcome();
        break;
    case Screen::PAUSED:
        FramePaused(pad);
        break;
    case Screen::CONTROLS:
        if (Pressed(pad, input::Button::B) || Pressed(pad, input::Button::A) || Pressed(pad, input::Button::START)) {
            audio::Play(audio::Sfx::MENU_SELECT, 0.8f);
            Go(Screen::PAUSED);
            app.selection = 2;
        }
        break;
    case Screen::RESULTS:
        game::UpdateResults(app.screenTime, dt);
        if (app.screenTime > RESULTS_MIN_TIME && Confirm(pad))
            ToTitle();
        break;
    case Screen::DEFEAT:
        FrameDefeat(pad);
        break;
    case Screen::DEMO:
        if (app.screenTime > 1.0f && AnyInput(pad))
            ToTitle();
        else
            CheckOutcome();
        break;
    case Screen::AUTOPLAY:
        CheckOutcome();
        break;
    case Screen::AUTOPLAY_RESULTS:
        if (app.victory)
            game::UpdateResults(app.screenTime, dt);
        if (app.screenTime > AUTOPLAY_RESULTS_TIME)
            ToAutoplayBriefing();
        break;
    case Screen::AUTOPLAY_BRIEFING:
        if (app.screenTime > AUTOPLAY_BRIEFING_TIME)
            ToMission(Screen::AUTOPLAY);
        break;
    }
}

// --- Steps (fixed 1/60 s)

void Step(const input::State& pad)
{
    switch (app.screen) {
    case Screen::TITLE:
    case Screen::BRIEFING:
    case Screen::AUTOPLAY_BRIEFING:
        game::UpdateTitle(game::STEP);
        break;
    case Screen::PLAYING:
        game::UpdateMission(pad, game::STEP);
        break;
    case Screen::RESULTS:
    case Screen::DEFEAT:
    case Screen::AUTOPLAY_RESULTS:
        game::UpdateMission(input::State(), game::STEP);
        break;
    case Screen::DEMO:
    case Screen::AUTOPLAY:
        game::UpdateMission(game::AutopilotInput(), game::STEP);
        break;
    case Screen::PAUSED:
    case Screen::CONTROLS:
        break;
    }
}

void Draw(float alpha)
{
    switch (app.screen) {
    case Screen::TITLE:
        game::DrawTitleScene(alpha);
        game::DrawTitleScreen(app.screenTime);
        break;
    case Screen::BRIEFING:
    case Screen::AUTOPLAY_BRIEFING:
        game::DrawTitleScene(alpha);
        game::DrawBriefing(app.screenTime);
        break;
    case Screen::PLAYING:
    case Screen::AUTOPLAY:
        game::DrawMission(alpha);
        break;
    case Screen::DEMO:
        game::DrawMission(alpha);
        game::DrawDemoBanner(app.screenTime);
        break;
    case Screen::PAUSED:
        game::DrawMission(alpha);
        game::DrawPauseMenu(app.selection);
        break;
    case Screen::CONTROLS:
        game::DrawMission(alpha);
        game::DrawControlsScreen(app.screenTime);
        break;
    case Screen::RESULTS:
        game::DrawMission(alpha);
        game::DrawResults(app.results, app.screenTime);
        break;
    case Screen::DEFEAT:
        game::DrawMission(alpha);
        game::DrawDefeat(app.results, app.selection, app.screenTime);
        break;
    case Screen::AUTOPLAY_RESULTS:
        game::DrawMission(alpha);
        if (app.victory)
            game::DrawResults(app.results, app.screenTime);
        else
            game::DrawDefeat(app.results, 0, app.screenTime);
        break;
    }
    render::EndFrame();
}

// Autoplay: a line with the frame rate, the slowest frame and the renderer's counters every few seconds.
void LogFrameStats(float dt)
{
    app.statsFrames++;
    app.statsTime += dt;
    app.slowest = dt > app.slowest ? dt : app.slowest;
    if (app.statsFrames < STATS_FRAMES)
        return;
    render::FrameStats stats = render::LastFrameStats();
    // Integers only: nxdk's printf has no floating-point conversions.
    char line[128];
    int fpsTenths = static_cast<int>(app.statsFrames * 10.0f / app.statsTime);
    snprintf(line, sizeof(line), "%d.%d fps, slowest %d ms, %d draws, %d triangles, %d culled, gpu wait %d.%d ms",
             fpsTenths / 10, fpsTenths % 10, static_cast<int>(app.slowest * 1000.0f), stats.drawCalls,
             stats.triangles, stats.culled, static_cast<int>(stats.gpuWaitMs),
             static_cast<int>(stats.gpuWaitMs * 10.0f) % 10);
    DebugLine(line);
    app.statsFrames = 0;
    app.statsTime = 0.0f;
    app.slowest = 0.0f;
}

// True for autoplay: a REDBABOON_AUTOPLAY build, or D:\autoplay.txt (its settings go to app.autoplay).
bool ReadAutoplay()
{
    FILE* file = fopen(AUTOPLAY_FILE, "r");
    if (!file)
        return REDBABOON_AUTOPLAY != 0;
    static char text[AUTOPLAY_FILE_MAX]; // not on the main thread's stack (see the Makefile)
    int length = static_cast<int>(fread(text, 1, sizeof(text), file));
    fclose(file);
    app.autoplay = game::ParseAutoplaySettings(text, length);
    if (app.autoplay.seedSet) {
        char line[48];
        snprintf(line, sizeof(line), "autoplay seed %08x", static_cast<unsigned>(app.autoplay.seed));
        DebugLine(line);
    }
    return true;
}

} // namespace

// --- Main

int main()
{
    DebugLine("start");
    if (!render::Init()) {
        DebugLine("render init failed");
        return 1;
    }
    LogTime("render ready");
    // The loading screen first (the logo is made before anything else), so the screen is never black for long.
    game::LoadLogo();
    LoadingProgress("Starting", 0.0f);
    // The controller start-up mostly waits (SDL polls the USB hubs for half a second in 1 ms sleeps, which takes
    // about 1.7 s), so it runs on its own thread while the sound bank and the content are built.
    HANDLE inputThread = CreateThread(nullptr, 0, InputStartup, nullptr, 0, nullptr);
    if (!inputThread)
        InputStartup(nullptr);
    if (!audio::Init())
        DebugLine("audio unavailable, running silent");
    LogTime("audio ready");
    // The title theme plays over the loading screen.
    audio::SetMusic(audio::Music::TITLE);
    game::SetLog(DebugLine);
    if (!game::LoadAssets(LoadingProgress))
        DebugLine("content upload incomplete");
    // The sound bank builds on its own thread during the content upload; make sure it is complete.
    audio::WaitUntilLoaded();
    LogTime("sound bank ready");
    if (inputThread) {
        WaitForSingleObject(inputThread, INFINITE);
        CloseHandle(inputThread);
    }
    LogTime("input ready");

    if (ReadAutoplay()) {
        DebugLine("autoplay");
        ToAutoplayBriefing();
    } else {
        ToTitle();
    }

    float accumulator = 0.0f;
    Uint64 frequency = SDL_GetPerformanceFrequency();
    Uint64 last = SDL_GetPerformanceCounter();
    while (true) {
        input::Update();
        const input::State& pad = input::Get();
        for (int i = 0; i < static_cast<int>(input::Button::COUNT); i++)
            app.pending[i] = app.pending[i] || pad.pressed[i];

        Uint64 now = SDL_GetPerformanceCounter();
        float dt = static_cast<float>(now - last) / static_cast<float>(frequency);
        last = now;
        if (dt > MAX_FRAME_TIME)
            dt = MAX_FRAME_TIME;
        if (dt < 0.0f)
            dt = 0.0f;

        Screen before = app.screen;
        Frame(pad, dt);
        if (app.screen != before) {
            accumulator = 0.0f;
            for (bool& press : app.pending)
                press = false;
        }
        accumulator += dt;
        while (accumulator >= game::STEP) {
            // Each press reaches exactly one step, even when a frame runs none or several.
            input::State stepPad = pad;
            for (int i = 0; i < static_cast<int>(input::Button::COUNT); i++) {
                stepPad.pressed[i] = app.pending[i];
                app.pending[i] = false;
            }
            Step(stepPad);
            accumulator -= game::STEP;
        }
        audio::Update(dt);
        Draw(accumulator / game::STEP);
        if (app.screen == Screen::AUTOPLAY || app.screen == Screen::AUTOPLAY_RESULTS)
            LogFrameStats(dt);
    }
    return 0;
}
