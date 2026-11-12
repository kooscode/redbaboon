// Synthesis building blocks: oscillators, filters, envelopes and sample conversion (see synth.h).
#include "audio/synth.h"

#include <math.h>
#include <string.h>
#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace audio {

// --- Rates and samples

// Removes DC from a buffer about to become a sample: a looping sample loses the mean of its loop (a constant, so
// the loop stays seamless), a one-shot goes through a 20 Hz highpass and ends with a short fade.
static std::vector<float> WithoutDc(const std::vector<float>& buffer, uint32_t rate, uint32_t loopStart,
                                    uint32_t loopEnd)
{
    std::vector<float> out = buffer;
    if (loopEnd > loopStart) {
        double sum = 0.0;
        for (uint32_t i = loopStart; i < loopEnd; ++i)
            sum += out[i];
        float mean = (float)(sum / (double)(loopEnd - loopStart));
        for (float& v : out)
            v -= mean;
        return out;
    }
    RemoveDc(out, (float)rate);
    FadeEdges(out, 0, 32);
    return out;
}

Sample MakeSample(const std::vector<float>& input, uint32_t rate, float peak, uint32_t loopStart, uint32_t loopEnd)
{
    Sample sample;
    std::vector<float> buffer = WithoutDc(input, rate, loopStart, loopEnd);
    float maxAbs = 0.0f;
    for (float v : buffer) {
        float a = fabsf(v);
        if (a > maxAbs)
            maxAbs = a;
    }
    float scale = maxAbs > 1e-9f ? peak * 32767.0f / maxAbs : 0.0f;
    sample.frames = (uint32_t)buffer.size();
    sample.data.resize(sample.frames + 1);
    for (uint32_t i = 0; i < sample.frames; ++i) {
        float v = buffer[i] * scale;
        if (v > 32767.0f)
            v = 32767.0f;
        if (v < -32767.0f)
            v = -32767.0f;
        sample.data[i] = (int16_t)RoundToInt(v);
    }
    sample.loopStart = loopStart;
    sample.loopEnd = loopEnd;
    sample.data[sample.frames] = loopEnd > 0 ? sample.data[loopStart] : 0;
    sample.rate = rate;
    return sample;
}

uint32_t SampleBytes(const Sample& sample)
{
    return (uint32_t)(sample.data.size() * sizeof(int16_t));
}

// --- Random numbers

uint32_t Rng::Next()
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

float Rng::Float()
{
    return (float)(Next() >> 8) * (1.0f / 16777216.0f);
}

float Rng::Bipolar()
{
    return Float() * 2.0f - 1.0f;
}

float Rng::Range(float lo, float hi)
{
    return lo + (hi - lo) * Float();
}

// --- Oscillators

// Size of the sine table (one cycle, plus a guard entry for interpolation).
constexpr int SINE_TABLE_SIZE = 4096;

// The sine table, filled on first use.
static float sineTable[SINE_TABLE_SIZE + 1];
static bool sineTableReady = false;

static void BuildSineTable()
{
    for (int i = 0; i <= SINE_TABLE_SIZE; ++i)
        sineTable[i] = sinf(2.0f * 3.14159265358979f * (float)i / (float)SINE_TABLE_SIZE);
    sineTableReady = true;
}

float Sine(float phase)
{
    if (!sineTableReady)
        BuildSineTable();
    float p = phase - Floor(phase);
    float x = p * (float)SINE_TABLE_SIZE;
    int i = (int)x;
    float f = x - (float)i;
    return sineTable[i] + (sineTable[i + 1] - sineTable[i]) * f;
}

float NoteHz(float midi)
{
    return 440.0f * FastExp((midi - 69.0f) * (0.69314718f / 12.0f));
}

// PolyBLEP correction for a step at phase 0 (t in cycles, dt = increment).
static float PolyBlep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

float SawOsc::Next(float increment)
{
    float v = 2.0f * phase - 1.0f - PolyBlep(phase, increment);
    phase += increment;
    phase -= Floor(phase);
    return v;
}

float PulseOsc::Next(float increment, float duty)
{
    float v = phase < duty ? 1.0f : -1.0f;
    v += PolyBlep(phase, increment);
    float shifted = phase + 1.0f - duty;
    v -= PolyBlep(shifted - Floor(shifted), increment);
    phase += increment;
    phase -= Floor(phase);
    return v;
}

// --- Filters

void OnePole::SetCutoff(float hz, float rate)
{
    coef = 1.0f - FastExp(-2.0f * 3.14159265358979f * hz / rate);
}

float OnePole::Lowpass(float x)
{
    y += (x - y) * coef;
    return y;
}

float OnePole::Highpass(float x)
{
    y += (x - y) * coef;
    return x - y;
}

void Biquad::SetLowpass(float hz, float q, float rate)
{
    float w = 2.0f * 3.14159265358979f * hz / rate;
    float c = FastCos(w);
    float alpha = FastSin(w) / (2.0f * q);
    float a0 = 1.0f + alpha;
    b0 = (1.0f - c) * 0.5f / a0;
    b1 = (1.0f - c) / a0;
    b2 = b0;
    a1 = -2.0f * c / a0;
    a2 = (1.0f - alpha) / a0;
}

void Biquad::SetHighpass(float hz, float q, float rate)
{
    float w = 2.0f * 3.14159265358979f * hz / rate;
    float c = FastCos(w);
    float alpha = FastSin(w) / (2.0f * q);
    float a0 = 1.0f + alpha;
    b0 = (1.0f + c) * 0.5f / a0;
    b1 = -(1.0f + c) / a0;
    b2 = b0;
    a1 = -2.0f * c / a0;
    a2 = (1.0f - alpha) / a0;
}

void Biquad::SetBandpass(float hz, float q, float rate)
{
    float w = 2.0f * 3.14159265358979f * hz / rate;
    float c = FastCos(w);
    float alpha = FastSin(w) / (2.0f * q);
    float a0 = 1.0f + alpha;
    b0 = alpha / a0;
    b1 = 0.0f;
    b2 = -alpha / a0;
    a1 = -2.0f * c / a0;
    a2 = (1.0f - alpha) / a0;
}

float Biquad::Process(float x)
{
    float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
}

void Svf::Set(float hz, float q, float rate)
{
    if (hz > rate * 0.45f)
        hz = rate * 0.45f;
    float w = 3.14159265358979f * hz / rate;
    g = FastSin(w) / FastCos(w);
    k = 1.0f / q;
    a1 = 1.0f / (1.0f + g * (g + k));
}

void Svf::Process(float x)
{
    float v3 = x - ic2;
    float v1 = a1 * ic1 + g * a1 * v3;
    float v2 = ic2 + g * v1;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
    low = v2;
    band = v1;
    high = x - k * v1 - v2;
}

// --- Shapes and helpers

int RoundToInt(float x)
{
    return (int)(x < 0.0f ? x - 0.5f : x + 0.5f);
}

float SoftClip(float x)
{
    if (x > 3.0f)
        return 1.0f;
    if (x < -3.0f)
        return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

float FastSin(float x)
{
    constexpr float PI_F = 3.14159265358979f;
    x -= 2.0f * PI_F * Floor(x * 0.159154943f + 0.5f);
    if (x > 0.5f * PI_F)
        x = PI_F - x;
    else if (x < -0.5f * PI_F)
        x = -PI_F - x;
    float x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f + x2 * (0.0083333310f + x2 * (-0.00019840874f + x2 * 2.7525562e-6f))));
}

float FastCos(float x)
{
    return FastSin(x + 0.5f * 3.14159265358979f);
}

// Square root in one SSE instruction (sqrtf is an x87 library call with -fno-builtin).
static float FastSqrt(float x)
{
#if defined(__SSE__) || defined(__x86_64__)
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)));
#else
    return sqrtf(x);
#endif
}

float Floor(float x)
{
    int i = (int)x;
    return (float)(x < (float)i ? i - 1 : i);
}

float FastExp(float x)
{
    if (x < -80.0f)
        return 0.0f;
    if (x > 80.0f)
        x = 80.0f;

    // Near 0 (per-sample decay factors, compounded over 10^5 samples) the Taylor series is exact to 1e-9.
    if (x > -0.01f && x < 0.01f)
        return 1.0f + x * (1.0f + x * (0.5f + x * (1.0f / 6.0f)));

    // 2^y = 2^whole * 2^f with f in [-0.5, 0.5]: the series of 2^f to the 7th power is good to 2e-7.
    float y = x * 1.44269504f;
    float whole = Floor(y + 0.5f);
    float f = y - whole;
    float high = 0.00961813f + f * (0.00133336f + f * (0.00015404f + f * 1.525e-5f));
    float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * high)));
    uint32_t bits;
    memcpy(&bits, &p, sizeof(bits));
    bits += (uint32_t)((int)whole) << 23;
    memcpy(&p, &bits, sizeof(p));
    return p;
}

float Decay(float t, float tau)
{
    return FastExp(-t / tau);
}

float AttackDecay(float t, float attack, float tau)
{
    if (t < 0.0f)
        return 0.0f;
    if (t < attack)
        return t / attack;
    return FastExp(-(t - attack) / tau);
}

EnvelopeAD::EnvelopeAD(float attack, float tau, float rate)
    : attackFrames((int)(attack * rate)), fall(FastExp(-1.0f / (tau * rate)))
{
}

float EnvelopeAD::Next()
{
    if (frame < attackFrames) {
        ++frame;
        return (float)frame / (float)attackFrames;
    }
    level *= fall;
    return level;
}

void MixInto(std::vector<float>& dst, const std::vector<float>& src, int offset, float gain)
{
    if (offset < 0)
        offset = 0;
    if (dst.size() < src.size() + (size_t)offset)
        dst.resize(src.size() + (size_t)offset, 0.0f);
    for (size_t i = 0; i < src.size(); ++i)
        dst[i + (size_t)offset] += src[i] * gain;
}

void FadeEdges(std::vector<float>& buffer, int fadeIn, int fadeOut)
{
    int n = (int)buffer.size();
    for (int i = 0; i < fadeIn && i < n; ++i)
        buffer[i] *= (float)i / (float)fadeIn;
    for (int i = 0; i < fadeOut && i < n; ++i)
        buffer[n - 1 - i] *= (float)i / (float)fadeOut;
}

void RemoveDc(std::vector<float>& buffer, float rate)
{
    OnePole dc;
    dc.SetCutoff(20.0f, rate);
    for (float& v : buffer)
        v = dc.Highpass(v);
}

void MakeLoop(std::vector<float>& buffer, int loopStart, int loopEnd, int crossfade)
{
    for (int i = 0; i < crossfade; ++i) {
        float w = (float)i / (float)crossfade;
        int dst = loopEnd - crossfade + i;
        int src = loopStart - crossfade + i;
        buffer[dst] = buffer[dst] * FastSqrt(1.0f - w) + buffer[src] * FastSqrt(w);
    }
    buffer.resize(loopEnd);
}

std::vector<float> Pluck(float hz, float seconds, float brightness, float rate, uint32_t seed)
{
    int frames = (int)(seconds * rate);
    std::vector<float> out(frames);
    float lpCoef = 0.25f + 0.7f * brightness;
    float period = rate / hz - (1.0f - lpCoef) / lpCoef;
    int size = (int)period + 4;
    std::vector<float> line(size, 0.0f);
    Rng rng(seed);

    // Excitation: one period of noise, lowpassed by the brightness.
    OnePole excite;
    excite.SetCutoff(800.0f + 9000.0f * brightness, rate);
    for (int i = 0; i < size; ++i)
        line[i] = excite.Lowpass(rng.Bipolar());

    float decay = powf(0.001f, 1.0f / (hz * seconds * 0.8f));
    float lp = 0.0f;
    int write = 0;
    for (int n = 0; n < frames; ++n) {
        float readPos = (float)write - period;
        while (readPos < 0.0f)
            readPos += (float)size;
        int i0 = (int)readPos;
        float f = readPos - (float)i0;
        int i1 = (i0 + 1) % size;
        float delayed = line[i0] + (line[i1] - line[i0]) * f;
        lp += (delayed - lp) * lpCoef;
        float v = n < size ? line[write] : lp * decay;
        if (n >= size)
            line[write] = v;
        out[n] = v;
        write = (write + 1) % size;
    }
    return out;
}

// --- Common sound layers

std::vector<float> NoiseBurst(float seconds, float hz, float q, float attack, float tau, uint32_t seed, float rate)
{
    std::vector<float> out((size_t)(seconds * rate));
    Rng rng(seed);
    Biquad bp;
    bp.SetBandpass(hz, q, rate);
    EnvelopeAD env(attack, tau, rate);
    for (float& v : out)
        v = bp.Process(rng.Bipolar()) * env.Next();
    return out;
}

std::vector<float> Thump(float seconds, float f0, float f1, float glide, float attack, float tau, float rate)
{
    std::vector<float> out((size_t)(seconds * rate));
    int glideFrames = (int)(glide * rate) + 1;
    float ratio = powf(f1 / f0, 1.0f / (float)glideFrames);
    float hz = f0;
    float phase = 0.0f;
    EnvelopeAD env(attack, tau, rate);
    for (size_t i = 0; i < out.size(); ++i) {
        if ((int)i < glideFrames)
            hz *= ratio;
        phase += hz / rate;
        out[i] = Sine(phase) * env.Next();
    }
    return out;
}

std::vector<float> Modes(float seconds, float f0, const float* ratios, const float* gains, const float* taus, int count,
                         float rate)
{
    std::vector<float> out((size_t)(seconds * rate), 0.0f);
    for (int m = 0; m < count; ++m) {
        float hz = f0 * ratios[m];
        if (hz > rate * 0.45f)
            continue;
        float increment = hz / rate;
        float phase = 0.0f;
        float level = gains[m];
        float fall = FastExp(-1.0f / (taus[m] * rate));
        for (float& v : out) {
            phase += increment;
            v += Sine(phase) * level;
            level *= fall;
        }
    }
    return out;
}

} // namespace audio
