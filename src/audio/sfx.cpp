// The sound-effect bank and the engine loops: every effect is synthesised here at start-up from noise, oscillators,
// filters and formant voices (docs/design.md §6, creative brief §7).
#include "audio/sfx.h"

#include <atomic>
#include <math.h>

namespace audio {

// Rates the effects are rendered at: short bright sounds at the full SFX_RATE, voices a little lower, and long or
// low sounds (explosions, splashes, engines) at LOW_RATE, which halves their synthesis time and memory. The mixer
// resamples each sample from its own rate.
constexpr int VOICE_RATE = 22050;
constexpr int LOW_RATE = 16000;

// The rate the sound being built is rendered at (set by BuildSfxBank before each group).
static float renderRate = (float)SFX_RATE;

// Peak level each effect is normalised to; the mixer's per-effect gains set the balance.
constexpr float SFX_PEAK = 0.9f;

// The bank: variants per effect, the engine loops, the whine and the wind.
static Sample sfxBank[(int)Sfx::COUNT][MAX_SFX_VARIANTS];
static int sfxVariants[(int)Sfx::COUNT];
static Sample engineLoops[(int)EngineType::COUNT][2];
static Sample whineLoop;
static Sample windLoop;

// Frames in `seconds` at the bank rate.
static int Frames(float seconds)
{
    return (int)(seconds * renderRate);
}

// Seconds at frame i.
static float Time(int i)
{
    return (float)i / renderRate;
}

// Stores one rendered variant of an effect.
static void AddVariant(Sfx sfx, const std::vector<float>& buffer)
{
    int index = (int)sfx;
    sfxBank[index][sfxVariants[index]] = MakeSample(buffer, (uint32_t)renderRate, SFX_PEAK);
    ++sfxVariants[index];
}

// --- Common layers

// Sparse random clicks and tiny bandpassed bursts (debris, crackle, droplets of grit). density: events per second
// at the start, falling linearly to 0 at `seconds`.
static std::vector<float> Crackle(float seconds, float density, float loHz, float hiHz, uint32_t seed)
{
    std::vector<float> out(Frames(seconds), 0.0f);
    Rng rng(seed);
    float t = 0.0f;
    while (t < seconds) {
        float rate = density * (1.0f - t / seconds) + 1.0f;
        t += -logf(1.0f - rng.Float() * 0.999f) / rate;
        if (t >= seconds)
            break;
        float hz = rng.Range(loHz, hiHz);
        float len = rng.Range(0.004f, 0.018f);
        float gain = rng.Range(0.3f, 1.0f) * (1.0f - 0.7f * t / seconds);
        std::vector<float> grain = NoiseBurst(len, hz, 3.0f, 0.0005f, len * 0.3f, rng.Next(), renderRate);
        MixInto(out, grain, Frames(t), gain);
    }
    out.resize(Frames(seconds));
    return out;
}

// Brown noise (integrated white noise with a leak), lowpassed at `hz`.
static std::vector<float> Rumble(float seconds, float hz, uint32_t seed)
{
    std::vector<float> out(Frames(seconds));
    Rng rng(seed);
    float brown = 0.0f;
    Biquad lp;
    lp.SetLowpass(hz, 0.7f, renderRate);
    for (size_t i = 0; i < out.size(); ++i) {
        brown = brown * 0.995f + rng.Bipolar() * 0.1f;
        out[i] = lp.Process(brown);
    }
    return out;
}

// Soft-saturates a buffer with the given drive and scales it back.
static void Saturate(std::vector<float>& buffer, float drive)
{
    for (float& v : buffer)
        v = SoftClip(v * drive) / drive;
}

// Normalises a buffer to peak 1.
static void Normalise(std::vector<float>& buffer)
{
    float peak = 0.0f;
    for (float v : buffer) {
        if (fabsf(v) > peak)
            peak = fabsf(v);
    }
    if (peak < 1e-9f)
        return;
    for (float& v : buffer)
        v /= peak;
}

// --- Formant voices (monkey and gorilla)

// Formant frequencies (Hz) of the vowels the voices use, for an adult human tract.
enum Vowel {
    VOWEL_I,     // "ee"
    VOWEL_E,     // "eh"
    VOWEL_AE,    // "a" in cat
    VOWEL_A,     // "ah"
    VOWEL_O,     // "aw"
    VOWEL_U,     // "oo"
    VOWEL_UH,    // "uh"
    VOWEL_COUNT
};

static const float VOWEL_FORMANTS[VOWEL_COUNT][3] = {
    {270.0f, 2290.0f, 3010.0f},
    {530.0f, 1840.0f, 2480.0f},
    {660.0f, 1720.0f, 2410.0f},
    {730.0f, 1090.0f, 2440.0f},
    {570.0f, 840.0f, 2410.0f},
    {300.0f, 870.0f, 2240.0f},
    {640.0f, 1190.0f, 2390.0f},
};

// One voiced syllable. Pitch moves from pitchStart through pitchMid (at half time) to pitchEnd; the vowel glides from
// vowelStart to vowelEnd.
struct Syllable {
    float start;          // seconds into the sound
    float duration;
    float pitchStart;
    float pitchMid;
    float pitchEnd;
    Vowel vowelStart;
    Vowel vowelEnd;
    float gain;
    float breath;         // aspiration noise 0..1
    float rough;          // period-to-period roughness (growl, screech) 0..1
    float vibrato;        // depth as a fraction of pitch
    bool endsWithK;       // closes with a "k" release burst
};

// Renders one syllable through three parallel formant filters. formantScale: 1 = human, >1 smaller tract (monkey),
// <1 bigger (gorilla).
static std::vector<float> RenderSyllable(const Syllable& s, float formantScale, uint32_t seed)
{
    int frames = Frames(s.duration);
    std::vector<float> out(frames + Frames(0.08f), 0.0f);
    Rng rng(seed);
    SawOsc saw;
    Biquad formants[3];
    OnePole tilt;
    tilt.SetCutoff(1800.0f, renderRate);
    float jitter = 0.0f;
    float jitterTarget = 0.0f;
    float lastPhase = 0.0f;
    int periodCount = 0;
    constexpr float FORMANT_GAINS[3] = {1.0f, 0.7f, 0.4f};
    constexpr float BANDWIDTHS[3] = {90.0f, 120.0f, 170.0f};

    for (int i = 0; i < frames; ++i) {
        float t = Time(i);
        float k = t / s.duration;
        if ((i & 31) == 0) {
            for (int f = 0; f < 3; ++f) {
                float hz = Lerp(VOWEL_FORMANTS[s.vowelStart][f], VOWEL_FORMANTS[s.vowelEnd][f], k) * formantScale;
                float bw = BANDWIDTHS[f] * formantScale * (1.0f + s.rough);
                formants[f].SetBandpass(hz, hz / bw, renderRate);
            }
        }
        float pitch = k < 0.5f ? Lerp(s.pitchStart, s.pitchMid, k * 2.0f)
                               : Lerp(s.pitchMid, s.pitchEnd, k * 2.0f - 1.0f);
        pitch *= 1.0f + s.vibrato * Sine(t * 6.5f) + jitter;
        saw.Next(pitch / renderRate);

        // A new glottal period: pick the next jitter and count periods for the subharmonic growl.
        if (saw.phase < lastPhase) {
            ++periodCount;
            jitterTarget = rng.Bipolar() * 0.06f * s.rough;
        }
        lastPhase = saw.phase;
        jitter += (jitterTarget - jitter) * 0.05f;

        float source = tilt.Lowpass(2.0f * saw.phase - 1.0f);
        if ((periodCount & 1) != 0)
            source *= 1.0f - 0.6f * s.rough;
        source += rng.Bipolar() * s.breath;
        float voiced = 0.0f;
        for (int f = 0; f < 3; ++f)
            voiced += formants[f].Process(source) * FORMANT_GAINS[f];

        float attack = t < 0.02f ? t / 0.02f : 1.0f;
        float release = s.duration - t < 0.05f ? (s.duration - t) / 0.05f : 1.0f;
        out[i] = voiced * attack * release * s.gain;
    }

    // The "k": a short closure, then a bandpassed noise release.
    if (s.endsWithK) {
        float burstHz = 2600.0f * formantScale * 0.8f;
        std::vector<float> burst = NoiseBurst(0.03f, burstHz, 2.0f, 0.001f, 0.008f, rng.Next(), renderRate);
        MixInto(out, burst, frames + Frames(0.025f), 1.6f * s.gain);
    }
    return out;
}

// Renders a list of syllables into one sound.
static std::vector<float> RenderVoice(const Syllable* syllables, int count, float formantScale, float drive,
                                      uint32_t seed)
{
    std::vector<float> out;
    for (int i = 0; i < count; ++i) {
        std::vector<float> part = RenderSyllable(syllables[i], formantScale, seed + (uint32_t)i * 977u);
        Normalise(part);
        MixInto(out, part, Frames(syllables[i].start), syllables[i].gain);
    }
    Saturate(out, drive);
    FadeEdges(out, 16, Frames(0.02f));
    return out;
}

// Formant scale of the baboon's small vocal tract.
constexpr float MONKEY_TRACT = 1.35f;

// Formant scale of the gorilla's big chest.
constexpr float GORILLA_TRACT = 0.78f;

// A gorilla chest-beat: hollow thump.
static std::vector<float> ChestBeat(float pitch, uint32_t seed)
{
    std::vector<float> thump = Thump(0.14f, pitch * 1.4f, pitch, 0.02f, 0.002f, 0.035f, renderRate);
    std::vector<float> slap = NoiseBurst(0.06f, 450.0f, 1.5f, 0.001f, 0.012f, seed, renderRate);
    MixInto(thump, slap, 0, 0.5f);
    return thump;
}

static void BuildMonkeyCheers()
{
    const Syllable cheer0[] = {
        {0.00f, 0.13f, 400.0f, 470.0f, 500.0f, VOWEL_U, VOWEL_U, 0.8f, 0.05f, 0.1f, 0.0f, true},
        {0.22f, 0.13f, 430.0f, 500.0f, 540.0f, VOWEL_U, VOWEL_U, 0.85f, 0.05f, 0.1f, 0.0f, true},
        {0.44f, 0.55f, 650.0f, 1100.0f, 1180.0f, VOWEL_E, VOWEL_I, 1.0f, 0.08f, 0.15f, 0.035f, false},
    };
    const Syllable cheer1[] = {
        {0.00f, 0.10f, 420.0f, 480.0f, 470.0f, VOWEL_U, VOWEL_U, 0.75f, 0.05f, 0.1f, 0.0f, true},
        {0.16f, 0.10f, 440.0f, 500.0f, 490.0f, VOWEL_U, VOWEL_U, 0.8f, 0.05f, 0.1f, 0.0f, true},
        {0.32f, 0.10f, 470.0f, 540.0f, 530.0f, VOWEL_U, VOWEL_U, 0.85f, 0.05f, 0.1f, 0.0f, true},
        {0.50f, 0.50f, 700.0f, 980.0f, 1050.0f, VOWEL_A, VOWEL_I, 1.0f, 0.1f, 0.2f, 0.03f, false},
    };
    const Syllable cheer2[] = {
        {0.00f, 0.30f, 380.0f, 560.0f, 720.0f, VOWEL_U, VOWEL_O, 0.8f, 0.25f, 0.1f, 0.0f, false},
        {0.36f, 0.45f, 620.0f, 950.0f, 820.0f, VOWEL_U, VOWEL_U, 1.0f, 0.2f, 0.15f, 0.04f, false},
    };
    const Syllable cheer3[] = {
        {0.00f, 0.32f, 900.0f, 1200.0f, 1150.0f, VOWEL_I, VOWEL_I, 1.0f, 0.1f, 0.2f, 0.04f, false},
        {0.42f, 0.11f, 480.0f, 560.0f, 540.0f, VOWEL_U, VOWEL_U, 0.8f, 0.05f, 0.1f, 0.0f, true},
        {0.60f, 0.11f, 520.0f, 620.0f, 600.0f, VOWEL_U, VOWEL_U, 0.85f, 0.05f, 0.1f, 0.0f, true},
    };
    AddVariant(Sfx::MONKEY_CHEER, RenderVoice(cheer0, 3, MONKEY_TRACT, 1.3f, 11));
    AddVariant(Sfx::MONKEY_CHEER, RenderVoice(cheer1, 4, MONKEY_TRACT, 1.3f, 12));
    AddVariant(Sfx::MONKEY_CHEER, RenderVoice(cheer2, 2, MONKEY_TRACT, 1.3f, 13));
    AddVariant(Sfx::MONKEY_CHEER, RenderVoice(cheer3, 3, MONKEY_TRACT, 1.3f, 14));
}

static void BuildMonkeyScreeches()
{
    const Syllable hurt0[] = {
        {0.00f, 0.45f, 1200.0f, 1650.0f, 900.0f, VOWEL_I, VOWEL_AE, 1.0f, 0.3f, 0.5f, 0.02f, true},
    };
    const Syllable hurt1[] = {
        {0.00f, 0.50f, 1050.0f, 1250.0f, 700.0f, VOWEL_AE, VOWEL_A, 1.0f, 0.3f, 0.6f, 0.03f, false},
        {0.56f, 0.16f, 800.0f, 760.0f, 600.0f, VOWEL_A, VOWEL_UH, 0.6f, 0.3f, 0.5f, 0.0f, false},
    };
    const Syllable hurt2[] = {
        {0.00f, 0.15f, 1300.0f, 1550.0f, 1450.0f, VOWEL_I, VOWEL_I, 1.0f, 0.25f, 0.5f, 0.0f, true},
        {0.24f, 0.17f, 1350.0f, 1650.0f, 1400.0f, VOWEL_I, VOWEL_E, 1.0f, 0.25f, 0.5f, 0.0f, true},
    };
    const Syllable hurt3[] = {
        {0.00f, 0.60f, 800.0f, 1350.0f, 620.0f, VOWEL_U, VOWEL_AE, 1.0f, 0.3f, 0.55f, 0.03f, false},
    };
    AddVariant(Sfx::MONKEY_HURT, RenderVoice(hurt0, 1, MONKEY_TRACT, 2.0f, 21));
    AddVariant(Sfx::MONKEY_HURT, RenderVoice(hurt1, 2, MONKEY_TRACT, 2.0f, 22));
    AddVariant(Sfx::MONKEY_HURT, RenderVoice(hurt2, 2, MONKEY_TRACT, 2.0f, 23));
    AddVariant(Sfx::MONKEY_HURT, RenderVoice(hurt3, 1, MONKEY_TRACT, 2.0f, 24));
}

// Adds `count` chest beats starting at `start` seconds, `interval` apart.
static void AddChestBeats(std::vector<float>& out, float start, int count, float interval, uint32_t seed)
{
    for (int i = 0; i < count; ++i) {
        float pitch = (i & 1) != 0 ? 150.0f : 172.0f;
        MixInto(out, ChestBeat(pitch, seed + (uint32_t)i), Frames(start + interval * (float)i), 0.9f);
    }
}

static void BuildGorillaTaunts()
{
    const Syllable grunt0[] = {
        {0.00f, 0.24f, 115.0f, 105.0f, 92.0f, VOWEL_UH, VOWEL_UH, 1.0f, 0.35f, 0.8f, 0.0f, false},
        {0.32f, 0.42f, 105.0f, 95.0f, 75.0f, VOWEL_U, VOWEL_O, 1.0f, 0.3f, 0.8f, 0.0f, false},
    };
    const Syllable grunt1[] = {
        {0.00f, 0.85f, 88.0f, 82.0f, 68.0f, VOWEL_UH, VOWEL_O, 1.0f, 0.3f, 1.0f, 0.0f, false},
    };
    const Syllable grunt2[] = {
        {0.00f, 0.20f, 140.0f, 170.0f, 165.0f, VOWEL_U, VOWEL_U, 0.9f, 0.15f, 0.4f, 0.0f, false},
        {0.28f, 0.20f, 150.0f, 185.0f, 175.0f, VOWEL_U, VOWEL_U, 0.9f, 0.15f, 0.4f, 0.0f, false},
        {0.56f, 0.45f, 125.0f, 110.0f, 88.0f, VOWEL_A, VOWEL_UH, 1.0f, 0.35f, 0.8f, 0.0f, false},
    };
    const Syllable grunt3[] = {
        {0.50f, 0.60f, 110.0f, 120.0f, 82.0f, VOWEL_A, VOWEL_O, 1.0f, 0.45f, 0.9f, 0.0f, false},
    };
    std::vector<float> taunt = RenderVoice(grunt0, 2, GORILLA_TRACT, 1.8f, 31);
    AddChestBeats(taunt, 0.85f, 5, 0.085f, 310);
    AddVariant(Sfx::ENEMY_TAUNT, taunt);
    taunt = RenderVoice(grunt1, 1, GORILLA_TRACT, 2.0f, 32);
    AddVariant(Sfx::ENEMY_TAUNT, taunt);
    taunt = RenderVoice(grunt2, 3, GORILLA_TRACT, 1.8f, 33);
    AddVariant(Sfx::ENEMY_TAUNT, taunt);
    taunt = RenderVoice(grunt3, 1, GORILLA_TRACT, 2.0f, 34);
    AddChestBeats(taunt, 0.0f, 4, 0.1f, 340);
    AddVariant(Sfx::ENEMY_TAUNT, taunt);
}

// --- Weapons and impacts

// Banana gun, the most-heard sound of the game: a 2-6 kHz crack, a heavy 80-150 Hz thump body with a sub kick under
// it, a short mid "pop", and a mechanical tail (bolt ticks and belt rattle) that runs together into a rattle when
// the guns fire continuously. Kept short and not too bright, so 14 shots a second for minutes do not tire the ear.
static std::vector<float> GunShot(int variant)
{
    Rng rng(100u + (uint32_t)variant * 31u);
    float thumpHz = 92.0f + 9.0f * (float)variant;
    std::vector<float> out = Thump(0.22f, thumpHz * 2.4f, thumpHz, 0.012f, 0.0006f, 0.06f, renderRate);
    MixInto(out, Thump(0.2f, 120.0f, 58.0f, 0.02f, 0.001f, 0.05f, renderRate), 0, 0.45f);

    // The crack: a wide 2-6 kHz burst over a very short click.
    float crackHz = 3300.0f + 250.0f * (float)variant;
    MixInto(out, NoiseBurst(0.04f, crackHz, 0.5f, 0.0001f, 0.008f, rng.Next(), renderRate), 0, 4.5f);
    MixInto(out, NoiseBurst(0.01f, 6500.0f, 0.9f, 0.0001f, 0.0012f, rng.Next(), renderRate), 0, 0.8f);

    // The mid pop.
    float popHz = 650.0f + 80.0f * (float)variant;
    MixInto(out, NoiseBurst(0.06f, popHz, 1.3f, 0.0004f, 0.016f, rng.Next(), renderRate), 0, 1.0f);

    // Mechanical tail: two bolt ticks and a light belt rattle.
    constexpr float RATIOS[3] = {1.0f, 1.62f, 2.41f};
    constexpr float GAINS[3] = {1.0f, 0.6f, 0.4f};
    constexpr float TAUS[3] = {0.02f, 0.014f, 0.01f};
    std::vector<float> tick = Modes(0.06f, 1900.0f + 220.0f * (float)variant, RATIOS, GAINS, TAUS, 3, renderRate);
    MixInto(out, tick, Frames(rng.Range(0.030f, 0.038f)), 0.07f);
    MixInto(out, tick, Frames(rng.Range(0.058f, 0.070f)), 0.04f);
    for (int r = 0; r < 3; ++r) {
        std::vector<float> rattle = NoiseBurst(0.015f, rng.Range(2200.0f, 3400.0f), 3.0f, 0.0005f, 0.004f,
                                               rng.Next(), renderRate);
        MixInto(out, rattle, Frames(0.045f + 0.025f * (float)r + rng.Range(0.0f, 0.008f)), 0.25f - 0.06f * (float)r);
    }
    Saturate(out, 2.4f);
    out.resize(Frames(0.17f));
    FadeEdges(out, 4, Frames(0.025f));
    return out;
}

static std::vector<float> RocketLaunch(int variant)
{
    float seconds = 1.5f;
    int frames = Frames(seconds);
    std::vector<float> out(frames, 0.0f);
    Rng rng(200u + (uint32_t)variant);
    Svf whoosh;
    Biquad hiss;
    hiss.SetHighpass(3500.0f, 0.7f, renderRate);
    float whistlePhase = 0.0f;
    float crackleGate = 0.0f;
    for (int i = 0; i < frames; ++i) {
        float t = Time(i);
        if ((i & 15) == 0) {
            float hz = t < 0.22f ? 500.0f + 2600.0f * t / 0.22f : 700.0f + 2400.0f * Decay(t - 0.22f, 0.4f);
            whoosh.Set(hz * (1.0f + 0.1f * (float)variant), 1.3f, renderRate);
        }
        float noise = rng.Bipolar();
        whoosh.Process(noise);
        float env = AttackDecay(t, 0.04f, 0.55f);
        if (rng.Float() < 0.004f)
            crackleGate = 1.0f;
        crackleGate *= 0.995f;
        float whistleHz = (1700.0f + 150.0f * (float)variant) * (0.75f + 0.25f * Decay(t, 0.5f));
        whistlePhase += whistleHz * (1.0f + 0.01f * Sine(t * 9.0f)) / renderRate;
        out[i] = whoosh.band * env * 1.4f + hiss.Process(noise) * env * (0.25f + 0.5f * crackleGate) +
                 Sine(whistlePhase) * 0.12f * AttackDecay(t, 0.08f, 0.35f);
    }
    MixInto(out, Thump(0.3f, 140.0f, 60.0f, 0.06f, 0.002f, 0.08f, renderRate), 0, 0.9f);
    MixInto(out, NoiseBurst(0.04f, 2500.0f, 0.6f, 0.0005f, 0.008f, rng.Next(), renderRate), 0, 1.5f);
    FadeEdges(out, 8, Frames(0.15f));
    return out;
}

// Bullet hitting metal: a bright "tink" with a dull dent underneath.
static std::vector<float> MetalHit(int variant)
{
    constexpr float RATIOS[4] = {1.0f, 2.32f, 4.25f, 6.63f};
    constexpr float GAINS[4] = {1.0f, 0.6f, 0.4f, 0.25f};
    constexpr float TAUS[4] = {0.11f, 0.07f, 0.045f, 0.03f};
    constexpr float BASE_HZ[4] = {1320.0f, 1560.0f, 1790.0f, 2080.0f};
    Rng rng(300u + (uint32_t)variant);
    std::vector<float> out = Modes(0.35f, BASE_HZ[variant & 3], RATIOS, GAINS, TAUS, 4, renderRate);
    MixInto(out, NoiseBurst(0.01f, 5000.0f, 0.7f, 0.0002f, 0.0015f, rng.Next(), renderRate), 0, 2.0f);
    MixInto(out, Thump(0.08f, 320.0f, 200.0f, 0.02f, 0.001f, 0.02f, renderRate), 0, 0.6f);
    FadeEdges(out, 4, Frames(0.05f));
    return out;
}

// The player's plane being hit: a crunch of bent metal under a thud.
static std::vector<float> Crunch(int variant)
{
    float seconds = 0.5f;
    Rng rng(400u + (uint32_t)variant);
    std::vector<float> out = Thump(seconds, 130.0f, 58.0f, 0.04f, 0.001f, 0.07f, renderRate);
    std::vector<float> grit(Frames(seconds));
    Biquad bp;
    bp.SetBandpass(1400.0f + 300.0f * (float)variant, 0.9f, renderRate);
    float gate = 0.0f;
    for (size_t i = 0; i < grit.size(); ++i) {
        if ((i % 180) == 0)
            gate = rng.Float() < 0.6f ? rng.Range(0.3f, 1.0f) : 0.05f;
        grit[i] = bp.Process(rng.Bipolar()) * gate * AttackDecay(Time((int)i), 0.002f, 0.09f);
    }
    MixInto(out, grit, 0, 1.6f);
    constexpr float RATIOS[3] = {1.0f, 2.7f, 5.1f};
    constexpr float GAINS[3] = {1.0f, 0.5f, 0.3f};
    constexpr float TAUS[3] = {0.08f, 0.05f, 0.03f};
    MixInto(out, Modes(0.3f, 640.0f + 90.0f * (float)variant, RATIOS, GAINS, TAUS, 3, renderRate), 0, 0.35f);
    MixInto(out, Crackle(0.4f, 50.0f, 3000.0f, 7000.0f, rng.Next()), Frames(0.03f), 0.3f);
    Saturate(out, 1.6f);
    FadeEdges(out, 4, Frames(0.05f));
    return out;
}

// --- Explosions and water

// One blast: sharp crack, 40-60 Hz boom, a roaring noise body and a rattle of debris.
static std::vector<float> Blast(float seconds, float boomHz, float size, uint32_t seed)
{
    Rng rng(seed);
    std::vector<float> out = Thump(seconds, boomHz * 1.8f, boomHz, 0.08f, 0.003f, 0.4f * size, renderRate);

    // The crack: a sharp 2-6 kHz transient and a smaller second snap, so a blast cuts through engines and music.
    std::vector<float> crack = NoiseBurst(0.1f, 3800.0f, 0.6f, 0.0002f, 0.018f, rng.Next(), renderRate);
    MixInto(out, crack, 0, 2.6f);
    std::vector<float> snap = NoiseBurst(0.06f, 2600.0f, 0.8f, 0.0002f, 0.01f, rng.Next(), renderRate);
    MixInto(out, snap, Frames(0.028f), 1.4f);

    // Roaring body: brown noise whose lowpass closes over the decay.
    std::vector<float> body(Frames(seconds));
    Rng noise(rng.Next());
    Svf lp;
    float brown = 0.0f;
    for (size_t i = 0; i < body.size(); ++i) {
        float t = Time((int)i);
        if ((i & 31) == 0)
            lp.Set(180.0f + 2200.0f * Decay(t, 0.25f * size), 0.8f, renderRate);
        brown = brown * 0.98f + noise.Bipolar() * 0.2f;
        lp.Process(brown + noise.Bipolar() * 0.15f);
        body[i] = lp.low * AttackDecay(t, 0.006f, 0.45f * size);
    }
    MixInto(out, body, 0, 1.1f);
    MixInto(out, Crackle(seconds * 0.85f, 90.0f, 900.0f, 5500.0f, rng.Next()), Frames(0.06f), 0.22f);
    return out;
}

// Plays a buffer back `ratio` times faster (below 1: slower, lower and longer), with linear interpolation.
static std::vector<float> Resample(const std::vector<float>& source, float ratio)
{
    std::vector<float> out((size_t)((float)(source.size() - 1) / ratio));
    for (size_t i = 0; i < out.size(); ++i) {
        float at = (float)i * ratio;
        size_t whole = (size_t)at;
        float f = at - (float)whole;
        out[i] = source[whole] + (source[whole + 1] - source[whole]) * f;
    }
    return out;
}

// The one blast every explosion is made from (rendered once, then played at different speeds: lower and longer
// for the big ones), which keeps the bank's start-up time down.
static std::vector<float> baseBlast;

static const std::vector<float>& BaseBlast()
{
    if (baseBlast.empty()) {
        baseBlast = Blast(3.0f, 50.0f, 1.2f, 500);
        FadeEdges(baseBlast, 0, Frames(0.8f));
    }
    return baseBlast;
}

static std::vector<float> Explosion(int variant)
{
    constexpr float SPEEDS[3] = {1.05f, 0.94f, 1.16f};
    std::vector<float> out = Resample(BaseBlast(), SPEEDS[variant]);
    out.resize(Frames(2.0f));
    Saturate(out, 2.2f);
    RemoveDc(out, renderRate);
    FadeEdges(out, 4, Frames(0.3f));
    return out;
}

// The zeppelin going up: three blasts, a long rumble, the hull groaning and debris raining down.
static std::vector<float> BigExplosion()
{
    float seconds = 5.5f;
    std::vector<float> out = Resample(BaseBlast(), 0.82f);
    out.resize(Frames(seconds), 0.0f);
    MixInto(out, Resample(BaseBlast(), 0.95f), Frames(0.42f), 0.8f);
    MixInto(out, Resample(BaseBlast(), 0.86f), Frames(1.15f), 0.9f);

    std::vector<float> rumble = Rumble(seconds, 140.0f, 603);
    Rng wobble(604);
    float am = 1.0f;
    for (size_t i = 0; i < rumble.size(); ++i) {
        float t = Time((int)i);
        if ((i % 2400) == 0)
            am = wobble.Range(0.6f, 1.0f);
        rumble[i] *= AttackDecay(t, 0.3f, 1.9f) * am;
    }
    Normalise(rumble);
    MixInto(out, rumble, 0, 0.9f);

    // The hull groaning: a slow sawtooth through a resonant bandpass, falling in pitch.
    std::vector<float> groan(Frames(2.8f));
    SawOsc saw;
    Biquad bp;
    bp.SetBandpass(320.0f, 3.0f, renderRate);
    for (size_t i = 0; i < groan.size(); ++i) {
        float t = Time((int)i);
        float hz = 62.0f - 22.0f * t / 2.8f + 3.0f * Sine(t * 1.7f);
        groan[i] = bp.Process(saw.Next(hz / renderRate)) * Sine(0.5f * t / 2.8f);
    }
    MixInto(out, groan, Frames(1.6f), 0.35f);
    MixInto(out, Crackle(3.5f, 60.0f, 700.0f, 4500.0f, 605), Frames(1.4f), 0.2f);
    Saturate(out, 1.4f);
    RemoveDc(out, renderRate);
    FadeEdges(out, 4, Frames(0.8f));
    return out;
}

// A huge splash: a body of water thrown up and falling back (a falling bandpass over seconds), then rain.
static std::vector<float> BigSplash(uint32_t seed)
{
    float seconds = 4.5f;
    Rng rng(seed);
    std::vector<float> out = Thump(1.0f, 160.0f, 45.0f, 0.3f, 0.01f, 0.35f, renderRate);
    std::vector<float> body(Frames(seconds));
    Svf bp;
    EnvelopeAD env(0.08f, 1.1f, renderRate);
    for (size_t i = 0; i < body.size(); ++i) {
        float t = Time((int)i);
        if ((i & 31) == 0)
            bp.Set(250.0f + 1600.0f * Decay(t, 0.9f), 0.7f, renderRate);
        bp.Process(rng.Bipolar());
        body[i] = (bp.band + bp.low * 0.6f) * env.Next();
    }
    MixInto(out, body, 0, 1.4f);
    MixInto(out, Crackle(3.5f, 260.0f, 1200.0f, 6000.0f, rng.Next()), Frames(0.6f), 0.35f);
    FadeEdges(out, 0, Frames(1.5f));
    return out;
}

// The zeppelin's end: three big blasts 0.6 s apart, the hull hitting the sea, metal creaking and groaning as it
// folds, and a 30-60 Hz rumble that carries on for seconds. The loudest sound of the game.
static std::vector<float> ZeppelinDeath()
{
    float seconds = 8.0f;
    std::vector<float> out(Frames(seconds), 0.0f);
    for (int b = 0; b < 3; ++b) {
        std::vector<float> blast = Resample(BaseBlast(), 0.72f + 0.05f * (float)b);
        MixInto(out, blast, Frames(0.6f * (float)b), b == 1 ? 0.85f : 1.0f);
    }
    MixInto(out, BigSplash(910), Frames(0.15f), 1.1f);

    // The rumble: brown noise kept between 30 and 60 Hz, swelling and fading over the whole sound.
    std::vector<float> rumble = Rumble(seconds, 60.0f, 920);
    Biquad cut;
    cut.SetHighpass(30.0f, 0.7f, renderRate);
    Rng wobble(921);
    float am = 1.0f;
    float amTarget = 1.0f;
    EnvelopeAD swell(0.4f, 2.6f, renderRate);
    for (size_t i = 0; i < rumble.size(); ++i) {
        if ((i % 3200) == 0)
            amTarget = wobble.Range(0.55f, 1.0f);
        am += (amTarget - am) * 0.0005f;
        rumble[i] = cut.Process(rumble[i]) * swell.Next() * am;
    }
    Normalise(rumble);
    MixInto(out, rumble, 0, 1.1f);

    // Creaking metal: noise through a narrow bandpass falling from 400 to 200 Hz, in irregular strains.
    std::vector<float> creak(Frames(4.0f));
    Svf strain;
    Rng metal(930);
    float gate = 0.0f;
    float gateTarget = 0.0f;
    for (size_t i = 0; i < creak.size(); ++i) {
        float k = (float)i / (float)creak.size();
        if ((i & 31) == 0)
            strain.Set(400.0f - 200.0f * k + 25.0f * Sine(k * 9.0f), 14.0f, renderRate);
        if ((i % 1600) == 0)
            gateTarget = metal.Float() < 0.55f ? metal.Range(0.4f, 1.0f) : 0.0f;
        gate += (gateTarget - gate) * 0.004f;
        strain.Process(metal.Bipolar());
        creak[i] = strain.band * gate * Sine(0.5f * k);
    }
    Normalise(creak);
    MixInto(out, creak, Frames(1.4f), 0.45f);
    Saturate(out, 1.6f);
    RemoveDc(out, renderRate);
    FadeEdges(out, 4, Frames(1.5f));
    return out;
}

static std::vector<float> Splash(int variant)
{
    float seconds = 1.4f;
    Rng rng(700u + (uint32_t)variant);
    std::vector<float> out = Thump(seconds, 420.0f, 110.0f, 0.07f, 0.002f, 0.07f, renderRate);
    for (float& v : out)
        v *= 0.7f;
    std::vector<float> body(Frames(seconds));
    Svf bp;
    Biquad spray;
    spray.SetHighpass(4000.0f, 0.7f, renderRate);
    for (size_t i = 0; i < body.size(); ++i) {
        float t = Time((int)i);
        if ((i & 31) == 0)
            bp.Set(600.0f + 2400.0f * Decay(t, 0.25f), 0.9f, renderRate);
        float n = rng.Bipolar();
        bp.Process(n);
        body[i] = bp.band * AttackDecay(t, 0.012f, 0.32f) * 1.3f +
                  spray.Process(n) * AttackDecay(t, 0.005f, 0.2f) * 0.5f;
    }
    MixInto(out, body, 0, 1.0f);

    // Droplets and bubbles: tiny rising sine chirps.
    for (int d = 0; d < 28; ++d) {
        float start = 0.08f + rng.Float() * rng.Float() * 1.0f;
        float hz = rng.Range(800.0f, 2300.0f);
        std::vector<float> drop(Frames(0.04f));
        float phase = 0.0f;
        for (size_t i = 0; i < drop.size(); ++i) {
            float t = Time((int)i);
            phase += hz * (1.0f + 12.0f * t) / renderRate;
            drop[i] = Sine(phase) * AttackDecay(t, 0.001f, 0.012f);
        }
        MixInto(out, drop, Frames(start), rng.Range(0.05f, 0.18f) * (1.2f - start));
    }
    FadeEdges(out, 4, Frames(0.2f));
    return out;
}

// --- Chimes, swooshes and interface

// A glockenspiel-like bell at `hz`.
static std::vector<float> Bell(float hz, float seconds)
{
    constexpr float RATIOS[4] = {1.0f, 2.76f, 5.40f, 8.93f};
    constexpr float GAINS[4] = {1.0f, 0.45f, 0.22f, 0.1f};
    constexpr float TAUS[4] = {0.45f, 0.2f, 0.09f, 0.05f};
    return Modes(seconds, hz, RATIOS, GAINS, TAUS, 4, renderRate);
}

static std::vector<float> PickupChime()
{
    std::vector<float> out;
    constexpr float NOTES[4] = {84.0f, 88.0f, 91.0f, 96.0f};
    for (int i = 0; i < 4; ++i)
        MixInto(out, Bell(NoteHz(NOTES[i]), 0.9f), Frames(0.055f * (float)i), i == 3 ? 1.0f : 0.75f);
    FadeEdges(out, 4, Frames(0.1f));
    return out;
}

static std::vector<float> RingDing()
{
    std::vector<float> out = Bell(NoteHz(91.0f), 1.3f);
    MixInto(out, Bell(NoteHz(91.0f) * 1.004f, 1.3f), 0, 0.6f);
    MixInto(out, Bell(NoteHz(98.0f), 1.1f), 0, 0.7f);
    MixInto(out, Bell(NoteHz(103.0f), 0.8f), Frames(0.11f), 0.4f);
    FadeEdges(out, 4, Frames(0.15f));
    return out;
}

static std::vector<float> Swoosh(int variant)
{
    float seconds = 1.1f + 0.2f * (float)variant;
    int frames = Frames(seconds);
    std::vector<float> out(frames);
    Rng rng(800u + (uint32_t)variant);
    Svf bp;
    for (int i = 0; i < frames; ++i) {
        float k = (float)i / (float)frames;
        if ((i & 15) == 0) {
            float peak = variant == 0 ? 0.4f : 0.55f;
            float shape = k < peak ? k / peak : 1.0f - (k - peak) / (1.0f - peak);
            bp.Set(260.0f + 1700.0f * shape * shape, 1.6f, renderRate);
        }
        bp.Process(rng.Bipolar());
        float env = Sine(0.5f * k);
        out[i] = (bp.band + bp.low * 0.3f) * env * env;
    }
    return out;
}

static std::vector<float> WarningBeep()
{
    std::vector<float> out(Frames(0.42f), 0.0f);
    PulseOsc osc;
    Biquad lp;
    lp.SetLowpass(3000.0f, 0.7f, renderRate);
    for (size_t i = 0; i < out.size(); ++i) {
        float t = Time((int)i);
        bool first = t < 0.13f;
        bool second = t >= 0.2f && t < 0.33f;
        float hz = first ? 1100.0f : 880.0f;
        float v = lp.Process(osc.Next(hz / renderRate, 0.5f));
        float local = first ? t : t - 0.2f;
        float edge = local < 0.005f ? local / 0.005f : (0.13f - local < 0.005f ? (0.13f - local) / 0.005f : 1.0f);
        out[i] = (first || second) ? v * edge : 0.0f;
    }
    return out;
}

// A soft marimba note (menus).
static std::vector<float> Marimba(float hz, float seconds)
{
    constexpr float RATIOS[3] = {1.0f, 3.93f, 9.2f};
    constexpr float GAINS[3] = {1.0f, 0.35f, 0.1f};
    constexpr float TAUS[3] = {0.12f, 0.04f, 0.015f};
    return Modes(seconds, hz, RATIOS, GAINS, TAUS, 3, renderRate);
}

static std::vector<float> MenuMove()
{
    std::vector<float> out = Marimba(NoteHz(84.0f), 0.18f);
    FadeEdges(out, 4, Frames(0.03f));
    return out;
}

static std::vector<float> MenuSelect()
{
    std::vector<float> out = Marimba(NoteHz(84.0f), 0.5f);
    MixInto(out, Marimba(NoteHz(91.0f), 0.5f), Frames(0.07f), 1.0f);
    MixInto(out, Bell(NoteHz(103.0f), 0.4f), Frames(0.07f), 0.15f);
    FadeEdges(out, 4, Frames(0.08f));
    return out;
}

static std::vector<float> OverheatHiss()
{
    float seconds = 1.7f;
    Rng rng(900);
    std::vector<float> out(Frames(seconds));
    Biquad hp;
    hp.SetHighpass(2500.0f, 0.7f, renderRate);
    Biquad bp;
    bp.SetBandpass(5500.0f, 1.2f, renderRate);
    for (size_t i = 0; i < out.size(); ++i) {
        float t = Time((int)i);
        float n = rng.Bipolar();
        float flutter = 0.8f + 0.2f * Sine(t * 23.0f);
        float env = t < 0.04f ? t / 0.04f : (t < 0.9f ? 1.0f : Decay(t - 0.9f, 0.25f));
        out[i] = (hp.Process(n) * 0.5f + bp.Process(n)) * env * flutter;
    }
    constexpr float RATIOS[3] = {1.0f, 2.6f, 4.1f};
    constexpr float GAINS[3] = {1.0f, 0.55f, 0.3f};
    constexpr float TAUS[3] = {0.25f, 0.14f, 0.07f};
    std::vector<float> clank = Modes(0.6f, 430.0f, RATIOS, GAINS, TAUS, 3, renderRate);
    MixInto(clank, NoiseBurst(0.01f, 3000.0f, 0.7f, 0.0002f, 0.002f, 901, renderRate), 0, 1.5f);
    MixInto(out, clank, 0, 1.6f);
    FadeEdges(out, 4, Frames(0.1f));
    return out;
}

static std::vector<float> GlassCrack(int variant)
{
    Rng rng(1000u + (uint32_t)variant);
    std::vector<float> out = NoiseBurst(0.02f, 3500.0f, 0.5f, 0.0002f, 0.003f, rng.Next(), renderRate);
    for (float& v : out)
        v *= 2.0f;
    MixInto(out, Crackle(0.22f, 220.0f, 3000.0f, 9000.0f, rng.Next()), Frames(0.005f), 0.8f);
    constexpr float RATIOS[3] = {1.0f, 1.64f, 2.53f};
    constexpr float GAINS[3] = {1.0f, 0.7f, 0.5f};
    constexpr float TAUS[3] = {0.14f, 0.1f, 0.07f};
    MixInto(out, Modes(0.5f, 2800.0f + 400.0f * (float)variant, RATIOS, GAINS, TAUS, 3, renderRate), 0, 0.25f);
    FadeEdges(out, 4, Frames(0.08f));
    return out;
}

// An engine on fire coughing: irregular backfire pops.
static std::vector<float> Sputter(int variant)
{
    Rng rng(1100u + (uint32_t)variant);
    std::vector<float> out(Frames(0.9f), 0.0f);
    float t = 0.0f;
    int pops = 4 + variant;
    for (int p = 0; p < pops; ++p) {
        bool bang = p == pops - 1 && variant != 1;
        std::vector<float> pop = Thump(0.2f, 120.0f, 60.0f, 0.03f, 0.001f, bang ? 0.06f : 0.035f, renderRate);
        MixInto(pop, NoiseBurst(0.1f, 800.0f, 1.0f, 0.001f, bang ? 0.03f : 0.015f, rng.Next(), renderRate), 0, 1.4f);
        MixInto(out, pop, Frames(t), bang ? 1.0f : rng.Range(0.4f, 0.75f));
        t += rng.Range(0.06f, 0.17f);
    }
    Saturate(out, 1.6f);
    FadeEdges(out, 4, Frames(0.1f));
    return out;
}

// Radio message start: squelch click and a short two- or three-tone chirp through a telephone band.
static std::vector<float> RadioBlip(int variant)
{
    std::vector<float> out(Frames(0.26f), 0.0f);
    MixInto(out, NoiseBurst(0.04f, 2000.0f, 1.0f, 0.0005f, 0.012f, 1200u + (uint32_t)variant, renderRate), 0, 1.2f);
    PulseOsc osc;
    Biquad hp;
    hp.SetHighpass(400.0f, 0.7f, renderRate);
    Biquad lp;
    lp.SetLowpass(2800.0f, 0.7f, renderRate);
    constexpr float TONES[2][3] = {{1250.0f, 1650.0f, 0.0f}, {1650.0f, 1250.0f, 1850.0f}};
    for (int k = 0; k < 3; ++k) {
        float hz = TONES[variant][k];
        if (hz == 0.0f)
            break;
        int start = Frames(0.05f + 0.06f * (float)k);
        int len = Frames(0.05f);
        for (int i = 0; i < len; ++i) {
            float edge = i < 100 ? (float)i / 100.0f : (len - i < 100 ? (float)(len - i) / 100.0f : 1.0f);
            out[start + i] += osc.Next(hz / renderRate, 0.5f) * 0.35f * edge;
        }
    }
    for (float& v : out)
        v = lp.Process(hp.Process(v));
    FadeEdges(out, 4, Frames(0.02f));
    return out;
}

// Kill sting: a xylophone flourish up to a bright brass "ta-DAH" on G (root, fifth, octave: it sits over every
// track's key), 0.45 s.
static std::vector<float> KillSting(int variant)
{
    constexpr float RATIOS[3] = {1.0f, 3.0f, 6.2f};
    constexpr float GAINS[3] = {1.0f, 0.4f, 0.15f};
    constexpr float TAUS[3] = {0.18f, 0.06f, 0.02f};
    constexpr float XYLOPHONE[2][3] = {{79.0f, 86.0f, 91.0f}, {86.0f, 91.0f, 98.0f}};
    std::vector<float> out(Frames(0.5f), 0.0f);
    for (int n = 0; n < 3; ++n) {
        std::vector<float> bar = Modes(0.3f, NoteHz(XYLOPHONE[variant][n]), RATIOS, GAINS, TAUS, 3, renderRate);
        MixInto(out, bar, Frames(0.045f * (float)n), 0.5f);
    }

    // The brass stab: detuned saws per note through a lowpass that opens and closes fast.
    constexpr float STAB[3] = {67.0f, 74.0f, 79.0f};
    int start = Frames(0.11f);
    int length = Frames(0.36f);
    for (int n = 0; n < 3; ++n) {
        SawOsc a;
        SawOsc b;
        Svf tone;
        float hz = NoteHz(STAB[n]);
        for (int i = 0; i < length; ++i) {
            float t = Time(i);
            if ((i & 15) == 0)
                tone.Set(hz * (1.5f + 6.0f * AttackDecay(t, 0.02f, 0.1f)), 0.9f, renderRate);
            tone.Process(a.Next(hz / renderRate) + b.Next(hz * 1.006f / renderRate));
            out[start + i] += tone.low * 0.35f * AttackDecay(t, 0.012f, 0.16f);
        }
    }
    Saturate(out, 1.5f);
    FadeEdges(out, 4, Frames(0.05f));
    return out;
}

// --- Engine loops

// Crossfade length of the engine loops (frames).
static int EngineCrossfade()
{
    return (int)renderRate / 10;
}

// Pirate radial: a firing cycle (9 cylinders, 2 revolutions) at a 45 Hz firing rate, in frames.
static int RadialCycle()
{
    return (int)renderRate * 9 / 45;
}
constexpr int RADIAL_CYCLES_PER_LOOP = 9;

// Player inline: 1.8 s loop, every partial an integer number of cycles in it.
static int InlineLoop()
{
    return (int)renderRate * 18 / 10;
}

// Pirate radial chug. rough 1 = idle (lumpy, dark), 0 = full throttle (smoother, brighter, more propeller).
static std::vector<float> RadialEngine(float rough, uint32_t seed)
{
    int loop = RadialCycle() * RADIAL_CYCLES_PER_LOOP;
    int frames = loop + EngineCrossfade();
    std::vector<float> out(frames, 0.0f);
    Rng rng(seed);

    // Fixed per-cylinder character: strength and timing offset.
    float cylGain[9];
    float cylShift[9];
    for (int c = 0; c < 9; ++c) {
        cylGain[c] = 1.0f + rng.Bipolar() * (0.12f + 0.3f * rough);
        cylShift[c] = rng.Bipolar() * (0.02f + 0.06f * rough);
    }

    // Firing pulses: a damped low thump plus a short noise bang through the exhaust resonance.
    float firing = (float)RadialCycle() / 9.0f;
    int pulses = (frames + RadialCycle()) * 9 / RadialCycle() + 9;
    std::vector<float> pulse(Frames(0.06f));
    float pipeHz = 210.0f - 50.0f * rough;
    for (size_t i = 0; i < pulse.size(); ++i) {
        float t = Time((int)i);
        pulse[i] = Sine(t * pipeHz) * AttackDecay(t, 0.0008f, 0.009f + 0.004f * rough) +
                   0.5f * Sine(t * 540.0f) * AttackDecay(t, 0.0005f, 0.004f) +
                   0.6f * Sine(t * 70.0f) * AttackDecay(t, 0.002f, 0.012f);
    }
    Biquad exhaust;
    exhaust.SetBandpass(420.0f + 300.0f * (1.0f - rough), 1.4f, renderRate);
    for (int p = 0; p < pulses; ++p) {
        int c = p % 9;
        float misfire = rng.Float() < 0.03f * rough ? 0.3f : 1.0f;
        float at = ((float)p + cylShift[c]) * firing;
        int start = (int)at;
        float gain = cylGain[c] * misfire * rng.Range(0.9f, 1.1f);
        for (size_t i = 0; i < pulse.size() && start + (int)i < frames; ++i) {
            if (start + (int)i >= 0)
                out[start + i] += pulse[i] * gain;
        }
        for (int i = 0; i < Frames(0.012f) && start + i < frames; ++i) {
            if (start + i >= 0)
                out[start + i] += rng.Bipolar() * gain * Decay(Time(i), 0.0035f) * (0.5f + 0.4f * rough);
        }
    }
    for (float& v : out)
        v = v * 0.6f + exhaust.Process(v) * 0.8f;

    // Propeller: band noise pulsing at the blade-pass rate (6 per cycle), stronger at full throttle.
    Biquad prop;
    prop.SetBandpass(700.0f, 0.6f, renderRate);
    for (int i = 0; i < frames; ++i) {
        float blade = 0.5f + 0.5f * Sine((float)i * 6.0f / (float)RadialCycle());
        out[i] += prop.Process(rng.Bipolar()) * blade * blade * (0.15f + 0.35f * (1.0f - rough));
    }

    // Exhaust rumble and the final tone colour.
    std::vector<float> rumble = Rumble((float)frames / renderRate + 0.01f, 160.0f, rng.Next());
    Biquad lp;
    lp.SetLowpass(1100.0f + 1800.0f * (1.0f - rough), 0.8f, renderRate);
    for (int i = 0; i < frames; ++i) {
        float beat = 0.6f + 0.4f * Sine((float)i * 9.0f / (float)RadialCycle());
        out[i] = lp.Process(out[i] + rumble[i] * 0.7f * beat);
    }
    Saturate(out, 1.3f);
    RemoveDc(out, renderRate);
    MakeLoop(out, EngineCrossfade(), EngineCrossfade() + loop, EngineCrossfade());
    return out;
}

// Player inline: two detuned saws at ~70 Hz, a third harmonic and a low putter. bright 0 = idle, 1 = full.
static std::vector<float> InlineEngine(float bright, uint32_t seed)
{
    int frames = InlineLoop() + EngineCrossfade();
    std::vector<float> out(frames);
    Rng rng(seed);
    float loopSeconds = (float)InlineLoop() / renderRate;

    // Frequencies with a whole number of cycles in the loop, so the tones loop seamlessly.
    float f1 = 126.0f / loopSeconds;
    float f2 = 127.0f / loopSeconds;
    float f3 = 378.0f / loopSeconds;
    SawOsc sawA;
    SawOsc sawB;
    sawB.phase = 0.37f;
    Svf tone;
    tone.Set(380.0f + 1200.0f * bright, 1.2f + 1.0f * bright, renderRate);
    Biquad putterBand;
    putterBand.SetBandpass(260.0f, 1.2f, renderRate);
    for (int i = 0; i < frames; ++i) {
        float t = Time(i);
        float saws = sawA.Next(f1 / renderRate) + sawB.Next(f2 / renderRate);
        float third = Sine(t * f3) * (0.25f + 0.35f * bright);
        tone.Process(saws * 0.5f);

        // Putter: noise gated by the firing rhythm (each saw cycle), low and soft.
        float firingPhase = t * f1 - Floor(t * f1);
        float gate = Decay(firingPhase, 0.18f);
        float putter = putterBand.Process(rng.Bipolar()) * gate * (0.55f - 0.25f * bright);
        out[i] = tone.low + tone.band * 0.3f * bright + third + putter;
    }
    Saturate(out, 1.5f);
    RemoveDc(out, renderRate);
    MakeLoop(out, EngineCrossfade(), EngineCrossfade() + InlineLoop(), EngineCrossfade());
    return out;
}

// The zeppelin's fall, a 4 s loop. fall 0: a roaring, crackling fire, metal groaning and the engines coughing out;
// fall 1: the fire with a rushing wind and a falling whistle over it, the engines gone.
static std::vector<float> ZeppelinFall(float fall, uint32_t seed)
{
    int loop = (int)renderRate * 4;
    int frames = loop + EngineCrossfade();
    float seconds = (float)frames / renderRate;
    Rng rng(seed);

    // The fire: a low roar with a slow flicker, and crackle.
    std::vector<float> out = Rumble(seconds, 420.0f, rng.Next());
    out.resize(frames, 0.0f);
    Normalise(out);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] *= 0.45f + 0.15f * Sine(Time((int)i) * 1.25f) + 0.1f * Sine(Time((int)i) * 3.75f);
    std::vector<float> crackle = Crackle(seconds, 160.0f, 400.0f, 3500.0f, rng.Next());
    MixInto(out, crackle, 0, 0.5f);

    // Metal groans: slow sawtooths through a resonant bandpass, falling in pitch.
    for (int g = 0; g < 2; ++g) {
        float length = 1.4f;
        std::vector<float> groan(Frames(length));
        SawOsc saw;
        Biquad bp;
        bp.SetBandpass(260.0f + 90.0f * (float)g, 4.0f, renderRate);
        float base = rng.Range(48.0f, 70.0f);
        for (size_t i = 0; i < groan.size(); ++i) {
            float t = Time((int)i);
            float hz = base * (1.0f - 0.25f * t / length) + 2.0f * Sine(t * 2.3f);
            groan[i] = bp.Process(saw.Next(hz / renderRate)) * Sine(0.5f * t / length);
        }
        MixInto(out, groan, Frames(0.3f + 2.0f * (float)g), 0.6f * (1.0f - 0.5f * fall));
    }

    // The dying engines: irregular backfires, only while the fall begins.
    if (fall < 0.5f) {
        float t = 0.2f;
        while (t < seconds - 0.3f) {
            std::vector<float> pop = Thump(0.15f, 110.0f, 55.0f, 0.03f, 0.001f, 0.04f, renderRate);
            MixInto(pop, NoiseBurst(0.08f, 700.0f, 1.0f, 0.001f, 0.015f, rng.Next(), renderRate), 0, 1.2f);
            MixInto(out, pop, Frames(t), rng.Range(0.3f, 0.7f));
            t += rng.Range(0.25f, 0.9f);
        }
    }

    // The fall: rushing air and a whistle (its pitch rises with the playback rate as the fall speeds up). Every
    // modulation has a whole number of cycles in the loop.
    if (fall > 0.5f) {
        Svf rush;
        rush.Set(900.0f, 1.2f, renderRate);
        float phase = 0.0f;
        for (size_t i = 0; i < out.size(); ++i) {
            float t = Time((int)i);
            rush.Process(rng.Bipolar());
            phase += (1100.0f + 40.0f * Sine(t * 0.5f)) / renderRate;
            out[i] += rush.band * 0.5f + Sine(phase) * 0.08f * (0.8f + 0.2f * Sine(t * 0.75f));
        }
    }
    RemoveDc(out, renderRate);
    MakeLoop(out, EngineCrossfade(), EngineCrossfade() + loop, EngineCrossfade());
    return out;
}

// Supercharger whine: WHINE_BASE_HZ with a little second harmonic and gear roughness. 0.5 s, whole cycles.
static std::vector<float> Whine()
{
    int loop = (int)renderRate / 2;
    int fade = (int)renderRate / 20;
    int frames = loop + fade;
    std::vector<float> out(frames);
    Rng rng(1300);
    Biquad hiss;
    hiss.SetBandpass(WHINE_BASE_HZ * 2.0f, 4.0f, renderRate);
    for (int i = 0; i < frames; ++i) {
        float t = Time(i);
        float wobble = 1.0f + 0.15f * Sine(t * 6.0f);
        out[i] = Sine(t * WHINE_BASE_HZ) * wobble + Sine(t * WHINE_BASE_HZ * 2.0f) * 0.25f +
                 hiss.Process(rng.Bipolar()) * 0.6f;
    }
    MakeLoop(out, fade, fade + loop, fade);
    return out;
}

// Rushing air: broadband noise with a slow swell, 1.5 s.
static std::vector<float> Wind()
{
    int loop = (int)renderRate * 3 / 2;
    int fade = (int)renderRate / 5;
    int frames = loop + fade;
    std::vector<float> out(frames);
    Rng rng(1400);
    Biquad hp;
    hp.SetHighpass(250.0f, 0.7f, renderRate);
    Biquad lp;
    lp.SetLowpass(3200.0f, 0.6f, renderRate);
    for (int i = 0; i < frames; ++i)
        out[i] = lp.Process(hp.Process(rng.Bipolar()));
    MakeLoop(out, fade, fade + loop, fade);
    return out;
}

// --- Bank

// Engine loops are normalised lower than effects: several play at once.
constexpr float ENGINE_PEAK = 0.7f;

static Sample MakeEngineSample(const std::vector<float>& buffer)
{
    return MakeSample(buffer, (uint32_t)renderRate, ENGINE_PEAK, EngineCrossfade(), (uint32_t)buffer.size());
}

// Which parts are built, as the mixer and the game thread see them: the menu sounds, then everything.
static std::atomic<bool> menuReady{false};
static std::atomic<bool> bankReady{false};

void BuildMenuSounds()
{
    if (menuReady.load(std::memory_order_acquire))
        return;
    renderRate = (float)SFX_RATE;
    AddVariant(Sfx::MENU_MOVE, MenuMove());
    AddVariant(Sfx::MENU_SELECT, MenuSelect());
    menuReady.store(true, std::memory_order_release);
}

void BuildSfxBank()
{
    BuildMenuSounds();
    if (bankReady.load(std::memory_order_acquire))
        return;

    // Short, bright sounds at the full rate.
    renderRate = (float)SFX_RATE;
    for (int v = 0; v < 4; ++v)
        AddVariant(Sfx::GUN, GunShot(v));
    for (int v = 0; v < 4; ++v)
        AddVariant(Sfx::HIT, MetalHit(v));
    AddVariant(Sfx::PICKUP, PickupChime());
    AddVariant(Sfx::RING, RingDing());
    AddVariant(Sfx::WARNING, WarningBeep());
    AddVariant(Sfx::OVERHEAT, OverheatHiss());
    for (int v = 0; v < 2; ++v)
        AddVariant(Sfx::GLASS_CRACK, GlassCrack(v));
    for (int v = 0; v < 2; ++v)
        AddVariant(Sfx::RADIO_BLIP, RadioBlip(v));
    for (int v = 0; v < 3; ++v)
        AddVariant(Sfx::CRUNCH, Crunch(v));
    for (int v = 0; v < 2; ++v)
        AddVariant(Sfx::KILL_STING, KillSting(v));

    // Voices.
    renderRate = (float)VOICE_RATE;
    BuildMonkeyCheers();
    BuildMonkeyScreeches();
    BuildGorillaTaunts();
    for (int v = 0; v < 3; ++v)
        AddVariant(Sfx::SPUTTER, Sputter(v));

    // Long and low sounds, and the engine loops.
    renderRate = (float)LOW_RATE;
    for (int v = 0; v < 2; ++v)
        AddVariant(Sfx::ROCKET, RocketLaunch(v));
    for (int v = 0; v < 3; ++v)
        AddVariant(Sfx::EXPLOSION, Explosion(v));
    AddVariant(Sfx::BIG_EXPLOSION, BigExplosion());
    for (int v = 0; v < 2; ++v)
        AddVariant(Sfx::SPLASH, Splash(v));
    for (int v = 0; v < 2; ++v)
        AddVariant(Sfx::STUNT, Swoosh(v));
    AddVariant(Sfx::ZEPPELIN_DEATH, ZeppelinDeath());
    engineLoops[(int)EngineType::PIRATE][(int)EngineLayer::LOW] = MakeEngineSample(RadialEngine(1.0f, 1500));
    engineLoops[(int)EngineType::PIRATE][(int)EngineLayer::HIGH] = MakeEngineSample(RadialEngine(0.0f, 1501));
    engineLoops[(int)EngineType::PLAYER][(int)EngineLayer::LOW] = MakeEngineSample(InlineEngine(0.0f, 1502));
    engineLoops[(int)EngineType::PLAYER][(int)EngineLayer::HIGH] = MakeEngineSample(InlineEngine(1.0f, 1503));
    engineLoops[(int)EngineType::ZEPPELIN_FALL][(int)EngineLayer::LOW] = MakeEngineSample(ZeppelinFall(0.0f, 1504));
    engineLoops[(int)EngineType::ZEPPELIN_FALL][(int)EngineLayer::HIGH] = MakeEngineSample(ZeppelinFall(1.0f, 1505));
    std::vector<float> whine = Whine();
    whineLoop = MakeSample(whine, LOW_RATE, ENGINE_PEAK, LOW_RATE / 20, (uint32_t)whine.size());
    std::vector<float> wind = Wind();
    windLoop = MakeSample(wind, LOW_RATE, ENGINE_PEAK, LOW_RATE / 5, (uint32_t)wind.size());
    bankReady.store(true, std::memory_order_release);
}

bool SfxReady(Sfx sfx)
{
    if (sfx == Sfx::MENU_MOVE || sfx == Sfx::MENU_SELECT)
        return menuReady.load(std::memory_order_acquire);
    return bankReady.load(std::memory_order_acquire);
}

bool EnginesReady()
{
    return bankReady.load(std::memory_order_acquire);
}

int SfxVariantCount(Sfx sfx)
{
    return sfxVariants[(int)sfx];
}

const Sample& SfxVariant(Sfx sfx, int variant)
{
    return sfxBank[(int)sfx][variant];
}

const Sample& EngineLoop(EngineType type, EngineLayer layer)
{
    return engineLoops[(int)type][(int)layer];
}

const Sample& WhineLoop()
{
    return whineLoop;
}

const Sample& WindLoop()
{
    return windLoop;
}

uint32_t SfxBankBytes()
{
    uint32_t bytes = 0;
    for (int s = 0; s < (int)Sfx::COUNT; ++s) {
        for (int v = 0; v < sfxVariants[s]; ++v)
            bytes += SampleBytes(sfxBank[s][v]);
    }
    for (int t = 0; t < (int)EngineType::COUNT; ++t) {
        for (int l = 0; l < 2; ++l)
            bytes += SampleBytes(engineLoops[t][l]);
    }
    return bytes + SampleBytes(whineLoop) + SampleBytes(windLoop);
}

} // namespace audio
