// Audio demo: an Xbox title that checks the audio code without the game. It times the bank synthesis (built here on
// the main thread, so Init finds it done) and the mixer (5 s of a busy mix and the music transitions rendered on the
// main thread before the device opens), then opens the device and loops: music tracks in turn, effects every half
// second, a pirate engine circling the listener, a heartbeat line per second on screen and on the serial port. The
// heartbeat must keep coming when the device stops calling back.
#include "audio/audio.h"
#include "audio/mixer.h"
#include "audio/music.h"
#include "audio/sfx.h"

#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <math.h>
#include <stdio.h>

namespace {

// Prints a line on screen and on the serial log.
void Say(const char* text)
{
    debugPrint("%s\n", text);
    DbgPrint("audio_demo: %s\n", text);
}

double Seconds()
{
    LARGE_INTEGER count;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return (double)count.QuadPart / (double)frequency.QuadPart;
}

// Mixing buffer for the benchmark (static: the stack is small).
int16_t benchmarkBuffer[1024 * 2];

// Mixing buffer of one 60 Hz frame for the transition benchmark.
int16_t frameBuffer[800 * 2];

// The game's music path three times round (TITLE, COMBAT, BOSS, VICTORY then TITLE, NONE), rendered on this
// thread; prints the mixer's average and worst cost per 60 Hz frame in each state.
void BenchmarkTransitions()
{
    constexpr audio::Music STATES[5] = {
        audio::Music::TITLE, audio::Music::COMBAT, audio::Music::BOSS, audio::Music::TITLE, audio::Music::NONE,
    };
    constexpr const char* NAMES[5] = {"title", "combat", "boss", "victory+title", "none"};
    char line[128];
    for (int round = 0; round < 3; ++round) {
        for (int state = 0; state < 5; ++state) {
            if (state == 3)
                audio::MixerSetMusic(audio::Music::VICTORY);
            audio::MixerSetMusic(STATES[state]);
            double total = 0.0;
            double worst = 0.0;
            for (int f = 0; f < 6 * 60; ++f) {
                double start = Seconds();
                audio::MixerRender(frameBuffer, 800);
                double cost = Seconds() - start;
                total += cost;
                if (cost > worst)
                    worst = cost;
            }
            snprintf(line, sizeof(line), "round %d %s: %d us avg, %d us worst per frame", round + 1, NAMES[state],
                     (int)(total / 360.0 * 1e6), (int)(worst * 1e6));
            Say(line);
        }
    }
}

// The tracks the demo cycles through, ten seconds each.
constexpr audio::Music TRACKS[5] = {
    audio::Music::TITLE, audio::Music::COMBAT, audio::Music::BOSS, audio::Music::VICTORY, audio::Music::DEFEAT,
};

// Renders 5 s of combat music, guns and six engines on this thread and returns the milliseconds per audio second.
double BenchmarkMixer()
{
    audio::MixerSetMusic(audio::Music::COMBAT);
    for (int i = 0; i < audio::AUDIBLE_ENGINES; ++i) {
        audio::EngineParams p;
        p.gainL = 0.2f;
        p.gainR = 0.2f;
        p.throttleMix = 0.5f;
        p.lowpass = 0.5f;
        p.windGain = 0.1f;
        audio::MixerSetEngine(i, audio::EngineType::PIRATE, p);
    }
    const int buffers = 5 * audio::MIX_RATE / 1024;
    double start = Seconds();
    for (int b = 0; b < buffers; ++b) {
        if (b % 3 == 0) {
            audio::PlayCommand c;
            c.sample = &audio::SfxVariant(audio::Sfx::GUN, b % 4);
            c.step = (uint32_t)(65536.0f * audio::SFX_RATE / audio::MIX_RATE);
            c.gainL = 12000;
            c.gainR = 12000;
            audio::MixerPlay(c);
        }
        audio::MixerRender(benchmarkBuffer, 1024);
    }
    double elapsed = Seconds() - start;
    for (int i = 0; i < audio::AUDIBLE_ENGINES; ++i)
        audio::MixerSetEngine(i, audio::EngineType::PIRATE, audio::EngineParams());
    audio::MixerSetMusic(audio::Music::NONE);
    return elapsed * 1000.0 / 5.0;
}

} // namespace

int main()
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    char line[128];
    Say("RedBaboon audio demo");

    double start = Seconds();
    audio::BuildTitleMusic();
    audio::BuildMenuSounds();
    double title = Seconds() - start;
    audio::BuildSfxBank();
    audio::BuildMusic();
    snprintf(line, sizeof(line), "synthesis %d ms (title theme and menu sounds %d ms), bank %u KB",
             (int)((Seconds() - start) * 1000.0), (int)(title * 1000.0),
             (unsigned)((audio::SfxBankBytes() + audio::MusicBankBytes()) / 1024));
    Say(line);
    snprintf(line, sizeof(line), "mixer %d us per audio second (combat + 6 engines + guns)",
             (int)(BenchmarkMixer() * 1000.0));
    Say(line);
    BenchmarkTransitions();

    bool ok = audio::Init();
    Say(ok ? "audio device open" : "audio unavailable: running silent");
    audio::EngineId engine = audio::CreateEngine(audio::EngineType::PIRATE);

    double begin = Seconds();
    double last = begin;
    int second = 0;
    int track = -1;
    float nextEffect = 0.0f;
    int effect = 0;
    for (;;) {
        double now = Seconds();
        float t = (float)(now - begin);
        float dt = (float)(now - last);
        last = now;
        int wanted = (int)(t / 10.0f) % 5;
        if (wanted != track) {
            track = wanted;
            audio::SetMusic(TRACKS[track]);
        }
        if (t >= nextEffect) {
            audio::Play((audio::Sfx)(effect % (int)audio::Sfx::COUNT), 0.8f);
            ++effect;
            nextEffect += 0.5f;
        }
        Vec3 position(cosf(t * 0.5f) * 60.0f, 10.0f, sinf(t * 0.5f) * 60.0f);
        Vec3 velocity(-sinf(t * 0.5f) * 30.0f, 0.0f, cosf(t * 0.5f) * 30.0f);
        audio::SetListener(Vec3(), Vec3(0, 0, -1), Vec3(0, 1, 0));
        audio::SetEngine(engine, position, velocity, 0.5f + 0.5f * sinf(t * 0.3f), 1.0f);
        audio::Update(dt);
        if ((int)t > second) {
            second = (int)t;
            snprintf(line, sizeof(line), "t=%d s, track %d, effects %d, voices %d", second, track, effect,
                     audio::MixerActiveVoices());
            Say(line);
        }
        Sleep(16);
    }
    return 0;
}
