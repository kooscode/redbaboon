// Synthesis building blocks for the sound bank: random numbers, oscillators, filters, envelopes, loop making and the
// Sample type the mixer plays (docs/design.md §6). Everything here runs once at start-up, never in the mixer.
#pragma once

#include <stdint.h>
#include <vector>

namespace audio {

// --- Rates and samples

// The output rate of the mixer and the device.
constexpr int MIX_RATE = 48000;

// The rate sound effects and engine loops are rendered at (16 kHz of bandwidth is plenty, and it saves memory).
constexpr int SFX_RATE = 32000;

// The rate music instruments are rendered at.
constexpr int MUSIC_RATE = 22050;

// A mono 16-bit sound. data holds frames + 1 guard frame, so the mixer's interpolation may read one past the last
// frame: the guard repeats data[loopStart] for a looping sample, else it is 0.
struct Sample {
    std::vector<int16_t> data;
    uint32_t frames = 0;
    uint32_t loopStart = 0;
    uint32_t loopEnd = 0;     // 0: plays once
    uint32_t rate = SFX_RATE;
};

// Converts a float buffer to a Sample without DC, scaled so that its peak is `peak` (0..1 of full scale).
Sample MakeSample(const std::vector<float>& buffer, uint32_t rate, float peak, uint32_t loopStart = 0,
                  uint32_t loopEnd = 0);

// Bytes held by a sample.
uint32_t SampleBytes(const Sample& sample);

// --- Random numbers

// xorshift32: fast, deterministic per seed.
class Rng {
public:
    constexpr explicit Rng(uint32_t seed) : state(seed ? seed : 0x9E3779B9u) {}

    uint32_t Next();
    float Float();                       // [0, 1)
    float Bipolar();                     // [-1, 1)
    float Range(float lo, float hi);

private:
    uint32_t state;
};

// --- Oscillators

// sin(2 pi phase) from a table; phase in cycles (any value).
float Sine(float phase);

// Equal-tempered frequency of a MIDI note (69 = A4 = 440 Hz).
float NoteHz(float midi);

// Band-limited sawtooth (PolyBLEP). Next() returns -1..1 and advances by `increment` cycles per sample.
struct SawOsc {
    float phase = 0.0f;

    float Next(float increment);
};

// Band-limited square / pulse wave (PolyBLEP), duty 0..1.
struct PulseOsc {
    float phase = 0.0f;

    float Next(float increment, float duty);
};

// --- Filters

// One-pole lowpass (and the matching highpass as x - lowpass).
struct OnePole {
    float coef = 1.0f;
    float y = 0.0f;

    void SetCutoff(float hz, float rate);
    float Lowpass(float x);
    float Highpass(float x);
};

// Biquad (RBJ cookbook), direct form 1.
struct Biquad {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float x1 = 0.0f;
    float x2 = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;

    void SetLowpass(float hz, float q, float rate);
    void SetHighpass(float hz, float q, float rate);
    void SetBandpass(float hz, float q, float rate);    // 0 dB peak gain
    float Process(float x);
};

// State-variable filter (topology preserving), for cutoffs that move over time.
struct Svf {
    float g = 0.0f;
    float k = 1.0f;
    float a1 = 1.0f;
    float ic1 = 0.0f;
    float ic2 = 0.0f;
    float low = 0.0f;
    float band = 0.0f;
    float high = 0.0f;

    void Set(float hz, float q, float rate);
    void Process(float x);
};

// --- Shapes and helpers

// Nearest integer (nxdk's lrintf is not implemented).
int RoundToInt(float x);

// Soft saturation, -1..1 out for any input, ~linear near 0.
float SoftClip(float x);

// sin and cos of x radians to about 1e-6 (range reduction and a short polynomial), and a square root with SSE: the
// synthesis must not call nxdk's x87 libm (it builds with -fno-builtin, so sinf, expf and friends are slow calls).
float FastSin(float x);
float FastCos(float x);

// floor(x) without the C library (nxdk's floorf switches the x87 rounding mode on every call).
float Floor(float x);

// exp(x) to about 1e-6 relative error (1e-9 near 0, where per-sample decay factors are taken), without libm.
float FastExp(float x);

// exp(-t / tau).
float Decay(float t, float tau);

// Linear attack over `attack` seconds then exponential decay with time constant `tau`.
float AttackDecay(float t, float attack, float tau);

// AttackDecay computed incrementally (no exp per sample): call Next() once per frame.
class EnvelopeAD {
public:
    EnvelopeAD(float attack, float tau, float rate);

    float Next();

private:
    int attackFrames;
    int frame = 0;
    float level = 1.0f;
    float fall;
};

// Adds `src` into `dst` at frame `offset` with gain `gain`, growing dst if needed.
void MixInto(std::vector<float>& dst, const std::vector<float>& src, int offset, float gain);

// Short linear fades at both ends (removes clicks at sample start and end).
void FadeEdges(std::vector<float>& buffer, int fadeIn, int fadeOut);

// Removes DC with a 20 Hz highpass.
void RemoveDc(std::vector<float>& buffer, float rate);

// Makes [loopStart, loopEnd) loop without a click: the last `crossfade` frames before loopEnd are blended with the
// frames before loopStart, so frame loopEnd-1 runs on into loopStart. Needs loopStart >= crossfade. The buffer is
// cut at loopEnd.
void MakeLoop(std::vector<float>& buffer, int loopStart, int loopEnd, int crossfade);

// --- Common sound layers

// A noise burst through a bandpass at `hz`, shaped by attack and exponential decay.
std::vector<float> NoiseBurst(float seconds, float hz, float q, float attack, float tau, uint32_t seed,
                              float rate = (float)SFX_RATE);

// A sine gliding exponentially from f0 to f1 over `glide` seconds, shaped by attack and exponential decay.
std::vector<float> Thump(float seconds, float f0, float f1, float glide, float attack, float tau,
                         float rate = (float)SFX_RATE);

// A struck object: `count` decaying partials at f0 * ratios[i] with gains[i] and decay times taus[i] (modal
// synthesis).
std::vector<float> Modes(float seconds, float f0, const float* ratios, const float* gains, const float* taus, int count,
                         float rate = (float)SFX_RATE);

// Karplus-Strong plucked string: `seconds` long at `hz`; brightness 0..1 sets the excitation and the damping.
std::vector<float> Pluck(float hz, float seconds, float brightness, float rate, uint32_t seed);

} // namespace audio
