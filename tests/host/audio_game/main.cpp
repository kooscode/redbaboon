// The game's start-up and the player's path through the real audio code on the host: audio::Init, the title theme
// over the loading, the title screen with its fly-by, Start (a mission), and the mission flown by the autopilot as
// a player would. The mixer is pulled for each 60 Hz frame as the device would; every second's level is printed so
// a silent stretch shows. Usage: audio_game OUT.wav
#include "audio/audio.h"
#include "audio/mixer.h"
#include "game/game.h"

#include <math.h>
#include <stdio.h>
#include <vector>

namespace audio {

// The null device: this test pulls the frames itself.
bool DeviceOpen()
{
    return true;
}

void DeviceRunInBackground(void (*work)())
{
    work();
}

void DeviceWaitForBackground()
{
    // Nothing runs in the background.
}

void DeviceLog(const char* text)
{
    printf("  %s\n", text);
}

void DeviceStatus(char* text, int size)
{
    snprintf(text, (size_t)size, "host");
}

} // namespace audio

namespace {

// Output frames per 60 Hz frame.
constexpr int FRAME = 800;

// Everything mixed so far, interleaved stereo.
std::vector<int16_t> recording;

// Seconds mixed so far.
int second = 0;

// Whether to print each second's level; and the 50 ms levels printed from fineFrom on (the results transition).
bool printLevels = true;
bool finePrint = false;
size_t fineFrom = 0;

// Prints the level of every 50 ms from fineFrom to the end of the recording.
void PrintFine()
{
    size_t step = (size_t)audio::MIX_RATE / 20 * 2;
    for (size_t at = fineFrom; at + step <= recording.size(); at += step) {
        double sum = 0.0;
        for (size_t i = at; i < at + step; ++i)
            sum += (double)recording[i] * recording[i];
        printf("  %+6.2f s %6.1f dBFS\n", (double)((long)at - (long)fineFrom) / (2.0 * audio::MIX_RATE) - 1.0,
               10.0 * log10(sum / (double)step / 1073741824.0 + 1e-12));
    }
}

// Mixes one frame of audio and prints the level of each finished second.
void MixFrame()
{
    static int16_t buffer[FRAME * 2];
    audio::MixerRender(buffer, FRAME);
    recording.insert(recording.end(), buffer, buffer + FRAME * 2);
    size_t perSecond = (size_t)audio::MIX_RATE * 2;
    if (recording.size() < (size_t)(second + 1) * perSecond)
        return;
    double sum = 0.0;
    for (size_t i = (size_t)second * perSecond; i < (size_t)(second + 1) * perSecond; ++i)
        sum += (double)recording[i] * recording[i];
    if (printLevels)
        printf("  %3d s  %6.1f dBFS RMS\n", second, 10.0 * log10(sum / (double)perSecond / 1073741824.0 + 1e-12));
    ++second;
}

// Level (dBFS) of the 20 ms window of the recording starting at `at` (an index into the interleaved samples).
double WindowLevel(size_t at)
{
    size_t length = (size_t)audio::MIX_RATE / 50 * 2;
    double sum = 0.0;
    for (size_t i = at; i < at + length && i < recording.size(); ++i)
        sum += (double)recording[i] * recording[i];
    return 10.0 * log10(sum / (double)length / 1073741824.0 + 1e-12);
}

// Mixes `seconds` of a screen of the game (`step` runs before each frame) and prints the largest fall in level from
// one 20 ms window to the next over its first second: a cut shows as a fall of tens of dB.
template <typename F>
void CheckTransition(const char* name, float seconds, F step)
{
    size_t start = recording.size() >= (size_t)audio::MIX_RATE / 5 ? recording.size() - (size_t)audio::MIX_RATE / 5 : 0;
    for (int f = 0; f < (int)(seconds * 60.0f); ++f) {
        step(f);
        audio::Update(1.0f / 60.0f);
        MixFrame();
    }
    size_t window = (size_t)audio::MIX_RATE / 50 * 2;
    double worst = 0.0;
    size_t end = start + (size_t)audio::MIX_RATE * 2;
    for (size_t at = start; at + 2 * window <= end && at + 2 * window <= recording.size(); at += window) {
        double fall = WindowLevel(at) - WindowLevel(at + window);
        if (fall > worst)
            worst = fall;
    }
    printf("  %-28s largest 20 ms fall %5.1f dB, level after %6.1f dBFS\n", name, worst,
           WindowLevel(recording.size() - window));
}

void WriteWav(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    uint32_t bytes = (uint32_t)(recording.size() * 2);
    uint32_t header[11] = {0x46464952u, 36 + bytes, 0x45564157u, 0x20746D66u, 16, 1u | (2u << 16),
                           (uint32_t)audio::MIX_RATE, (uint32_t)audio::MIX_RATE * 4, 4u | (16u << 16), 0x61746164u,
                           bytes};
    fwrite(header, sizeof(header), 1, f);
    fwrite(recording.data(), 2, recording.size(), f);
    fclose(f);
}

} // namespace

int main(int argc, char** argv)
{
    audio::Init();
    audio::SetMusic(audio::Music::TITLE);
    game::LoadAssets(nullptr);
    audio::WaitUntilLoaded();
    float dt = 1.0f / 60.0f;
    printf("Title screen:\n");
    game::StartTitle();
    for (int f = 0; f < 10 * 60; ++f) {
        game::UpdateTitle(dt);
        audio::Update(dt);
        MixFrame();
    }
    printf("Mission (Start pressed):\n");
    game::StartMission(game::FIXED_SEED);
    bool quiet = printLevels;
    printLevels = false;
    int frames = 0;
    while (game::MissionOutcome() == game::Outcome::PLAYING && frames < 15 * 60 * 60) {
        game::UpdateMission(game::AutopilotInput(), dt);
        audio::Update(dt);
        MixFrame();
        ++frames;
    }
    printLevels = quiet;
    printf("Mission over after %d s (%s); results, level every 50 ms from 1 s before:\n", frames / 60,
           game::MissionOutcome() == game::Outcome::VICTORY ? "victory" : "defeat");
    finePrint = true;
    fineFrom = recording.size() - (size_t)audio::MIX_RATE * 2;
    game::ShowResultsFlyby();
    audio::SetMusicNext(audio::Music::TITLE);
    for (int f = 0; f < 4 * 60; ++f) {
        game::UpdateResults((float)f / 60.0f, dt);
        audio::Update(dt);
        MixFrame();
    }
    PrintFine();
    printf("Screen changes:\n");
    CheckTransition("results -> restart", 20.0f, [](int f) {
        if (f == 0)
            game::StartMission(game::FIXED_SEED);
        game::UpdateMission(game::AutopilotInput(), 1.0f / 60.0f);
    });
    CheckTransition("mission -> pause", 3.0f, [](int f) {
        if (f == 0)
            game::PauseMission();
    });
    CheckTransition("pause -> title", 6.0f, [](int f) {
        if (f == 0) {
            game::StopMission();
            game::StartTitle();
        }
        game::UpdateTitle(1.0f / 60.0f);
    });
    if (argc > 1)
        WriteWav(argv[1]);
    return 0;
}
