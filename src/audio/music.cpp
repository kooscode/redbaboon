// Music: a small swing band synthesised at start-up (clarinet, trumpets, trombones, upright bass, marimba, tenor
// banjo, drums and timpani) and the five tracks, written in a compact note notation and compiled into note lists
// for the mixer's sequencer (docs/design.md §6, creative brief §7). All melodies are original.
//
// Notation of a part: tokens separated by spaces; '|' marks bars and is ignored.
//   c5:2     note C5 for 2 eighths (durations are in eighth notes; decimals allowed, 0.667 = a triplet eighth)
//   f#4:1    sharp, bb4:1 flat (B-flat), b4:1 B natural
//   r:4      rest
//   T-12     transposes the following notes by -12 semitones
// Suffixes after the duration: '.' staccato, '>' accent, '~' legato (tied to the next note), '_' rings out (a
// RING_OUT fade after the note, for the last chord of a fanfare).
// Brass stabs: x stab, X accent, L long (each '-' holds it an eighth more), . rest; a long stab that ends a song
// that plays once rings out the same way.
// Swing: an eighth on the off-beat starts `swing` ticks (of 24) into the beat; 12 is straight, 16 a triplet swing.
#include "audio/music.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>

namespace audio {

// The rate instruments are rendered at, as a float.
constexpr float RATE = (float)MUSIC_RATE;

// Peak level instrument samples are normalised to.
constexpr float NOTE_PEAK = 0.9f;

// Frames in `seconds` at the instrument rate.
static int Frames(float seconds)
{
    return (int)(seconds * RATE);
}

// Seconds at frame i.
static float Time(int i)
{
    return (float)i / RATE;
}

// --- Instruments

enum Instrument {
    CLARINET,
    TRUMPET,
    TROMBONE,
    BASS,
    MARIMBA,
    INSTRUMENT_COUNT
};

// How an instrument sits in the mix.
struct InstrumentStyle {
    float gain;
    float pan;              // -1 left .. 1 right
    float release;          // seconds of fade at note-off
    int lowestRoot;         // MIDI note of the lowest multisample
    int rootStep;           // semitones between multisamples
    int rootCount;
};

static const InstrumentStyle INSTRUMENT_STYLES[INSTRUMENT_COUNT] = {
    {0.50f, -0.15f, 0.06f, 53, 7, 5},     // CLARINET: F3 .. A5
    {0.46f, 0.20f, 0.07f, 55, 7, 6},      // TRUMPET: G3 .. F#6
    {0.50f, -0.30f, 0.08f, 36, 7, 4},     // TROMBONE: C2 .. A3
    {0.80f, 0.00f, 0.05f, 29, 7, 4},      // BASS: F1 .. D3
    {0.42f, -0.25f, 0.10f, 62, 8, 4},     // MARIMBA: D4 .. D6
};

// The multisamples of an instrument and the frequency each was rendered at.
struct MultiSample {
    std::vector<Sample> samples;
    std::vector<float> hz;
    std::vector<int> roots;
};

static MultiSample instruments[INSTRUMENT_COUNT];

// Sustained notes loop over the last LOOP_SECONDS after LOOP_START seconds of attack. Oscillator frequencies are
// rounded to whole cycles per loop so the loop point is seamless.
constexpr float LOOP_START = 0.25f;
constexpr float LOOP_SECONDS = 0.8f;
constexpr float LOOP_CROSSFADE = 0.1f;

// Rounds hz to a whole number of cycles per loop.
static float LoopHz(float hz)
{
    return roundf(hz * LOOP_SECONDS) / LOOP_SECONDS;
}

// Vibrato rate: four cycles per loop (5 Hz).
constexpr float VIBRATO_HZ = 4.0f / LOOP_SECONDS;

// Turns a rendered sustained note into a looping sample.
static Sample MakeSustainSample(std::vector<float>& buffer)
{
    int loopStart = Frames(LOOP_START);
    int loopEnd = loopStart + Frames(LOOP_SECONDS);
    MakeLoop(buffer, loopStart, loopEnd, Frames(LOOP_CROSSFADE));
    return MakeSample(buffer, MUSIC_RATE, NOTE_PEAK, (uint32_t)loopStart, (uint32_t)loopEnd);
}

// Clarinet: a square wave (odd harmonics) through a gentle lowpass, a breath chiff and delayed vibrato.
static Sample ClarinetNote(float midi, float& hz)
{
    hz = LoopHz(NoteHz(midi));
    int frames = Frames(LOOP_START + LOOP_SECONDS);
    std::vector<float> out(frames);
    PulseOsc osc;
    Svf tone;
    Biquad breathBand;
    breathBand.SetBandpass(2200.0f, 1.0f, RATE);
    Rng rng((uint32_t)midi * 13u + 1u);
    float phase = 0.0f;
    for (int i = 0; i < frames; ++i) {
        float t = Time(i);
        float depth = t < 0.08f ? 0.0f : (t < 0.22f ? (t - 0.08f) / 0.14f : 1.0f);
        float f = hz * (1.0f + 0.0035f * depth * Sine(t * VIBRATO_HZ));
        if ((i & 15) == 0) {
            float open = t < 0.04f ? 2.0f + 2.5f * t / 0.04f : 4.5f;
            tone.Set(fminf(f * open, 5000.0f), 0.7f, RATE);
        }
        phase += f / RATE;
        float v = osc.Next(f / RATE, 0.5f) + 0.3f * Sine(phase);
        tone.Process(v);
        float attack = t < 0.03f ? t / 0.03f : 1.0f;
        float breath = breathBand.Process(rng.Bipolar()) * (0.05f + 0.25f * Decay(t, 0.03f));
        out[i] = (tone.low + breath) * attack;
    }
    return MakeSustainSample(out);
}

// Brass: two slightly detuned sawtooths through a lowpass that opens with the attack ("blat") and a small pitch
// scoop. dark 0 = trumpet, 1 = trombone.
static Sample BrassNote(float midi, float dark, float& hz)
{
    float nominal = NoteHz(midi);
    float f1 = LoopHz(nominal);
    float detune = fmaxf(1.0f, roundf(nominal * 0.005f * LOOP_SECONDS)) / LOOP_SECONDS;
    float f2 = f1 + detune;
    hz = (f1 + f2) * 0.5f;
    int frames = Frames(LOOP_START + LOOP_SECONDS);
    std::vector<float> out(frames);
    SawOsc sawA;
    SawOsc sawB;
    sawB.phase = 0.5f;
    Svf tone;
    float bright = 5.0f - 2.6f * dark;
    for (int i = 0; i < frames; ++i) {
        float t = Time(i);
        float scoop = t < 0.05f ? 1.0f - 0.015f * (1.0f - t / 0.05f) : 1.0f;
        float depth = t < 0.15f ? 0.0f : (t < 0.24f ? (t - 0.15f) / 0.09f : 1.0f);
        float wobble = scoop * (1.0f + 0.002f * depth * Sine(t * VIBRATO_HZ));
        if ((i & 15) == 0) {
            float swell = t < 0.04f ? 1.4f * t / 0.04f : (t < 0.2f ? 1.4f - 0.4f * (t - 0.04f) / 0.16f : 1.0f);
            tone.Set(fminf(hz * (1.2f + bright * swell), 9000.0f), 0.9f, RATE);
        }
        float v = sawA.Next(f1 * wobble / RATE) + sawB.Next(f2 * wobble / RATE);
        tone.Process(v * 0.5f);
        float attack = t < 0.02f ? t / 0.02f : 1.0f;
        out[i] = SoftClip(tone.low * 1.5f) * attack;
    }
    return MakeSustainSample(out);
}

// Upright bass: a dark plucked string with a strong fundamental and a finger thump.
static Sample BassNote(float midi, float& hz)
{
    hz = NoteHz(midi);
    std::vector<float> out = Pluck(hz, 1.2f, 0.3f, RATE, (uint32_t)midi * 7u + 3u);
    std::vector<float> body(out.size());
    float phase = 0.0f;
    EnvelopeAD env(0.004f, 0.45f, RATE);
    for (float& v : body) {
        phase += hz / RATE;
        v = Sine(phase) * env.Next();
    }
    MixInto(out, body, 0, 0.6f);
    MixInto(out, Thump(0.06f, hz * 3.0f, hz * 1.5f, 0.02f, 0.001f, 0.012f, RATE), 0, 0.3f);
    Biquad lp;
    lp.SetLowpass(1400.0f, 0.7f, RATE);
    for (float& v : out)
        v = lp.Process(v);
    FadeEdges(out, 8, Frames(0.1f));
    return MakeSample(out, MUSIC_RATE, NOTE_PEAK);
}

// Marimba: wooden bar modes (1 : 3.93 : 9.2) and a soft mallet.
static Sample MarimbaNote(float midi, float& hz)
{
    hz = NoteHz(midi);
    float scale = powf(440.0f / hz, 0.3f);
    const float ratios[3] = {1.0f, 3.93f, 9.2f};
    const float gains[3] = {1.0f, 0.3f, 0.08f};
    const float taus[3] = {0.4f * scale, 0.1f * scale, 0.025f * scale};
    std::vector<float> out = Modes(0.9f, hz, ratios, gains, taus, 3, RATE);
    MixInto(out, NoiseBurst(0.01f, hz * 4.0f, 1.0f, 0.0005f, 0.002f, (uint32_t)midi, RATE), 0, 0.2f);
    FadeEdges(out, 8, Frames(0.1f));
    return MakeSample(out, MUSIC_RATE, NOTE_PEAK);
}

static void BuildInstrument(Instrument instrument)
{
    const InstrumentStyle& style = INSTRUMENT_STYLES[instrument];
    MultiSample& multi = instruments[instrument];
    for (int r = 0; r < style.rootCount; ++r) {
        int root = style.lowestRoot + r * style.rootStep;
        float hz = 0.0f;
        Sample sample;
        if (instrument == CLARINET)
            sample = ClarinetNote((float)root, hz);
        else if (instrument == TRUMPET)
            sample = BrassNote((float)root, 0.0f, hz);
        else if (instrument == TROMBONE)
            sample = BrassNote((float)root, 1.0f, hz);
        else if (instrument == BASS)
            sample = BassNote((float)root, hz);
        else
            sample = MarimbaNote((float)root, hz);
        multi.samples.push_back(sample);
        multi.hz.push_back(hz);
        multi.roots.push_back(root);
    }
}

// --- Drums

enum Drum {
    KICK,
    BRUSH,
    SNARE,
    RIDE,
    HAT,
    CRASH,
    TOM_LOW,
    TOM_HIGH,
    TIMPANI,
    DRUM_COUNT
};

// How a drum sits in the mix, and the channel it plays on.
struct DrumStyle {
    float gain;
    float pan;
    float release;          // fade when the next hit on its channel cuts it
    int channel;
};

// Channels of the song layout (see the sequencer in mixer.cpp: a new note on a channel releases the old one).
constexpr int PART_CHANNEL = 0;          // written parts: 0 .. 3
constexpr int BASS_CHANNEL = 4;
constexpr int BANJO_CHANNEL = 5;
constexpr int BRASS_CHANNEL = 6;         // stab chords: 6 .. 9

static const DrumStyle DRUM_STYLES[DRUM_COUNT] = {
    {0.80f, 0.00f, 0.03f, 10},      // KICK
    {0.42f, 0.10f, 0.05f, 11},      // BRUSH
    {0.55f, 0.10f, 0.05f, 11},      // SNARE
    {0.20f, 0.35f, 0.10f, 12},      // RIDE: alternates between 12 and 13 so each hit rings on
    {0.24f, -0.25f, 0.02f, 14},     // HAT
    {0.30f, -0.35f, 0.20f, 15},     // CRASH
    {0.50f, -0.20f, 0.05f, 16},     // TOM_LOW
    {0.60f, 0.25f, 0.05f, 16},      // TOM_HIGH
    {0.70f, 0.00f, 0.08f, 17},      // TIMPANI
};

// The pitch the timpani sample is rendered at (G2).
constexpr float TIMPANI_ROOT = 43.0f;

static Sample drums[DRUM_COUNT];

// Metallic noise: six square waves at inharmonic frequencies (cymbals and hi-hat).
static std::vector<float> Metal(float seconds, float scale)
{
    constexpr float FREQS[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
    std::vector<float> out(Frames(seconds), 0.0f);
    for (int k = 0; k < 6; ++k) {
        PulseOsc osc;
        osc.phase = 0.13f * (float)k;
        float increment = FREQS[k] * scale / RATE;
        for (float& v : out)
            v += osc.Next(increment, 0.5f);
    }
    return out;
}

static std::vector<float> Cymbal(float seconds, float scale, float hp, float attack, float tau, uint32_t seed)
{
    std::vector<float> out = Metal(seconds, scale);
    Rng rng(seed);
    Biquad high;
    high.SetHighpass(hp, 0.8f, RATE);
    Biquad high2;
    high2.SetHighpass(hp, 0.8f, RATE);
    EnvelopeAD env(attack, tau, RATE);
    for (float& v : out)
        v = high2.Process(high.Process(v * 0.15f + rng.Bipolar() * 0.5f)) * env.Next();
    return out;
}

static void BuildDrums()
{
    std::vector<float> kick = Thump(0.45f, 115.0f, 48.0f, 0.05f, 0.001f, 0.13f, RATE);
    MixInto(kick, NoiseBurst(0.01f, 3000.0f, 0.8f, 0.0003f, 0.002f, 1, RATE), 0, 0.5f);
    drums[KICK] = MakeSample(kick, MUSIC_RATE, NOTE_PEAK);

    std::vector<float> brush = NoiseBurst(0.3f, 2600.0f, 0.5f, 0.004f, 0.05f, 2, RATE);
    MixInto(brush, Thump(0.08f, 200.0f, 190.0f, 0.01f, 0.001f, 0.025f, RATE), 0, 0.15f);
    drums[BRUSH] = MakeSample(brush, MUSIC_RATE, NOTE_PEAK);

    const float snareRatios[2] = {1.0f, 1.78f};
    const float snareGains[2] = {1.0f, 0.6f};
    const float snareTaus[2] = {0.05f, 0.035f};
    std::vector<float> snare = Modes(0.4f, 185.0f, snareRatios, snareGains, snareTaus, 2, RATE);
    std::vector<float> wires = NoiseBurst(0.4f, 4000.0f, 0.4f, 0.001f, 0.1f, 3, RATE);
    MixInto(snare, wires, 0, 1.6f);
    drums[SNARE] = MakeSample(snare, MUSIC_RATE, NOTE_PEAK);

    std::vector<float> ride = Cymbal(1.3f, 1.45f, 4500.0f, 0.001f, 0.45f, 4);
    const float bellRatios[3] = {1.0f, 1.48f, 1.97f};
    const float bellGains[3] = {1.0f, 0.6f, 0.4f};
    const float bellTaus[3] = {0.5f, 0.35f, 0.25f};
    MixInto(ride, Modes(1.3f, 2650.0f, bellRatios, bellGains, bellTaus, 3, RATE), 0, 0.12f);
    drums[RIDE] = MakeSample(ride, MUSIC_RATE, NOTE_PEAK);

    std::vector<float> hat = Cymbal(0.15f, 2.0f, 6500.0f, 0.001f, 0.022f, 5);
    MixInto(hat, Thump(0.05f, 140.0f, 120.0f, 0.01f, 0.001f, 0.01f, RATE), 0, 0.1f);
    drums[HAT] = MakeSample(hat, MUSIC_RATE, NOTE_PEAK);

    std::vector<float> crash = Cymbal(2.2f, 1.2f, 3000.0f, 0.004f, 0.8f, 6);
    drums[CRASH] = MakeSample(crash, MUSIC_RATE, NOTE_PEAK);

    const float tomRatios[2] = {1.0f, 1.5f};
    const float tomGains[2] = {0.0f, 0.25f};
    const float tomTaus[2] = {0.2f, 0.1f};
    std::vector<float> tomLow = Thump(0.6f, 118.0f, 86.0f, 0.08f, 0.001f, 0.22f, RATE);
    MixInto(tomLow, Modes(0.6f, 86.0f, tomRatios, tomGains, tomTaus, 2, RATE), 0, 1.0f);
    MixInto(tomLow, NoiseBurst(0.03f, 700.0f, 1.0f, 0.0005f, 0.012f, 7, RATE), 0, 0.6f);
    drums[TOM_LOW] = MakeSample(tomLow, MUSIC_RATE, NOTE_PEAK);
    std::vector<float> tomHigh = Thump(0.5f, 180.0f, 142.0f, 0.06f, 0.001f, 0.17f, RATE);
    MixInto(tomHigh, NoiseBurst(0.03f, 900.0f, 1.0f, 0.0005f, 0.01f, 8, RATE), 0, 0.6f);
    drums[TOM_HIGH] = MakeSample(tomHigh, MUSIC_RATE, NOTE_PEAK);

    const float timpRatios[5] = {1.0f, 1.504f, 1.742f, 2.0f, 2.245f};
    const float timpGains[5] = {1.0f, 0.5f, 0.25f, 0.3f, 0.15f};
    const float timpTaus[5] = {1.0f, 0.65f, 0.45f, 0.4f, 0.3f};
    std::vector<float> timpani = Modes(2.0f, NoteHz(TIMPANI_ROOT), timpRatios, timpGains, timpTaus, 5, RATE);
    MixInto(timpani, Thump(0.1f, 220.0f, 98.0f, 0.02f, 0.001f, 0.04f, RATE), 0, 0.5f);
    MixInto(timpani, NoiseBurst(0.03f, 350.0f, 1.0f, 0.001f, 0.01f, 9, RATE), 0, 0.4f);
    FadeEdges(timpani, 8, Frames(0.3f));
    drums[TIMPANI] = MakeSample(timpani, MUSIC_RATE, NOTE_PEAK);
}

// --- Defeat sting

// The sad trombone sample.
static Sample sadTrombone;

// "Wah wah wah waaah": four falling trombone notes through a plunger-mute filter that opens and closes on each one;
// the last wobbles and sags.
static void BuildSadTrombone()
{
    constexpr float NOTES[4] = {55.0f, 54.0f, 53.0f, 52.0f};
    constexpr float STARTS[4] = {0.0f, 0.5f, 1.0f, 1.5f};
    constexpr float LENGTHS[4] = {0.44f, 0.44f, 0.44f, 1.9f};
    std::vector<float> out(Frames(3.6f), 0.0f);
    for (int n = 0; n < 4; ++n) {
        bool last = n == 3;
        int start = Frames(STARTS[n]);
        int frames = Frames(LENGTHS[n]);
        SawOsc sawA;
        SawOsc sawB;
        Svf mute;
        float hz = NoteHz(NOTES[n]);
        for (int i = 0; i < frames && start + i < (int)out.size(); ++i) {
            float t = Time(i);
            float k = t / LENGTHS[n];
            float sag = last ? 1.0f - 0.06f * fmaxf(0.0f, k - 0.55f) / 0.45f : 1.0f - 0.012f * k * k;
            float vibrato = last ? 1.0f + 0.012f * Sine(t * 5.5f) * fminf(1.0f, t / 0.3f) : 1.0f;
            float f = hz * sag * vibrato;
            if ((i & 15) == 0) {
                float open = last ? 0.5f + 0.5f * Sine(t * 3.2f - 0.25f) : Sine(0.5f * fminf(1.0f, k * 1.2f));
                mute.Set(250.0f + 1300.0f * open * open, 2.2f, RATE);
            }
            mute.Process(sawA.Next(f / RATE) + sawB.Next(f * 1.004f / RATE));
            float attack = t < 0.025f ? t / 0.025f : 1.0f;
            float release = LENGTHS[n] - t < 0.06f ? (LENGTHS[n] - t) / 0.06f : 1.0f;
            float fade = last ? 1.0f - 0.7f * k : 1.0f;
            out[start + i] += (mute.band * 1.2f + mute.low * 0.5f) * attack * release * fade;
        }
    }
    for (float& v : out)
        v = SoftClip(v * 1.4f);
    FadeEdges(out, 8, Frames(0.05f));
    sadTrombone = MakeSample(out, MUSIC_RATE, NOTE_PEAK);
}

// --- Chords

// A chord: root pitch class and intervals above it.
struct Chord {
    int root = 0;
    int count = 0;
    int tones[5] = {};
};

// A chord over a span of the song, in eighths.
struct ChordSpan {
    float start;
    float end;
    Chord chord;
};

struct ChordQuality {
    const char* name;
    int count;
    int tones[5];
};

static const ChordQuality CHORD_QUALITIES[] = {
    {"", 3, {0, 4, 7}},
    {"6", 4, {0, 4, 7, 9}},
    {"7", 4, {0, 4, 7, 10}},
    {"9", 4, {0, 4, 10, 14}},
    {"maj7", 4, {0, 4, 7, 11}},
    {"m", 3, {0, 3, 7}},
    {"m6", 4, {0, 3, 7, 9}},
    {"m7", 4, {0, 3, 7, 10}},
    {"m7b5", 4, {0, 3, 6, 10}},
    {"dim", 4, {0, 3, 6, 9}},
    {"aug", 3, {0, 4, 8}},
};

// Pitch class of a note letter (either case), or -1.
static int LetterPitch(char c)
{
    constexpr int PITCHES[7] = {9, 11, 0, 2, 4, 5, 7};
    if (c >= 'a' && c <= 'g')
        return PITCHES[c - 'a'];
    if (c >= 'A' && c <= 'G')
        return PITCHES[c - 'A'];
    return -1;
}

// Problems found while compiling the tracks.
static char dataErrors[512];

static void ReportError(const char* what, const char* where)
{
    size_t used = strlen(dataErrors);
    snprintf(dataErrors + used, sizeof(dataErrors) - used, "%s: %s\n", where, what);
}

// Parses a chord symbol ("F6", "Bbm7", "C#dim") of `length` characters.
static bool ParseChord(const char* text, int length, Chord& chord)
{
    int root = LetterPitch(text[0]);
    if (root < 0)
        return false;
    int i = 1;
    if (i < length && text[i] == '#') {
        ++root;
        ++i;
    } else if (i < length && text[i] == 'b') {
        --root;
        ++i;
    }
    chord.root = (root + 12) % 12;
    for (const ChordQuality& quality : CHORD_QUALITIES) {
        if ((int)strlen(quality.name) == length - i && strncmp(quality.name, text + i, length - i) == 0) {
            chord.count = quality.count;
            for (int t = 0; t < quality.count; ++t)
                chord.tones[t] = quality.tones[t];
            return true;
        }
    }
    return false;
}

// Parses "F6 | D7 | Gm7 C7 | ..." into spans; returns the number of bars.
static int ParseChords(const char* text, std::vector<ChordSpan>& spans, const char* trackName)
{
    int bar = 0;
    const char* p = text;
    while (*p) {
        const char* barEnd = strchr(p, '|');
        if (!barEnd)
            barEnd = p + strlen(p);
        Chord chords[4];
        int count = 0;
        const char* q = p;
        while (q < barEnd) {
            while (q < barEnd && *q == ' ')
                ++q;
            const char* start = q;
            while (q < barEnd && *q != ' ')
                ++q;
            if (q > start && count < 4) {
                if (ParseChord(start, (int)(q - start), chords[count]))
                    ++count;
                else
                    ReportError("bad chord symbol", trackName);
            }
        }
        for (int c = 0; c < count; ++c) {
            float length = 8.0f / (float)count;
            spans.push_back({(float)bar * 8.0f + length * (float)c, (float)bar * 8.0f + length * (float)(c + 1),
                             chords[c]});
        }
        if (count > 0)
            ++bar;
        p = *barEnd ? barEnd + 1 : barEnd;
    }
    return bar;
}

// The chord sounding at an eighth position.
static const Chord& ChordAt(const std::vector<ChordSpan>& spans, float eighth)
{
    for (const ChordSpan& span : spans) {
        if (eighth < span.end)
            return span.chord;
    }
    return spans.back().chord;
}

// A close-position voicing of up to maxNotes notes, its average pitch nearest `center`. With more chord tones than
// maxNotes the root is left out (the bass has it). Returns the number of notes written to `notes`.
static int Voice(const Chord& chord, int center, int maxNotes, int* notes)
{
    int pcs[5];
    int count = 0;
    int first = chord.count > maxNotes ? 1 : 0;
    for (int t = first; t < chord.count && count < maxNotes; ++t)
        pcs[count++] = (chord.root + chord.tones[t]) % 12;
    float bestDistance = 1e9f;
    for (int low = center - 8; low <= center + 4; ++low) {
        int candidate[5];
        float sum = 0.0f;
        for (int n = 0; n < count; ++n) {
            int note = low + ((pcs[n] - low) % 12 + 12) % 12;
            candidate[n] = note;
            sum += (float)note;
        }
        float distance = fabsf(sum / (float)count - (float)center);
        if (distance < bestDistance) {
            bestDistance = distance;
            for (int n = 0; n < count; ++n)
                notes[n] = candidate[n];
        }
    }
    std::sort(notes, notes + count);
    return count;
}

// --- Banjo chords

// Most distinct banjo chords over all tracks.
constexpr int MAX_BANJO_CHORDS = 40;

// Rendered strums, keyed by root * 16 + quality.
static Sample banjoChords[MAX_BANJO_CHORDS];
static int banjoKeys[MAX_BANJO_CHORDS];
static int banjoCount = 0;

// A tenor banjo down-strum: four bright plucked strings, 9 ms apart, through a drum-head resonance.
static std::vector<float> BanjoStrum(const Chord& chord)
{
    int notes[4];
    int count = Voice(chord, 62, 4, notes);
    std::vector<float> out;
    for (int n = 0; n < count; ++n) {
        std::vector<float> string = Pluck(NoteHz((float)notes[n]), 0.55f, 0.85f, RATE, (uint32_t)notes[n] * 31u);
        MixInto(out, string, Frames(0.009f * (float)n), 1.0f);
    }
    Biquad head;
    head.SetBandpass(480.0f, 1.5f, RATE);
    Biquad cut;
    cut.SetHighpass(180.0f, 0.7f, RATE);
    for (float& v : out)
        v = cut.Process(v + head.Process(v) * 1.5f);
    FadeEdges(out, 8, Frames(0.15f));
    return out;
}

static const Sample* BanjoChord(const Chord& chord)
{
    int key = chord.root * 16 + chord.count * 1000 + chord.tones[1] * 100 + chord.tones[chord.count - 1];
    for (int i = 0; i < banjoCount; ++i) {
        if (banjoKeys[i] == key)
            return &banjoChords[i];
    }
    if (banjoCount == MAX_BANJO_CHORDS)
        return &banjoChords[0];
    banjoKeys[banjoCount] = key;
    banjoChords[banjoCount] = MakeSample(BanjoStrum(chord), MUSIC_RATE, NOTE_PEAK);
    return &banjoChords[banjoCount++];
}

// --- Track definitions

enum class CompStyle {
    NONE,
    FOUR,           // banjo strums all four beats, 2 and 4 accented
};

enum class BassStyle {
    NONE,           // the bass is a written part
    WALK,           // walking quarter notes from the chords
};

enum class DrumPattern {
    SWING,          // ride, hi-hat on 2 and 4, feathered kick, brush snare rolls into each phrase
    JUNGLE,         // floor-tom drive with snare rolls
    BOSS,           // heavy backbeat, timpani on the chord roots, tom fills
    FANFARE,        // victory: snare roll into a crash and timpani
    NONE,
};

// Most segments one part is written in.
constexpr int MAX_SEGMENTS = 8;

// A written line: segments of notation played one after the other (nullptr ends the list early).
struct Part {
    Instrument instrument;
    float gain;
    const char* segments[MAX_SEGMENTS];
};

constexpr int MAX_PARTS = 4;

struct TrackDef {
    const char* name;
    float bpm;
    int swing;                  // ticks of the off-beat eighth (12 straight, 16 triplet)
    const char* chords;
    int introBars;              // bars played once before the loop
    bool loops;
    Part parts[MAX_PARTS];
    int partCount;
    const char* stabs;          // brass section rhythm per bar: x stab, X accent, L long (- holds), . rest
    Instrument stabInstrument;
    int stabCenter;             // average pitch of the stab voicings
    CompStyle comp;
    BassStyle bass;
    DrumPattern drums;
};

// TITLE: F major, AABA with a two-bar vamp intro. Marimba takes the first A, clarinet the second, trumpet the
// bridge, clarinet and marimba together the last.
constexpr char TITLE_AH[] =
    "c5:2 a4:1 c5:1 d5:2 c5:2 | a5:3 f#5:1 d5:2 c5:2 | bb4:2 d5:1 f5:1 g5:2 f5:2 | e5:3 d5:1 c5:2 bb4:2 | ";
constexpr char TITLE_A1T[] =
    "a4:1 c5:1 f5:2 f#5:2 a5:2 | g5:2 bb5:1 a5:1 g5:2 e5:2 | f5:3 d5:1 c5:4 | r:2 g4:1 a4:1 bb4:1 b4:1 r:2 | ";
constexpr char TITLE_A2T[] =
    "a4:1 c5:1 f5:2 eb5:2 c5:2 | d5:2 f5:1 d5:1 db5:2 f5:2 | c5:2 a4:1 f4:1 g4:2 e4:2 | f4:2 r:1 c5:1 f5:2 r:2 | ";
constexpr char TITLE_A3T[] =
    "a4:1 c5:1 f5:2 eb5:2 c5:2 | d5:2 f5:1 d5:1 db5:2 f5:2 | c5:2 a4:1 f4:1 f#4:2 a4:2 | "
    "bb4:2 g4:1 bb4:1 e5:1 d5:1 c5:1 b4:1 | ";
constexpr char TITLE_B[] =
    "e5:2 c#5:1 e5:1 a5:3 g5:1 | g5:1 f5:1 e5:1 c#5:1 a4:4 | d5:2 f5:1 a5:1 c6:3 a5:1 | f5:2 d5:2 a4:2 r:2 | "
    "b4:2 d5:1 f5:1 g5:3 f5:1 | f5:2 d5:2 b4:2 r:2 | e5:2 g5:1 bb5:1 c6:2 bb5:2 | "
    "g5:1 e5:1 c5:1 bb4:1 g4:1 a4:1 bb4:1 b4:1 | ";
constexpr char REST_2[] = "r:16 | ";
constexpr char REST_8[] = "r:64 | ";

static const TrackDef TITLE_TRACK = {
    "title", 160.0f, 16,
    "Gm7 C7 | Gm7 C7 | "
    "F6 | D7 | Gm7 | C7 | F6 D7 | Gm7 C7 | F6 | C7 | "
    "F6 | D7 | Gm7 | C7 | F6 F7 | Bb6 Bbm6 | F6 | F6 | "
    "A7 | A7 | Dm7 | Dm7 | G7 | G7 | C7 | C7 | "
    "F6 | D7 | Gm7 | C7 | F6 F7 | Bb6 Bbm6 | F6 D7 | Gm7 C7",
    2, true,
    {
        {MARIMBA, 1.0f, {REST_2, TITLE_AH, TITLE_A1T, REST_8, REST_8, TITLE_AH, TITLE_A3T}},
        {CLARINET, 1.0f, {REST_2, REST_8, TITLE_AH, TITLE_A2T, REST_8, TITLE_AH, TITLE_A3T}},
        {TRUMPET, 1.0f, {REST_2, REST_8, REST_8, TITLE_B, REST_8}},
    },
    3,
    "x..x..x.|X.....X.|"
    "x..x....|......x.|x..x....|......x.|x..x....|......x.|....x.x.|X.......|"
    "x..x....|......x.|x..x....|......x.|x..x....|....x...|x..x..x.|X.....x.|"
    "L-------|L-------|L-------|L-------|L-------|L-------|x..x..x.|x.x.x.x.|"
    "x..x....|......x.|x..x....|......x.|x..x....|....x...|x..x..x.|x.x.x.X.",
    TRUMPET, 60, CompStyle::FOUR, BassStyle::WALK, DrumPattern::SWING,
};

// COMBAT: G minor, 150 bpm swing over jungle toms. Trumpet riff, clarinet line, brass shout bridge, both at the end.
constexpr char COMBAT_RIFF[] =
    "d5:1. d5:1. r:1 bb4:1 d5:2 g5:2 | e5:1 d5:1 bb4:1 g4:1 a4:2 bb4:2 | "
    "eb5:1. eb5:1. r:1 c5:1 eb5:2 g5:2 | a5:1 g5:1 eb5:1 c5:1 d5:4 | "
    "d5:1. d5:1. r:1 bb4:1 d5:2 g5:2 | bb5:1 g5:1 db5:2 c5:1 a4:1 f#4:2 | "
    "g4:2 bb4:1 d5:1 g5:3 r:1 | f#5:1 g5:1 a5:1 f#5:1 d5:2 r:2 | ";
constexpr char COMBAT_LINE[] =
    "g5:1 a5:1 bb5:1 a5:1 g5:1 f#5:1 g5:1 d5:1 | e5:1 g5:1 bb5:2 a5:1 g5:1 e5:2 | "
    "c5:1 d5:1 eb5:1 g5:1 c6:2 bb5:1 g5:1 | a5:1 g5:1 eb5:1 c5:1 a4:2 r:2 | "
    "d5:1 g5:1 bb5:1 d6:1 c6:1 bb5:1 a5:1 g5:1 | bb5:1 g5:1 eb5:1 db5:1 c5:1 d5:1 f#5:1 a5:1 | "
    "g5:4 d5:2 bb4:2 | a4:1 c5:1 d5:1 f#5:1 a5:2 r:2 | ";
constexpr char COMBAT_SHOUT[] =
    "bb4:1. eb5:1. g5:1. bb5:3> r:2 | db6:2 bb5:1 g5:1 eb5:2 db5:2 | "
    "d5:1. g5:1. bb5:1. d6:3> r:2 | c6:1 bb5:1 a5:1 g5:1 f#5:2 g5:2 | "
    "g5:1. g5:1. r:1 eb5:1 g5:2 c6:2 | a5:2 f#5:1 d5:1 c5:2 a4:2 | "
    "g4:1 bb4:1 d5:2 db5:2 bb4:2 | a4:1 c5:1 d5:1 eb5:1 d5:1 c5:1 bb4:1 a4:1 | ";

static const TrackDef COMBAT_TRACK = {
    "combat", 150.0f, 15,
    "Gm | Gm6 | Cm | Cm6 | Gm | Eb7 D7 | Gm | D7 | "
    "Gm | Gm6 | Cm | Cm6 | Gm | Eb7 D7 | Gm | D7 | "
    "Eb7 | Eb7 | Gm | Gm | Cm | D7 | Gm Eb7 | D7 | "
    "Gm | Gm6 | Cm | Cm6 | Gm | Eb7 D7 | Gm | D7",
    0, true,
    {
        {TRUMPET, 1.0f, {COMBAT_RIFF, REST_8, COMBAT_SHOUT, COMBAT_RIFF}},
        {CLARINET, 0.85f, {REST_8, COMBAT_LINE, REST_8, COMBAT_LINE}},
    },
    2,
    "x.......|....x...|x.......|....x...|x.......|x...x...|x.......|x.x.x.x.|"
    "X..x..x.|......x.|X..x..x.|......x.|X..x..x.|x...x...|X..x..x.|x.x.x.x.|"
    "L-------|L---x.x.|L-------|L---x.x.|L-------|L-------|x..x..x.|X.X.X.X.|"
    "X..x..x.|......x.|X..x..x.|......x.|X..x..x.|x...x...|X..x..x.|x.x.x.X.",
    TROMBONE, 55, CompStyle::FOUR, BassStyle::WALK, DrumPattern::JUNGLE,
};

// BOSS: C minor, 130 bpm, a trombone ostinato doubled by the bass, a high trumpet over it, a low clarinet bridge
// over held trombone chords.
constexpr char BOSS_OSTINATO[] =
    "c3:3 c3:1 eb3:2 c3:2 | g3:3 f#3:1 g3:2 c3:2 | ab2:3 ab2:1 c3:2 eb3:2 | g2:3 g2:1 b2:2 d3:2 | "
    "c3:3 c3:1 eb3:2 c3:2 | f3:3 f3:1 ab3:2 c4:2 | db3:3 db3:1 f3:2 ab3:2 | "
    "g3:2 f3:1 eb3:1 d3:2 b2:2 | ";
constexpr char BOSS_LEAD[] =
    "g5:6 eb5:2 | c6:4 b5:2 c6:2 | eb6:4 c6:2 ab5:2 | b5:6 r:2 | g5:2 c6:2 eb6:2 d6:2 | c6:4 ab5:4 | "
    "f5:2 ab5:2 db6:2 b5:2 | b5:2 d6:2 f6:2 r:2 | ";
constexpr char BOSS_BRIDGE[] =
    "c5:2 ab4:2 f4:2 g4:1 ab4:1 | c5:3 db5:1 c5:4 | eb5:2 c5:2 g4:2 ab4:1 g4:1 | eb4:4 r:4 | "
    "eb5:2 c5:1 ab4:1 gb5:4 | f5:2 db5:2 ab4:2 b4:2 | b4:2 d5:2 f5:2 ab5:2 | g5:4 r:4 | ";
constexpr char BOSS_BRIDGE_BASS[] =
    "f2:4 c2:4 | f2:4 c2:4 | c2:4 g1:4 | c2:4 g1:4 | ab1:4 eb2:4 | db2:4 ab1:4 | g1:4 d2:4 | "
    "g1:4 b1:4 | ";

static const TrackDef BOSS_TRACK = {
    "boss", 130.0f, 14,
    "Cm | Cm | Ab7 | G7 | Cm | Fm | Db7 | G7 | "
    "Cm | Cm | Ab7 | G7 | Cm | Fm | Db7 | G7 | "
    "Fm | Fm | Cm | Cm | Ab7 | Db7 | G7 | G7 | "
    "Cm | Cm | Ab7 | G7 | Cm | Fm | Db7 | G7",
    0, true,
    {
        {TROMBONE, 0.9f, {BOSS_OSTINATO, BOSS_OSTINATO, REST_8, BOSS_OSTINATO}},
        {BASS, 1.0f, {"T-12", BOSS_OSTINATO, BOSS_OSTINATO, "T0", BOSS_BRIDGE_BASS, "T-12", BOSS_OSTINATO}},
        {TRUMPET, 1.0f, {REST_8, BOSS_LEAD, REST_8, BOSS_LEAD}},
        {CLARINET, 1.0f, {REST_8, REST_8, BOSS_BRIDGE, REST_8}},
    },
    4,
    "........|........|........|........|........|........|........|x.x.x.x.|"
    "........|........|........|........|........|........|........|x.x.x.x.|"
    "L-------|L-------|L-------|L-------|L-------|L-------|L-------|X.X.X.X.|"
    "........|........|........|........|........|........|........|X.X.X.X.",
    TROMBONE, 52, CompStyle::NONE, BassStyle::NONE, DrumPattern::BOSS,
};

// VICTORY: a three-bar fanfare in F, about six seconds.
static const TrackDef VICTORY_TRACK = {
    "victory", 120.0f, 12,
    "F | Bb C7 | F",
    0, false,
    {
        {TRUMPET, 1.0f, {"c5:0.667 f5:0.667 a5:0.666 c6:3> a5:1 c6:2 | d6:3 bb5:1 e6:2 g5:2 | f6:8>_ | "}},
        {CLARINET, 0.8f, {"r:8 | d5:1 f5:1 bb5:1 d6:1 c5:1 e5:1 g5:1 bb5:1 | a5:8_ | "}},
        {BASS, 1.0f, {"f2:2 c2:2 f2:2 a2:2 | bb1:2 d2:2 c2:2 e2:2 | f2:8_ | "}},
    },
    3,
    "X..x..x.|X...X...|L-------",
    TRUMPET, 62, CompStyle::NONE, BassStyle::NONE, DrumPattern::FANFARE,
};

// --- Compiling

// The compiled tracks, indexed by Music.
static Song songs[(int)Music::DEFEAT + 1];

// What the compiler of one track needs.
struct TrackBuilder {
    const TrackDef* def;
    Song* song;
    std::vector<ChordSpan> chords;
    int bars;
    float framesPerTick;
    Rng rng;
};

// Tick of an eighth position, with swing.
static uint32_t SwingTick(float eighth, int swing)
{
    float beat = Floor(eighth / 2.0f);
    float within = eighth - beat * 2.0f;
    float tick = within < 1.0f ? within * (float)swing : (float)swing + (within - 1.0f) * (float)(24 - swing);
    return (uint32_t)RoundToInt(beat * (float)TICKS_PER_BEAT + tick);
}

// Fade of a note marked to ring out (seconds).
constexpr float RING_OUT = 1.5f;

// Level of the whole band under the effects (the mixer's music volume comes on top).
constexpr float MUSIC_LEVEL = 0.5f;

// Output-frame fade length for a time in seconds.
static uint32_t ReleaseFrames(float seconds)
{
    return (uint32_t)(seconds * (float)MIX_RATE);
}

// Adds an event playing `sample` (rendered at sampleHz) at `hz`.
static void AddEvent(TrackBuilder& b, uint32_t tick, uint32_t lengthTicks, int channel, const Sample* sample,
                     float sampleHz, float hz, float gain, float pan, float release)
{
    float angle = (pan + 1.0f) * 0.25f * 3.14159265f;
    float g = fminf(gain * MUSIC_LEVEL, 1.0f) * 32767.0f;
    NoteEvent e;
    e.tick = tick;
    e.lengthTicks = lengthTicks;
    e.sample = sample;
    e.step = (uint32_t)RoundToInt((float)sample->rate / (float)MIX_RATE * hz / sampleHz * 65536.0f);
    e.gainL = (uint16_t)RoundToInt(fminf(g * cosf(angle) * 1.41421356f, 32767.0f));
    e.gainR = (uint16_t)RoundToInt(fminf(g * sinf(angle) * 1.41421356f, 32767.0f));
    e.releaseFrames = ReleaseFrames(release);
    e.channel = (uint8_t)channel;
    b.song->events.push_back(e);
}

// Adds a note of an instrument (MIDI pitch) from eighth `start` for `eighths`, gate as a fraction of the length.
static void AddNote(TrackBuilder& b, Instrument instrument, int channel, int midi, float start, float eighths,
                    float gate, float velocity, bool ringOut = false)
{
    const InstrumentStyle& style = INSTRUMENT_STYLES[instrument];
    const MultiSample& multi = instruments[instrument];
    int best = 0;
    for (int r = 1; r < (int)multi.roots.size(); ++r) {
        if (abs(multi.roots[r] - midi) < abs(multi.roots[best] - midi))
            best = r;
    }
    uint32_t tick = SwingTick(start, b.def->swing);
    uint32_t end = SwingTick(start + eighths, b.def->swing);
    uint32_t length = (uint32_t)fmaxf(1.0f, (float)(end - tick) * gate);
    AddEvent(b, tick, length, channel, &multi.samples[best], multi.hz[best], NoteHz((float)midi),
             style.gain * velocity, style.pan, ringOut ? RING_OUT : style.release);
}

static void AddDrum(TrackBuilder& b, uint32_t tick, Drum drum, float gain, int channelOffset = 0, float midi = 0.0f)
{
    const DrumStyle& style = DRUM_STYLES[drum];
    float hz = drum == TIMPANI ? NoteHz(midi) : 1.0f;
    float sampleHz = drum == TIMPANI ? NoteHz(TIMPANI_ROOT) : 1.0f;
    float human = 1.0f + 0.08f * b.rng.Bipolar();
    AddEvent(b, tick, 0, style.channel + channelOffset, &drums[drum], sampleHz, hz, style.gain * gain * human,
             style.pan, style.release);
}

// Reads one token of a part. Returns false at the end of the text.
struct Token {
    bool rest;
    int midi;
    float eighths;
    float gate;
    float velocity;
    bool ringOut;
};

// Reads a decimal number ("2", "0.667") and moves p past it (nxdk's strtof is not implemented).
static float ParseNumber(const char*& p)
{
    float value = 0.0f;
    while (*p >= '0' && *p <= '9') {
        value = value * 10.0f + (float)(*p - '0');
        ++p;
    }
    if (*p != '.' || p[1] < '0' || p[1] > '9')
        return value;
    ++p;
    float scale = 0.1f;
    while (*p >= '0' && *p <= '9') {
        value += (float)(*p - '0') * scale;
        scale *= 0.1f;
        ++p;
    }
    return value;
}

static bool NextToken(const char*& p, Token& token, int& transpose, const char* trackName)
{
    for (;;) {
        while (*p == ' ' || *p == '|')
            ++p;
        if (*p == 0)
            return false;
        if (*p != 'T')
            break;
        transpose = (int)strtol(p + 1, const_cast<char**>(&p), 10);
    }
    token.rest = *p == 'r';
    token.midi = 0;
    if (token.rest) {
        ++p;
    } else {
        int pc = LetterPitch(*p);
        if (pc < 0) {
            ReportError("bad note", trackName);
            return false;
        }
        ++p;
        if (*p == '#') {
            ++pc;
            ++p;
        } else if (*p == 'b' && p[1] >= '0' && p[1] <= '9') {
            --pc;
            ++p;
        }
        int octave = *p - '0';
        ++p;
        token.midi = (octave + 1) * 12 + pc + transpose;
    }
    token.eighths = 1.0f;
    if (*p == ':') {
        ++p;
        token.eighths = ParseNumber(p);
    }
    token.gate = 0.88f;
    token.velocity = 1.0f;
    token.ringOut = false;
    while (*p == '.' || *p == '>' || *p == '~' || *p == '_') {
        if (*p == '.')
            token.gate = 0.45f;
        else if (*p == '>')
            token.velocity = 1.2f;
        else if (*p == '~')
            token.gate = 1.0f;
        else
            token.ringOut = true;
        ++p;
    }
    return true;
}

static void CompilePart(TrackBuilder& b, const Part& part, int channel)
{
    Token token;
    int transpose = 0;
    float position = 0.0f;
    for (const char* segment : part.segments) {
        if (!segment)
            break;
        const char* p = segment;
        while (NextToken(p, token, transpose, b.def->name)) {
            if (!token.rest) {
                float velocity = part.gain * token.velocity * (1.0f + 0.04f * b.rng.Bipolar());
                AddNote(b, part.instrument, channel, token.midi, position, token.eighths, token.gate, velocity,
                        token.ringOut);
            }
            position += token.eighths;
        }
    }
    if (fabsf(position - (float)b.bars * 8.0f) > 0.01f) {
        char message[96];
        snprintf(message, sizeof(message), "part on channel %d is %d/100 eighths, chords are %d", channel,
                 RoundToInt(position * 100.0f), b.bars * 8);
        ReportError(message, b.def->name);
    }
}

static void CompileStabs(TrackBuilder& b)
{
    const char* p = b.def->stabs;
    float position = 0.0f;
    while (*p) {
        char c = *p++;
        if (c == '|' || c == ' ')
            continue;
        float length = 1.0f;
        float gate = 0.5f;
        float velocity = c == 'X' ? 1.15f : 0.85f;
        if (c == 'L') {
            while (*p == '-') {
                length += 1.0f;
                ++p;
            }
            gate = 0.95f;
            velocity = 0.6f;
        }
        if (c == 'x' || c == 'X' || c == 'L') {
            int notes[4];
            int count = Voice(ChordAt(b.chords, position), b.def->stabCenter, 4, notes);
            bool last = !b.def->loops && c == 'L' && position + length >= (float)b.bars * 8.0f - 0.01f;
            for (int n = 0; n < count; ++n) {
                AddNote(b, b.def->stabInstrument, BRASS_CHANNEL + n, notes[n], position, length, gate,
                        velocity * 0.6f, last);
            }
        }
        position += length;
    }
    if (fabsf(position - (float)b.bars * 8.0f) > 0.01f)
        ReportError("stab pattern length does not match the chords", b.def->name);
}

static void CompileComp(TrackBuilder& b)
{
    if (b.def->comp != CompStyle::FOUR)
        return;
    for (int beat = 0; beat < b.bars * 4; ++beat) {
        float position = (float)beat * 2.0f;
        const Sample* strum = BanjoChord(ChordAt(b.chords, position));
        float accent = (beat & 1) != 0 ? 1.0f : 0.75f;
        float gain = 0.34f * accent * (1.0f + 0.06f * b.rng.Bipolar());
        AddEvent(b, SwingTick(position, b.def->swing), 16, BANJO_CHANNEL, strum, 1.0f, 1.0f, gain, 0.4f, 0.05f);
    }
}

// The pitch with pitch class `pc` nearest to `near`, kept within the upright bass's walking range.
static int NearestPitch(int pc, int near)
{
    int best = 0;
    for (int note = 28; note <= 50; ++note) {
        if (note % 12 == pc && (best == 0 || abs(note - near) < abs(best - near)))
            best = note;
    }
    return best;
}

static void CompileWalkingBass(TrackBuilder& b)
{
    if (b.def->bass != BassStyle::WALK)
        return;
    int previous = 38;
    int loopSpan = 0;
    for (size_t s = 0; s < b.chords.size(); ++s) {
        if (b.chords[s].start >= (float)(b.def->introBars * 8)) {
            loopSpan = (int)s;
            break;
        }
    }
    for (size_t s = 0; s < b.chords.size(); ++s) {
        const ChordSpan& span = b.chords[s];
        const Chord& next = s + 1 < b.chords.size() ? b.chords[s + 1].chord : b.chords[loopSpan].chord;
        int beats = RoundToInt((span.end - span.start) / 2.0f);
        int root = NearestPitch(span.chord.root, previous);
        int note = root;
        for (int beat = 0; beat < beats; ++beat) {
            if (beat == 0) {
                note = root;
            } else if (beat == beats - 1) {
                int target = NearestPitch(next.root, note);
                if (b.rng.Float() < 0.25f)
                    note = NearestPitch((next.root + 7) % 12, note);
                else
                    note = target + (note < target ? -1 : 1);
            } else {
                int tone = span.chord.tones[1 + (beat + (int)(b.rng.Next() % 2u)) % (span.chord.count - 1)];
                note = NearestPitch((span.chord.root + tone) % 12, note + (note < 40 ? 4 : -4));
            }
            float velocity = beat == 0 ? 1.0f : 0.85f;
            AddNote(b, BASS, BASS_CHANNEL, note, span.start + (float)beat * 2.0f, 2.0f, 0.95f, velocity);
        }
        previous = note;
    }
}

// Snare roll over beats 3 and 4 of a bar (sextuplets), getting louder.
static void AddSnareRoll(TrackBuilder& b, uint32_t barTick, Drum drum)
{
    for (int i = 0; i < 12; ++i)
        AddDrum(b, barTick + 48 + (uint32_t)i * 4, drum, 0.3f + 0.6f * (float)i / 11.0f);
}

// Root of the chord at `eighth` as a timpani pitch (F2 .. E3).
static float TimpaniPitch(TrackBuilder& b, float eighth, int interval)
{
    int pc = (ChordAt(b.chords, eighth).root + interval) % 12;
    return (float)(41 + ((pc - 41) % 12 + 12) % 12);
}

static void CompileDrums(TrackBuilder& b)
{
    DrumPattern pattern = b.def->drums;
    int swing = b.def->swing;
    for (int bar = 0; bar < b.bars; ++bar) {
        uint32_t barTick = (uint32_t)bar * 4 * TICKS_PER_BEAT;
        int phraseBar = (bar - b.def->introBars + 64) % 8;
        bool fillBar = phraseBar == 7 || (b.def->introBars > 0 && bar == b.def->introBars - 1);
        bool phraseStart = phraseBar == 0 && bar >= b.def->introBars;
        float barEighth = (float)bar * 8.0f;
        uint32_t offBeat = (uint32_t)swing;
        if (pattern == DrumPattern::SWING || pattern == DrumPattern::JUNGLE) {
            constexpr float RIDE_GAINS[6] = {0.7f, 1.0f, 0.55f, 0.7f, 1.0f, 0.55f};
            constexpr uint32_t RIDE_BEATS[6] = {0, 1, 1, 2, 3, 3};
            for (int r = 0; r < 6; ++r) {
                uint32_t tick = barTick + RIDE_BEATS[r] * TICKS_PER_BEAT + (r == 2 || r == 5 ? offBeat : 0);
                AddDrum(b, tick, RIDE, RIDE_GAINS[r] * (pattern == DrumPattern::JUNGLE ? 0.8f : 1.0f), r & 1);
            }
            AddDrum(b, barTick + TICKS_PER_BEAT, HAT, 1.0f);
            AddDrum(b, barTick + 3 * TICKS_PER_BEAT, HAT, 1.0f);
        }
        if (pattern == DrumPattern::SWING) {
            for (uint32_t beat = 0; beat < 4; ++beat) {
                if (fillBar && beat == 3)
                    continue;
                AddDrum(b, barTick + beat * TICKS_PER_BEAT, KICK, beat == 0 ? 0.35f : 0.22f);
            }
            if (!fillBar && b.rng.Float() < 0.5f)
                AddDrum(b, barTick + 2 * TICKS_PER_BEAT + offBeat, BRUSH, 0.45f);
            if (!fillBar)
                AddDrum(b, barTick + 3 * TICKS_PER_BEAT, BRUSH, 0.6f);
            if (fillBar) {
                AddSnareRoll(b, barTick, BRUSH);
                AddDrum(b, barTick + 3 * TICKS_PER_BEAT + offBeat, KICK, 0.8f);
            }
        } else if (pattern == DrumPattern::JUNGLE) {
            constexpr float TOM_GAINS[4] = {1.0f, 0.7f, 0.9f, 0.7f};
            for (uint32_t beat = 0; beat < 4; ++beat) {
                if (fillBar && beat >= 2)
                    continue;
                AddDrum(b, barTick + beat * TICKS_PER_BEAT, TOM_LOW, TOM_GAINS[beat]);
                if (beat == 1 || beat == 3)
                    AddDrum(b, barTick + beat * TICKS_PER_BEAT + offBeat, TOM_HIGH, 0.55f);
            }
            AddDrum(b, barTick, KICK, 0.55f);
            AddDrum(b, barTick + 2 * TICKS_PER_BEAT, KICK, 0.45f);
            if (phraseBar == 3)
                AddDrum(b, barTick + 3 * TICKS_PER_BEAT + offBeat, SNARE, 0.7f);
            if (fillBar)
                AddSnareRoll(b, barTick, SNARE);
        } else if (pattern == DrumPattern::BOSS) {
            for (uint32_t beat = 0; beat < 4; ++beat)
                AddDrum(b, barTick + beat * TICKS_PER_BEAT, RIDE, 0.6f, (int)(beat & 1));
            AddDrum(b, barTick, KICK, 1.0f);
            AddDrum(b, barTick + 2 * TICKS_PER_BEAT, KICK, 0.85f);
            AddDrum(b, barTick + TICKS_PER_BEAT, SNARE, 0.75f);
            if (!fillBar)
                AddDrum(b, barTick + 3 * TICKS_PER_BEAT, SNARE, 0.85f);
            AddDrum(b, barTick, TIMPANI, 1.0f, 0, TimpaniPitch(b, barEighth, 0));
            AddDrum(b, barTick + TICKS_PER_BEAT + offBeat, TIMPANI, 0.6f, 0, TimpaniPitch(b, barEighth, 7));
            if (phraseBar == 3) {
                constexpr Drum FILL[4] = {TOM_HIGH, TOM_HIGH, TOM_LOW, TOM_LOW};
                for (uint32_t i = 0; i < 4; ++i)
                    AddDrum(b, barTick + 3 * TICKS_PER_BEAT + i * 6, FILL[i], 0.7f + 0.1f * (float)i);
            }
            if (fillBar) {
                for (int i = 0; i < 16; ++i)
                    AddDrum(b, barTick + 48 + (uint32_t)i * 3, TIMPANI, 0.25f + 0.5f * (float)i / 15.0f, 0,
                            TimpaniPitch(b, barEighth, 0));
            }
        } else if (pattern == DrumPattern::FANFARE) {
            if (bar == 0) {
                AddDrum(b, barTick, KICK, 0.9f);
                AddDrum(b, barTick, CRASH, 0.7f);
                for (uint32_t i = 0; i < 3; ++i)
                    AddDrum(b, barTick + i * 8, SNARE, 0.6f);
                AddDrum(b, barTick + 2 * TICKS_PER_BEAT, KICK, 0.7f);
            } else if (bar == 1) {
                for (int i = 0; i < 24; ++i)
                    AddDrum(b, barTick + (uint32_t)i * 4, SNARE, 0.25f + 0.6f * (float)i / 23.0f);
            } else {
                AddDrum(b, barTick, CRASH, 1.0f);
                AddDrum(b, barTick, KICK, 1.0f);
                AddDrum(b, barTick, TIMPANI, 1.0f, 0, 41.0f);
                AddDrum(b, barTick + 2 * TICKS_PER_BEAT, TIMPANI, 0.6f, 0, 41.0f);
            }
        }
        if (phraseStart && pattern != DrumPattern::NONE && pattern != DrumPattern::FANFARE)
            AddDrum(b, barTick, CRASH, pattern == DrumPattern::SWING ? 0.6f : 0.85f);
    }
}

static void CompileTrack(const TrackDef& def, Song& song, uint32_t seed)
{
    TrackBuilder b{&def, &song, {}, 0, 0.0f, Rng(seed)};
    b.bars = ParseChords(def.chords, b.chords, def.name);
    song.bpm = def.bpm;
    song.loops = def.loops;
    for (int p = 0; p < def.partCount; ++p)
        CompilePart(b, def.parts[p], PART_CHANNEL + p);
    if (def.stabs)
        CompileStabs(b);
    CompileComp(b);
    CompileWalkingBass(b);
    CompileDrums(b);
    std::stable_sort(song.events.begin(), song.events.end(), [](const NoteEvent& a, const NoteEvent& c) {
        return a.tick < c.tick;
    });
    song.lengthTicks = (uint32_t)(b.bars * 4 * TICKS_PER_BEAT);
    song.loopTick = (uint32_t)(def.introBars * 4 * TICKS_PER_BEAT);
    song.loopEvent = 0;
    while (song.loopEvent < song.events.size() && song.events[song.loopEvent].tick < song.loopTick)
        ++song.loopEvent;
}

// DEFEAT is the sad trombone sample alone.
static void CompileDefeat(Song& song)
{
    song.bpm = 120.0f;
    song.loops = false;
    NoteEvent e;
    e.tick = 0;
    e.lengthTicks = 0;
    e.sample = &sadTrombone;
    e.step = (uint32_t)((uint64_t)sadTrombone.rate * 65536u / MIX_RATE);
    e.gainL = 20000;
    e.gainR = 20000;
    e.releaseFrames = ReleaseFrames(0.05f);
    e.channel = 0;
    song.events.push_back(e);
    float seconds = (float)sadTrombone.frames / (float)sadTrombone.rate + 0.5f;
    song.lengthTicks = (uint32_t)(seconds * song.bpm / 60.0f * (float)TICKS_PER_BEAT);
}

// Which parts are built: the title set, then the rest; and which tracks the mixer may play (set last, after each
// track's notes and samples are complete).
static bool titleBuilt = false;
static bool restBuilt = false;
static std::atomic<bool> songReady[(int)Music::DEFEAT + 1];

void BuildTitleMusic()
{
    if (titleBuilt)
        return;
    titleBuilt = true;
    dataErrors[0] = 0;
    for (int i = 0; i < INSTRUMENT_COUNT; ++i) {
        if (i != TROMBONE)
            BuildInstrument((Instrument)i);
    }
    BuildDrums();
    CompileTrack(TITLE_TRACK, songs[(int)Music::TITLE], 1);
    songReady[(int)Music::TITLE].store(true, std::memory_order_release);
}

void BuildMusic()
{
    BuildTitleMusic();
    if (restBuilt)
        return;
    restBuilt = true;
    BuildInstrument(TROMBONE);
    BuildSadTrombone();
    CompileTrack(COMBAT_TRACK, songs[(int)Music::COMBAT], 2);
    songReady[(int)Music::COMBAT].store(true, std::memory_order_release);
    CompileTrack(BOSS_TRACK, songs[(int)Music::BOSS], 3);
    songReady[(int)Music::BOSS].store(true, std::memory_order_release);
    CompileTrack(VICTORY_TRACK, songs[(int)Music::VICTORY], 4);
    songReady[(int)Music::VICTORY].store(true, std::memory_order_release);
    CompileDefeat(songs[(int)Music::DEFEAT]);
    songReady[(int)Music::DEFEAT].store(true, std::memory_order_release);
}

bool SongReady(Music music)
{
    return music == Music::NONE || songReady[(int)music].load(std::memory_order_acquire);
}

const Song& GetSong(Music music)
{
    return songs[(int)music];
}

uint32_t MusicBankBytes()
{
    uint32_t bytes = 0;
    for (const MultiSample& multi : instruments) {
        for (const Sample& sample : multi.samples)
            bytes += SampleBytes(sample);
    }
    for (const Sample& sample : drums)
        bytes += SampleBytes(sample);
    for (int i = 0; i < banjoCount; ++i)
        bytes += SampleBytes(banjoChords[i]);
    return bytes + SampleBytes(sadTrombone);
}

const char* MusicDataErrors()
{
    return dataErrors;
}

} // namespace audio
