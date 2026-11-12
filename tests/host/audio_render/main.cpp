// Host check of the audio code (no SDL): a null device, then WAV files of every effect variant, engine sweeps and
// fly-bys, a busy battle mix and each music track, with peak / RMS / DC levels, synthesis time, memory and the
// mixer's cost per second of audio. Usage: audio_render OUT_DIR
#include "audio/audio.h"
#include "audio/mixer.h"
#include "audio/music.h"
#include "audio/sfx.h"

#include <chrono>
#include <math.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace audio {

// The null device: the test pulls frames from the mixer itself.
bool DeviceOpen()
{
    return true;
}

// No threads here: the bank builds inside Init, so Init's time is the synthesis time.
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
    (void)text;
}

void DeviceStatus(char* text, int size)
{
    snprintf(text, (size_t)size, "host");
}

} // namespace audio

using namespace audio;

// Output folder for the WAV files.
static std::string outDir;

// Most voices (effects + music) sounding at once.
static int maxVoices = 0;

// Mixing time (seconds) spent in MixerRender and the audio seconds it produced.
static double mixSeconds = 0.0;
static double mixedAudio = 0.0;

static double Now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// --- WAV files and levels

static void WriteWav(const std::string& name, const int16_t* data, size_t frames, int channels, int rate)
{
    std::string path = outDir + "/" + name + ".wav";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        printf("cannot write %s\n", path.c_str());
        return;
    }
    uint32_t bytes = (uint32_t)(frames * channels * 2);
    uint32_t header[11] = {0x46464952u, 36 + bytes, 0x45564157u, 0x20746D66u, 16,
                           (uint32_t)(1 | (channels << 16)), (uint32_t)rate, (uint32_t)(rate * channels * 2),
                           (uint32_t)((channels * 2) | (16 << 16)), 0x61746164u, bytes};
    fwrite(header, sizeof(header), 1, f);
    fwrite(data, 2, frames * channels, f);
    fclose(f);
}

// Prints peak (dBFS), RMS (dBFS), DC offset and the largest sample-to-sample jump (click detector) of a buffer at
// `rate` frames per second.
static void Levels(const std::string& name, const int16_t* data, size_t count, int channels, int rate = MIX_RATE)
{
    double sum = 0.0;
    double dc = 0.0;
    int peak = 0;
    int jump = 0;
    for (size_t i = 0; i < count; ++i) {
        int v = data[i];
        sum += (double)v * v;
        dc += v;
        if (abs(v) > peak)
            peak = abs(v);
        if (i >= (size_t)channels && abs(v - data[i - channels]) > jump)
            jump = abs(v - data[i - channels]);
    }
    double rms = sqrt(sum / (double)(count ? count : 1));
    printf("  %-28s %6.2f s  peak %6.1f dBFS  rms %6.1f dBFS  dc %6.1f  max step %5d\n", name.c_str(),
           (double)count / channels / rate, 20.0 * log10((peak + 1e-9) / 32768.0),
           20.0 * log10((rms + 1e-9) / 32768.0), dc / (double)(count ? count : 1), jump);
}

// --- Mixer capture

// Collects mixer output; `step` runs between buffers (game-side calls), with the time in seconds.
struct Capture {
    std::vector<int16_t> data;

    template <typename F>
    void Run(float seconds, F step)
    {
        constexpr int FRAMES = 1024;
        int buffers = (int)(seconds * MIX_RATE / FRAMES);
        int16_t buffer[FRAMES * 2];
        for (int b = 0; b < buffers; ++b) {
            float t = (float)b * FRAMES / MIX_RATE;
            step(t, (float)FRAMES / MIX_RATE);
            double start = Now();
            MixerRender(buffer, FRAMES);
            mixSeconds += Now() - start;
            mixedAudio += (double)FRAMES / MIX_RATE;
            if (MixerActiveVoices() > maxVoices)
                maxVoices = MixerActiveVoices();
            data.insert(data.end(), buffer, buffer + FRAMES * 2);
        }
    }

    void Save(const std::string& name)
    {
        WriteWav(name, data.data(), data.size() / 2, 2, MIX_RATE);
        Levels(name, data.data(), data.size(), 2);
        printf("    most voices at once: %d\n", maxVoices);
        maxVoices = 0;
    }
};

static const char* SFX_NAMES[(int)Sfx::COUNT] = {
    "gun", "rocket", "hit", "explosion", "big_explosion", "splash", "pickup", "ring", "stunt", "warning",
    "monkey_cheer", "monkey_hurt", "enemy_taunt", "menu_move", "menu_select", "overheat", "glass_crack",
    "sputter", "radio_blip", "crunch", "kill_sting", "zeppelin_death",
};

static const char* MUSIC_NAMES[] = {"none", "title", "combat", "boss", "victory", "defeat"};

// --- Scenes

// Writes a looping sample played three times round (the loop seam is in the file twice).
static void SaveLoop(const std::string& name, const Sample& loop)
{
    std::vector<int16_t> data(loop.data.begin(), loop.data.begin() + loop.frames);
    for (int r = 0; r < 2; ++r)
        data.insert(data.end(), loop.data.begin() + loop.loopStart, loop.data.begin() + loop.loopEnd);
    WriteWav(name, data.data(), data.size(), 1, (int)loop.rate);
    Levels(name, data.data(), data.size(), 1, (int)loop.rate);
}

static void SaveEffects()
{
    printf("Effects (bank samples, %d Hz mono):\n", SFX_RATE);
    for (int s = 0; s < (int)Sfx::COUNT; ++s) {
        for (int v = 0; v < SfxVariantCount((Sfx)s); ++v) {
            const Sample& sample = SfxVariant((Sfx)s, v);
            std::string name = std::string("sfx_") + SFX_NAMES[s] + "_" + std::to_string(v);
            WriteWav(name, sample.data.data(), sample.frames, 1, (int)sample.rate);
            Levels(name, sample.data.data(), sample.frames, 1, (int)sample.rate);
        }
    }
    const char* types[3] = {"pirate", "player", "zeppelin_fall"};
    const char* layers[2] = {"low", "high"};
    for (int t = 0; t < (int)EngineType::COUNT; ++t) {
        for (int l = 0; l < 2; ++l) {
            const Sample& loop = EngineLoop((EngineType)t, (EngineLayer)l);
            std::string name = std::string("loop_engine_") + types[t] + "_" + layers[l];
            SaveLoop(name, loop);
        }
    }
    SaveLoop("loop_whine", WhineLoop());
    SaveLoop("loop_wind", WindLoop());
}

// Lets the mixer finish what is playing.
static void Settle()
{
    Capture idle;
    idle.Run(1.0f, [](float, float) {
        SetMusic(Music::NONE);
        Update(1024.0f / MIX_RATE);
    });
}

static void SaveGunBurst()
{
    printf("Mixer scenes (%d Hz stereo):\n", MIX_RATE);
    Capture c;
    float next = 0.0f;
    c.Run(3.0f, [&](float t, float dt) {
        Update(dt);
        while (next <= t && t < 2.0f) {
            Play(Sfx::GUN, 1.0f, (int)(next * 14.0f) % 2 == 0 ? -0.2f : 0.2f);
            next += 1.0f / 14.0f;
        }
        if (t >= 2.0f && t < 2.03f)
            Play(Sfx::OVERHEAT);
    });
    c.Save("scene_gun_burst_overheat");
}

static void SaveEngineSweep()
{
    EngineId player = CreateEngine(EngineType::PLAYER);
    Capture c;
    c.Run(12.0f, [&](float t, float dt) {
        float throttle = t < 8.0f ? t / 8.0f : 1.0f;
        float boost = t > 8.5f && t < 11.0f ? 1.0f : 0.0f;
        Vec3 plane(0, 0, -75.0f * t);
        SetEngine(player, plane, Vec3(0, 0, -75), throttle, 1.0f, boost);
        SetListener(plane + Vec3(0, 2, 12), Vec3(0, 0, -1), Vec3(0, 1, 0));
        Update(dt);
    });
    DestroyEngine(player);
    c.Save("scene_engine_player_sweep_boost");

    EngineId pirate = CreateEngine(EngineType::PIRATE);
    Capture p;
    p.Run(8.0f, [&](float t, float dt) {
        SetEngine(pirate, Vec3(0, 0, -15), Vec3(0, 0, -60), t / 8.0f, 1.0f);
        SetListener(Vec3(0, 0, 0), Vec3(0, 0, -1), Vec3(0, 1, 0));
        Update(dt);
    });
    DestroyEngine(pirate);
    p.Save("scene_engine_pirate_sweep");
    Settle();
}

static void SaveFlyBy()
{
    EngineId pirate = CreateEngine(EngineType::PIRATE);
    Capture c;
    c.Run(10.0f, [&](float t, float dt) {
        float x = -600.0f + 120.0f * t;
        SetEngine(pirate, Vec3(x, 8, -20), Vec3(120, 0, 0), 0.9f, 1.0f);
        SetListener(Vec3(0, 0, 0), Vec3(0, 0, -1), Vec3(0, 1, 0));
        Update(dt);
    });
    DestroyEngine(pirate);
    c.Save("scene_flyby_pirate");
    Settle();
}

// A fight like the mission's: combat music, the player's engine and eight pirates (six heard), gun bursts before
// each kill, hits, kills (explosion, sting, cheer), calm stretches, a radio call and the zeppelin's blast.
static void SaveBattle()
{
    EngineId player = CreateEngine(EngineType::PLAYER);
    EngineId pirates[8];
    for (EngineId& e : pirates)
        e = CreateEngine(EngineType::PIRATE);
    SetMusic(Music::COMBAT);
    constexpr float KILLS[5] = {5.0f, 9.0f, 21.0f, 25.5f, 37.0f};
    int nextKill = 0;
    float nextShot = 0.0f;
    float nextHit = 0.3f;
    bool radio = false;
    bool zeppelin = false;
    Capture c;
    c.Run(45.0f, [&](float t, float dt) {
        Vec3 plane(0, 0, -80.0f * t);
        SetListener(plane + Vec3(0, 2, 12), Vec3(0, 0, -1), Vec3(0, 1, 0));
        float boost = t > 13.0f && t < 16.0f ? 1.0f : 0.0f;
        SetEngine(player, plane, Vec3(0, 0, -80), boost > 0.0f ? 1.0f : 0.6f, 1.0f, boost);
        for (int i = 0; i < 8; ++i) {
            float a = t * 0.4f + (float)i * 0.785f;
            float r = 60.0f + 50.0f * (float)i;
            Vec3 position = plane + Vec3(cosf(a) * r, 10.0f, sinf(a) * r);
            Vec3 velocity = Vec3(-sinf(a) * r * 0.4f, 0.0f, cosf(a) * r * 0.4f) + Vec3(0, 0, -80);
            SetEngine(pirates[i], position, velocity, 0.6f, 1.0f);
        }
        bool firing = nextKill < 5 && t > KILLS[nextKill] - 1.5f;
        while (nextShot <= t) {
            if (firing)
                Play(Sfx::GUN, 0.55f, 0.0f);
            nextShot += 1.0f / 14.0f;
        }
        while (firing && nextHit <= t) {
            Play3D(Sfx::HIT, plane + Vec3(0, 0, -120), 0.7f);
            nextHit += 0.45f;
        }
        if (!firing)
            nextHit = t;
        if (nextKill < 5 && t >= KILLS[nextKill]) {
            Play3D(Sfx::EXPLOSION, plane + Vec3(30.0f, 0.0f, -120.0f), 1.0f);
            Play(Sfx::KILL_STING);
            Play(Sfx::MONKEY_CHEER, 0.9f);
            ++nextKill;
        }
        if (t > 30.0f && !radio) {
            radio = true;
            Play(Sfx::RADIO_BLIP);
            DuckMusic(2.5f);
        }
        if (t > 41.0f && !zeppelin) {
            zeppelin = true;
            Play3D(Sfx::BIG_EXPLOSION, plane + Vec3(0, 150, -400), 1.0f);
        }
        Update(dt);
    });
    DestroyEngine(player);
    for (EngineId e : pirates)
        DestroyEngine(e);
    c.Save("scene_battle");
    Settle();
}

static void SaveMusicChanges()
{
    Capture c;
    c.Run(24.0f, [](float t, float dt) {
        Music m = t < 6.0f ? Music::TITLE : (t < 12.0f ? Music::COMBAT : Music::VICTORY);
        SetMusic(m);
        if (m == Music::VICTORY)
            SetMusicNext(Music::TITLE);
        Update(dt);
    });
    c.Save("scene_music_changes");
    Settle();
}

// The game's music path, three times round: TITLE (silence in the first round), COMBAT (engines, guns), BOSS,
// VICTORY with TITLE queued after it (the results screen), then NONE. Prints the mixer's cost per 60 Hz frame
// (800 output frames) in each state, which must not grow from one round to the next.
static void MeasureTransitions()
{
    printf("Music transitions (mixer cost per 60 Hz frame of audio, average / worst, and voices):\n");
    EngineId player = CreateEngine(EngineType::PLAYER);
    EngineId pirates[6];
    for (EngineId& e : pirates)
        e = CreateEngine(EngineType::PIRATE);
    struct Stage {
        const char* name;
        float seconds;
        bool fight;
    };
    constexpr Stage STAGES[5] = {
        {"TITLE", 12.0f, false}, {"COMBAT", 15.0f, true}, {"BOSS", 15.0f, true},
        {"VICTORY+TITLE", 12.0f, false}, {"NONE", 6.0f, false},
    };
    constexpr int FRAMES = 800;
    static int16_t buffer[FRAMES * 2];
    float t = 0.0f;
    for (int round = 0; round < 3; ++round) {
        for (int stage = 0; stage < 5; ++stage) {
            const Stage& st = STAGES[stage];
            switch (stage) {
            case 0:
                SetMusic(round == 0 ? Music::NONE : Music::TITLE);
                break;
            case 1:
                SetMusic(Music::COMBAT);
                break;
            case 2:
                SetMusic(Music::BOSS);
                break;
            case 3:
                SetMusic(Music::NONE);
                SetMusic(Music::VICTORY);
                SetMusicNext(Music::TITLE);
                break;
            default:
                SetMusic(Music::NONE);
                break;
            }
            double total = 0.0;
            double worst = 0.0;
            int frames = (int)(st.seconds * 60.0f);
            int voices = 0;
            for (int f = 0; f < frames; ++f) {
                float dt = 1.0f / 60.0f;
                t += dt;
                SetListener(Vec3(0, 2, 12), Vec3(0, 0, -1), Vec3(0, 1, 0));
                float volume = st.fight ? 1.0f : 0.0f;
                SetEngine(player, Vec3(), Vec3(0, 0, -80), 0.7f, volume);
                for (int i = 0; i < 6; ++i)
                    SetEngine(pirates[i], Vec3(100.0f * (float)i, 20, -200), Vec3(0, 0, -70), 0.6f, volume);
                if (st.fight && f % 4 == 0)
                    Play(Sfx::GUN, 0.5f);
                Update(dt);
                double start = Now();
                MixerRender(buffer, FRAMES);
                double cost = Now() - start;
                if (f >= frames / 2) {
                    total += cost;
                    if (cost > worst)
                        worst = cost;
                }
                if (MixerActiveVoices() > voices)
                    voices = MixerActiveVoices();
            }
            printf("  round %d %-14s %6.1f us / %6.1f us  voices %2d\n", round + 1, st.name,
                   total / (frames - frames / 2) * 1e6, worst * 1e6, voices);
        }
    }
    DestroyEngine(player);
    for (EngineId e : pirates)
        DestroyEngine(e);
    Settle();
}

// The zeppelin's death as the game should play it: boss music, the fall bed for 18 s (the hull 400 m away, falling
// faster), then ZEPPELIN_DEATH and VICTORY at impact.
static void SaveZeppelinFall()
{
    SetMusic(Music::BOSS);
    EngineId fall = CreateEngine(EngineType::ZEPPELIN_FALL);
    Capture c;
    c.Run(18.0f, [&](float t, float dt) {
        float k = t / 18.0f;
        SetListener(Vec3(0, 300, 0), Vec3(0, 0, -1), Vec3(0, 1, 0));
        SetEngine(fall, Vec3(100.0f, 400.0f - 380.0f * k * k, -400.0f), Vec3(0, -40.0f * k, -10), k, 1.0f);
        Update(dt);
    });
    DestroyEngine(fall);
    Play(Sfx::ZEPPELIN_DEATH);
    SetMusic(Music::NONE);
    SetMusic(Music::VICTORY);
    c.Run(10.0f, [&](float, float dt) {
        Update(dt);
    });
    c.Save("scene_zeppelin_fall");
    Settle();
}

// The end of a won mission as the game plays it: the zeppelin hits the sea (ZEPPELIN_DEATH, explosions, VICTORY
// with the title theme queued after it), the results come up 8 s later. Checks the hand-over for holes.
static void SaveVictoryEnding()
{
    EngineId player = CreateEngine(EngineType::PLAYER);
    bool results = false;
    Capture c;
    SetMusic(Music::BOSS);
    c.Run(4.0f, [&](float, float dt) {
        SetEngine(player, Vec3(), Vec3(), 0.6f, 1.0f);
        Update(dt);
    });
    Play(Sfx::ZEPPELIN_DEATH);
    Play3D(Sfx::BIG_EXPLOSION, Vec3(0, 0, -300));
    SetMusic(Music::NONE);
    SetMusic(Music::VICTORY);
    SetMusicNext(Music::TITLE);
    c.Run(16.0f, [&](float t, float dt) {
        SetEngine(player, Vec3(), Vec3(), 0.6f, results ? 0.0f : 1.0f);
        if (t >= 8.0f && !results) {
            results = true;
            SetEngine(player, Vec3(), Vec3(), 0.0f, 0.0f);
            FadeOutEffects();
        }
        Update(dt);
    });
    DestroyEngine(player);
    c.Save("scene_victory_ending");

    // The quietest 50 ms from the impact to the end: the hand-over must never leave a hole.
    size_t window = (size_t)MIX_RATE / 20 * 2;
    double quietest = 0.0;
    for (size_t at = (size_t)MIX_RATE * 4 * 2; at + window <= c.data.size(); at += window) {
        double sum = 0.0;
        for (size_t i = at; i < at + window; ++i)
            sum += (double)c.data[i] * c.data[i];
        double level = 10.0 * log10(sum / (double)window / 1073741824.0 + 1e-12);
        if (at == (size_t)MIX_RATE * 4 * 2 || level < quietest)
            quietest = level;
    }
    printf("    quietest 50 ms after the impact: %.1f dBFS\n", quietest);
    Settle();
}

// RMS (dBFS) of the gun at the game's level and rate (14 a second, alternating sides) against the player's engine
// at cruise heard from the chase camera, each alone.
static void MeasureGunAgainstEngine()
{
    EngineId player = CreateEngine(EngineType::PLAYER);
    Capture engine;
    engine.Run(3.0f, [&](float, float dt) {
        SetListener(Vec3(0, 2, 12), Vec3(0, 0, -1), Vec3(0, 1, 0));
        SetEngine(player, Vec3(), Vec3(), 0.6f, 1.0f);
        Update(dt);
    });
    DestroyEngine(player);
    Settle();
    Capture gun;
    float next = 0.0f;
    bool left = false;
    audio::Rng pitch(77);
    gun.Run(3.0f, [&](float t, float dt) {
        while (next <= t) {
            Play(Sfx::GUN, 0.55f, left ? -0.45f : 0.45f, pitch.Range(0.96f, 1.04f));
            left = !left;
            next += 1.0f / 14.0f;
        }
        Update(dt);
    });
    gun.Save("scene_gun_alone");

    // The same burst over the cruising engine, as the player hears it.
    player = CreateEngine(EngineType::PLAYER);
    Capture both;
    next = 0.0f;
    both.Run(5.0f, [&](float t, float dt) {
        SetListener(Vec3(0, 2, 12), Vec3(0, 0, -1), Vec3(0, 1, 0));
        SetEngine(player, Vec3(), Vec3(), 0.6f, 1.0f);
        while (next <= t && t > 1.0f && t < 4.0f) {
            Play(Sfx::GUN, 0.55f, left ? -0.45f : 0.45f, pitch.Range(0.96f, 1.04f));
            left = !left;
            next += 1.0f / 14.0f;
        }
        if (next < t && t <= 1.0f)
            next = 1.0f;
        Update(dt);
    });
    DestroyEngine(player);
    both.Save("scene_gun_burst_over_engine");
    printf("Gun against engine (RMS over 1-3 s):\n");
    const char* names[2] = {"player engine at cruise", "gun at 14 Hz"};
    const Capture* captures[2] = {&engine, &gun};
    for (int c = 0; c < 2; ++c) {
        const std::vector<int16_t>& d = captures[c]->data;
        double sum = 0.0;
        size_t from = (size_t)MIX_RATE * 2;
        for (size_t i = from; i < d.size(); ++i)
            sum += (double)d[i] * d[i];
        printf("  %-26s %6.1f dBFS\n", names[c], 10.0 * log10(sum / (double)(d.size() - from) / 1073741824.0));
    }
    Settle();
}

static void SaveMusic()
{
    printf("Music (%d Hz stereo):\n", MIX_RATE);
    for (int m = (int)Music::TITLE; m <= (int)Music::DEFEAT; ++m) {
        Capture c;
        bool once = m == (int)Music::VICTORY || m == (int)Music::DEFEAT;
        float seconds = once ? 9.0f : 60.0f;
        c.Run(seconds, [&](float, float dt) {
            SetMusic((Music)m);
            Update(dt);
        });
        c.Save(std::string("music_") + MUSIC_NAMES[m]);
        const Song& song = GetSong((Music)m);
        printf("    %zu events, %.1f s per pass\n", song.events.size(),
               (double)song.lengthTicks * 60.0 / (song.bpm * TICKS_PER_BEAT));
        Settle();
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: audio_render OUT_DIR\n");
        return 1;
    }
    outDir = argv[1];
    double start = Now();
    bool ok = Init();
    double synthesis = Now() - start;
    printf("Init: %s, synthesis %.1f ms; bank memory: effects + engines %.2f MB, music %.2f MB\n", ok ? "ok" : "FAILED",
           synthesis * 1000.0, SfxBankBytes() / 1048576.0, MusicBankBytes() / 1048576.0);
    const char* errors = MusicDataErrors();
    if (errors[0]) {
        printf("Music data errors:\n%s", errors);
        return 1;
    }
    SaveEffects();
    SaveGunBurst();
    SaveEngineSweep();
    SaveFlyBy();
    double battleStart = mixSeconds;
    double battleAudio = mixedAudio;
    SaveBattle();
    double battleCost = (mixSeconds - battleStart) / (mixedAudio - battleAudio);
    SaveMusicChanges();
    MeasureTransitions();
    MeasureGunAgainstEngine();
    SaveZeppelinFall();
    SaveVictoryEnding();
    SaveMusic();
    printf("Mixing cost: %.2f ms per second of audio overall, %.2f ms per second in the battle scene\n",
           mixSeconds / mixedAudio * 1000.0, battleCost * 1000.0);
    return 0;
}
