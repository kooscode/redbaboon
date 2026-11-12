// The software mixer: plays effect voices, engine voices and the music sequencer into 48 kHz stereo S16, in the
// audio device's thread (docs/design.md §6). Per sample it only does integer interpolation, gain and a one-pole
// filter; everything costly (panning, Doppler, distance) is computed by the game thread or once per buffer.
#include "audio/mixer.h"

#include "audio/music.h"
#include "audio/sfx.h"

#include <atomic>
#include <math.h>

namespace audio {

// Largest block mixed at once (frames); longer requests are split.
constexpr int BLOCK = 1024;

// releaseIn of a voice that plays to its end.
constexpr uint32_t NO_RELEASE = 0xFFFFFFFFu;

// True for a usable float (not NaN or infinite). A single NaN in a gain or an envelope would silence the mix for
// good, so every float that comes from outside or carries over between buffers is checked.
static bool Finite(float x)
{
    return x == x && x - x == 0.0f;
}

// Count of non-finite values found and reset (diagnostics).
static uint32_t repairs = 0;

// Shortest fade-out of any voice (40 ms): a stolen, choked or stopped sound never clicks.
constexpr uint32_t RELEASE_MIN = MIX_RATE * 40 / 1000;

// Mix accumulators for the current block: effects + engines, and each music sequencer slot.
static int32_t mixL[BLOCK];
static int32_t mixR[BLOCK];
static int32_t musicL[2][BLOCK];
static int32_t musicR[2][BLOCK];

// --- Command queue (game thread -> mixer)

// Queue size: a power of two. At most this many effects can start per buffer (21 ms).
constexpr uint32_t QUEUE_SIZE = 64;

static PlayCommand queue[QUEUE_SIZE];
static std::atomic<uint32_t> queueHead{0};     // written by the game thread
static std::atomic<uint32_t> queueTail{0};     // written by the mixer

bool MixerPlay(const PlayCommand& command)
{
    uint32_t head = queueHead.load(std::memory_order_relaxed);
    uint32_t tail = queueTail.load(std::memory_order_acquire);
    if (head - tail >= QUEUE_SIZE)
        return false;
    queue[head % QUEUE_SIZE] = command;
    queueHead.store(head + 1, std::memory_order_release);
    return true;
}

// --- Voices

// A playing sample. Gains are 9.23 fixed point (1.0 = 1 << 23, the 0..32767 command gains shifted up by 8) so slow
// ramps keep their precision.
struct Voice {
    const Sample* sample = nullptr;    // nullptr: free
    uint32_t pos = 0;
    uint32_t frac = 0;                 // 0.16 fraction of pos
    uint32_t step = 0;
    int32_t gainL = 0;
    int32_t gainR = 0;
    int32_t targetL = 0;
    int32_t targetR = 0;
    int32_t stepL = 0;
    int32_t stepR = 0;
    uint32_t rampLeft = 0;
    uint32_t delay = 0;
    uint32_t releaseIn = NO_RELEASE;
    uint32_t releaseFrames = 64;
    bool releasing = false;
    int32_t lowpass = 16384;
    int32_t filtered = 0;
    float priority = 0.0f;
    uint32_t serial = 0;               // start order, for "oldest"
    uint8_t sfx = 0;
    int8_t slot = -1;                  // music sequencer slot, -1 for effects
    uint8_t channel = 0;
    bool hasNext = false;              // a stolen voice starts `next` when its fade ends
    PlayCommand next;
    int8_t nextSlot = -1;              // the slot and channel `next` belongs to (music)
    uint8_t nextChannel = 0;
};

static Voice sfxVoices[SFX_VOICES];
static Voice musicVoices[MUSIC_VOICES];

// Counts voice starts.
static uint32_t voiceSerial = 0;

// Glides a voice's gains to the targets over `frames`.
static void RampTo(Voice& v, int32_t left, int32_t right, uint32_t frames)
{
    v.targetL = left;
    v.targetR = right;
    if (frames == 0) {
        v.gainL = left;
        v.gainR = right;
        v.rampLeft = 0;
        v.stepL = 0;
        v.stepR = 0;
        return;
    }
    v.stepL = (left - v.gainL) / (int32_t)frames;
    v.stepR = (right - v.gainR) / (int32_t)frames;
    v.rampLeft = frames;
}

static void StartVoice(Voice& v, const PlayCommand& c, int8_t slot, uint8_t channel)
{
    v.sample = c.sample;
    v.pos = 0;
    v.frac = 0;
    v.step = c.step > 0 ? c.step : 1;
    v.delay = c.delayFrames;
    v.releaseIn = c.releaseIn;
    v.releaseFrames = c.releaseFrames > 0 ? c.releaseFrames : 1;
    v.releasing = false;
    v.lowpass = c.lowpass;
    v.filtered = 0;
    v.priority = c.priority;
    v.serial = ++voiceSerial;
    v.sfx = c.sfx;
    v.slot = slot;
    v.channel = channel;
    v.hasNext = false;
    RampTo(v, (int32_t)c.gainL << 8, (int32_t)c.gainR << 8, 0);
}

// The voice's sample has ended or its fade is done: free it, or start the sound that stole it.
static void FinishVoice(Voice& v)
{
    if (v.hasNext) {
        PlayCommand next = v.next;
        StartVoice(v, next, v.nextSlot, v.nextChannel);
        return;
    }
    v.sample = nullptr;
}

// Fades the voice out over `frames` (at least RELEASE_MIN: nothing is ever cut dead), then finishes it. A voice still
// waiting out its start delay has not sounded yet and finishes at once.
static void ReleaseVoice(Voice& v, uint32_t frames)
{
    if (v.delay > 0) {
        FinishVoice(v);
        return;
    }
    v.releasing = true;
    RampTo(v, 0, 0, frames > RELEASE_MIN ? frames : RELEASE_MIN);
}

// Output frames until the voice reaches source frame `end` (at least 1).
static uint32_t FramesUntil(const Voice& v, uint32_t end)
{
    uint64_t remaining = ((uint64_t)(end - v.pos) << 16) - v.frac;
    uint64_t frames = (remaining + v.step - 1) / v.step;
    return frames > 0 ? (uint32_t)frames : 1;
}

// Mixes n frames of a voice that stay inside its sample and its current ramp.
static void MixRun(Voice& v, int32_t* outL, int32_t* outR, uint32_t n)
{
    const int16_t* data = v.sample->data.data();
    uint32_t pos = v.pos;
    uint32_t frac = v.frac;
    uint32_t step = v.step;
    int32_t gL = v.gainL;
    int32_t gR = v.gainR;
    int32_t sL = v.stepL;
    int32_t sR = v.stepR;
    if (v.lowpass >= 16384) {
        for (uint32_t i = 0; i < n; ++i) {
            int32_t a = data[pos];
            int32_t s = a + (((data[pos + 1] - a) * (int32_t)(frac >> 2)) >> 14);
            outL[i] += (s * (gL >> 8)) >> 15;
            outR[i] += (s * (gR >> 8)) >> 15;
            gL += sL;
            gR += sR;
            frac += step;
            pos += frac >> 16;
            frac &= 0xFFFF;
        }
    } else {
        int32_t lp = v.filtered;
        int32_t coef = v.lowpass;
        for (uint32_t i = 0; i < n; ++i) {
            int32_t a = data[pos];
            int32_t s = a + (((data[pos + 1] - a) * (int32_t)(frac >> 2)) >> 14);
            lp += ((s - lp) * coef) >> 14;
            outL[i] += (lp * (gL >> 8)) >> 15;
            outR[i] += (lp * (gR >> 8)) >> 15;
            gL += sL;
            gR += sR;
            frac += step;
            pos += frac >> 16;
            frac &= 0xFFFF;
        }
        v.filtered = lp;
    }
    v.pos = pos;
    v.frac = frac;
    v.gainL = gL;
    v.gainR = gR;
}

// Mixes up to `frames` of a voice: start delay, note-off, ramps, loop wraps and the end of the sample.
static void MixVoice(Voice& v, int32_t* outL, int32_t* outR, uint32_t frames)
{
    uint32_t done = 0;
    while (done < frames && v.sample) {
        if (v.delay > 0) {
            uint32_t n = frames - done < v.delay ? frames - done : v.delay;
            v.delay -= n;
            done += n;
            continue;
        }
        if (!v.releasing && v.releaseIn == 0)
            ReleaseVoice(v, v.releaseFrames);
        uint32_t n = frames - done;
        if (v.rampLeft > 0 && v.rampLeft < n)
            n = v.rampLeft;
        if (!v.releasing && v.releaseIn < n)
            n = v.releaseIn;
        uint32_t end = v.sample->loopEnd > 0 ? v.sample->loopEnd : v.sample->frames;
        uint32_t untilEnd = FramesUntil(v, end);
        if (untilEnd < n)
            n = untilEnd;
        MixRun(v, outL + done, outR + done, n);
        done += n;
        if (!v.releasing && v.releaseIn != NO_RELEASE)
            v.releaseIn -= n;
        if (v.rampLeft > 0) {
            v.rampLeft -= n;
            if (v.rampLeft == 0) {
                v.gainL = v.targetL;
                v.gainR = v.targetR;
                v.stepL = 0;
                v.stepR = 0;
                if (v.releasing) {
                    FinishVoice(v);
                    continue;
                }
            }
        }
        if (v.pos >= end) {
            if (v.sample->loopEnd == 0) {
                FinishVoice(v);
                continue;
            }
            uint32_t length = v.sample->loopEnd - v.sample->loopStart;
            while (v.pos >= end)
                v.pos -= length;
        }
    }
}

// --- Effects

// Starts a queued effect: a free voice, else replaces the oldest instance of the same effect when it has too many,
// else steals the lowest-priority voice if the new sound matters more. Stolen voices fade out first.
static void StartEffect(const PlayCommand& c)
{
    int free = -1;
    int instances = 0;
    int oldestInstance = -1;
    int weakest = -1;
    for (int i = 0; i < SFX_VOICES; ++i) {
        Voice& v = sfxVoices[i];
        if (!v.sample) {
            if (free < 0)
                free = i;
            continue;
        }
        if (v.hasNext)
            continue;
        if (v.sfx == c.sfx && !v.releasing) {
            ++instances;
            if (oldestInstance < 0 || v.serial < sfxVoices[oldestInstance].serial)
                oldestInstance = i;
        }
        if (weakest < 0 || v.priority < sfxVoices[weakest].priority)
            weakest = i;
    }
    int target = -1;
    if (instances >= c.maxInstances && oldestInstance >= 0)
        target = oldestInstance;
    else if (free >= 0)
        target = free;
    else if (weakest >= 0 && sfxVoices[weakest].priority <= c.priority)
        target = weakest;
    if (target < 0)
        return;
    Voice& v = sfxVoices[target];
    if (!v.sample) {
        StartVoice(v, c, -1, 0);
        return;
    }
    v.next = c;
    v.hasNext = true;
    v.nextSlot = -1;
    v.nextChannel = 0;
    v.priority = c.priority;
    ReleaseVoice(v, RELEASE_MIN);
}

// A request from the game thread to fade every effect out (not the menu sounds): a counter that changes with each
// request, and the fade length.
static std::atomic<uint32_t> effectsFadeRequest{0};
static std::atomic<float> effectsFadeSeconds{0.6f};
static uint32_t effectsFadeDone = 0;

void MixerFadeOutEffects(float seconds)
{
    effectsFadeSeconds.store(seconds, std::memory_order_relaxed);
    effectsFadeRequest.fetch_add(1, std::memory_order_release);
}

// Fades out every playing effect but the menu sounds when the game asked for it.
static void UpdateEffectsFade()
{
    uint32_t request = effectsFadeRequest.load(std::memory_order_acquire);
    if (request == effectsFadeDone)
        return;
    effectsFadeDone = request;
    float seconds = effectsFadeSeconds.load(std::memory_order_relaxed);
    if (!Finite(seconds) || seconds < 0.0f || seconds > 10.0f)
        seconds = 0.6f;
    for (Voice& v : sfxVoices) {
        bool menu = v.sfx == (uint8_t)Sfx::MENU_MOVE || v.sfx == (uint8_t)Sfx::MENU_SELECT;
        if (v.sample && !menu && !v.releasing)
            ReleaseVoice(v, (uint32_t)(seconds * (float)MIX_RATE));
    }
}

static void DrainQueue()
{
    uint32_t tail = queueTail.load(std::memory_order_relaxed);
    uint32_t head = queueHead.load(std::memory_order_acquire);
    while (tail != head) {
        StartEffect(queue[tail % QUEUE_SIZE]);
        ++tail;
    }
    queueTail.store(tail, std::memory_order_release);
}

// --- Engines

// Parameters shared with the game thread: each field is written by MixerSetEngine and read once per buffer.
struct SharedEngine {
    std::atomic<int> type{0};
    std::atomic<float> gainL{0.0f};
    std::atomic<float> gainR{0.0f};
    std::atomic<float> rate{1.0f};
    std::atomic<float> throttleMix{0.0f};
    std::atomic<float> lowpass{1.0f};
    std::atomic<float> whineGain{0.0f};
    std::atomic<float> whineRate{1.0f};
    std::atomic<float> windGain{0.0f};
    std::atomic<float> windRate{1.0f};
};

static SharedEngine sharedEngines[MAX_ENGINES];

// A playing engine on the mixer side: loop positions and the current (gliding) parameters.
struct EngineVoice {
    int type = 0;
    uint32_t pos = 0;
    uint32_t frac = 0;
    uint32_t whinePos = 0;
    uint32_t whineFrac = 0;
    uint32_t windPos = 0;
    uint32_t windFrac = 0;
    int32_t filtered = 0;
    EngineParams now;
    bool started = false;
};

static EngineVoice engineVoices[MAX_ENGINES];

void MixerSetEngine(int slot, EngineType type, const EngineParams& params)
{
    if (slot < 0 || slot >= MAX_ENGINES)
        return;
    SharedEngine& e = sharedEngines[slot];
    e.type.store((int)type, std::memory_order_relaxed);
    e.gainL.store(params.gainL, std::memory_order_relaxed);
    e.gainR.store(params.gainR, std::memory_order_relaxed);
    e.rate.store(params.rate, std::memory_order_relaxed);
    e.throttleMix.store(params.throttleMix, std::memory_order_relaxed);
    e.lowpass.store(params.lowpass, std::memory_order_relaxed);
    e.whineGain.store(params.whineGain, std::memory_order_relaxed);
    e.whineRate.store(params.whineRate, std::memory_order_relaxed);
    e.windGain.store(params.windGain, std::memory_order_relaxed);
    e.windRate.store(params.windRate, std::memory_order_relaxed);
}

// Replaces non-finite or out-of-range engine parameters with silence.
static void Sanitize(EngineParams& p)
{
    bool ok = Finite(p.gainL) && Finite(p.gainR) && Finite(p.rate) && Finite(p.throttleMix) && Finite(p.lowpass) &&
              Finite(p.whineGain) && Finite(p.whineRate) && Finite(p.windGain) && Finite(p.windRate);
    if (ok && p.gainL <= 4.0f && p.gainR <= 4.0f && p.rate > 0.0f && p.rate < 8.0f && p.whineRate > 0.0f &&
        p.whineRate < 8.0f && p.windRate > 0.0f && p.windRate < 8.0f && p.lowpass >= 0.0f && p.lowpass <= 1.0f &&
        p.whineGain <= 4.0f && p.windGain <= 4.0f)
        return;
    p = EngineParams();
    ++repairs;
}

static EngineParams ReadEngine(const SharedEngine& e)
{
    EngineParams p;
    p.gainL = e.gainL.load(std::memory_order_relaxed);
    p.gainR = e.gainR.load(std::memory_order_relaxed);
    p.rate = e.rate.load(std::memory_order_relaxed);
    p.throttleMix = e.throttleMix.load(std::memory_order_relaxed);
    p.lowpass = e.lowpass.load(std::memory_order_relaxed);
    p.whineGain = e.whineGain.load(std::memory_order_relaxed);
    p.whineRate = e.whineRate.load(std::memory_order_relaxed);
    p.windGain = e.windGain.load(std::memory_order_relaxed);
    p.windRate = e.windRate.load(std::memory_order_relaxed);
    Sanitize(p);
    return p;
}

// Frames per parameter step inside a buffer.
constexpr int ENGINE_STEP = 32;

// Linear interpolation of the engine parameters at fraction k of the way from a to b.
static EngineParams LerpParams(const EngineParams& a, const EngineParams& b, float k)
{
    EngineParams p;
    p.gainL = a.gainL + (b.gainL - a.gainL) * k;
    p.gainR = a.gainR + (b.gainR - a.gainR) * k;
    p.rate = a.rate + (b.rate - a.rate) * k;
    p.throttleMix = a.throttleMix + (b.throttleMix - a.throttleMix) * k;
    p.lowpass = a.lowpass + (b.lowpass - a.lowpass) * k;
    p.whineGain = a.whineGain + (b.whineGain - a.whineGain) * k;
    p.whineRate = a.whineRate + (b.whineRate - a.whineRate) * k;
    p.windGain = a.windGain + (b.windGain - a.windGain) * k;
    p.windRate = a.windRate + (b.windRate - a.windRate) * k;
    return p;
}

// Interpolated sample of a looping sample at pos/frac.
static inline int32_t LoopRead(const int16_t* data, uint32_t pos, uint32_t frac)
{
    int32_t a = data[pos];
    return a + (((data[pos + 1] - a) * (int32_t)(frac >> 2)) >> 14);
}

// Advances a loop position by step and wraps it.
static inline void LoopAdvance(const Sample& s, uint32_t& pos, uint32_t& frac, uint32_t step)
{
    frac += step;
    pos += frac >> 16;
    frac &= 0xFFFF;
    while (pos >= s.loopEnd)
        pos -= s.loopEnd - s.loopStart;
}

// 16.16 step for a loop at a relative rate.
static uint32_t LoopStep(const Sample& s, float rate)
{
    return (uint32_t)(rate * (float)s.rate / (float)MIX_RATE * 65536.0f);
}

// Mixes one engine: the idle and full-throttle loops cross-faded, plus whine, through the distance lowpass, plus the
// fly-by wind; parameters glide from the last buffer's values to the new targets.
static void MixEngine(EngineVoice& e, const EngineParams& target, int frames)
{
    const Sample& low = EngineLoop((EngineType)e.type, EngineLayer::LOW);
    const Sample& high = EngineLoop((EngineType)e.type, EngineLayer::HIGH);
    const Sample& whine = WhineLoop();
    const Sample& wind = WindLoop();
    const int16_t* lowData = low.data.data();
    const int16_t* highData = high.data.data();
    const int16_t* whineData = whine.data.data();
    const int16_t* windData = wind.data.data();
    EngineParams start = e.now;
    int32_t lp = e.filtered;
    for (int base = 0; base < frames; base += ENGINE_STEP) {
        int n = frames - base < ENGINE_STEP ? frames - base : ENGINE_STEP;
        EngineParams p = LerpParams(start, target, (float)(base + n) / (float)frames);
        int32_t gL = (int32_t)(p.gainL * 16384.0f);
        int32_t gR = (int32_t)(p.gainR * 16384.0f);
        int32_t mix = (int32_t)(p.throttleMix * 16384.0f);
        int32_t coef = (int32_t)(p.lowpass * 16384.0f);
        int32_t whineGain = (int32_t)(p.whineGain * 16384.0f);
        int32_t windGain = (int32_t)(p.windGain * 16384.0f);
        uint32_t step = LoopStep(low, p.rate);
        uint32_t whineStep = LoopStep(whine, p.whineRate);
        uint32_t windStep = LoopStep(wind, p.windRate);
        for (int i = 0; i < n; ++i) {
            int32_t a = LoopRead(lowData, e.pos, e.frac);
            int32_t b = LoopRead(highData, e.pos, e.frac);
            int32_t x = a + (((b - a) * mix) >> 14);
            if (whineGain > 0) {
                x += (LoopRead(whineData, e.whinePos, e.whineFrac) * whineGain) >> 14;
                LoopAdvance(whine, e.whinePos, e.whineFrac, whineStep);
            }
            lp += ((x - lp) * coef) >> 14;
            int32_t out = lp;
            if (windGain > 0) {
                out += (LoopRead(windData, e.windPos, e.windFrac) * windGain) >> 14;
                LoopAdvance(wind, e.windPos, e.windFrac, windStep);
            }
            if (out > 65535)
                out = 65535;
            if (out < -65535)
                out = -65535;
            mixL[base + i] += (out * gL) >> 14;
            mixR[base + i] += (out * gR) >> 14;
            LoopAdvance(low, e.pos, e.frac, step);
        }
    }
    e.filtered = lp;
    e.now = target;
}

// Engines whose sound is below this gain are skipped.
constexpr float ENGINE_SILENT = 0.0005f;

// Seconds an engine takes at least to fade from full level to silence (a plane removed or the game paused).
constexpr float ENGINE_FADE = 0.6f;

// Moves `now` toward `target`, falling by at most `maxDrop`.
static float FallLimited(float now, float target, float maxDrop)
{
    return target < now - maxDrop ? now - maxDrop : target;
}

static void MixEngines(int frames)
{
    if (!EnginesReady())
        return;
    float maxDrop = (float)frames / (ENGINE_FADE * (float)MIX_RATE);
    for (int i = 0; i < MAX_ENGINES; ++i) {
        EngineVoice& e = engineVoices[i];
        const SharedEngine& shared = sharedEngines[i];
        EngineParams target = ReadEngine(shared);
        Sanitize(e.now);

        // An engine being silenced keeps its pitch and tone while it fades (the game may move it anywhere then).
        if (target.gainL + target.gainR <= 0.0f) {
            target.rate = e.now.rate;
            target.lowpass = e.now.lowpass;
            target.throttleMix = e.now.throttleMix;
            target.whineRate = e.now.whineRate;
            target.windRate = e.now.windRate;
        }
        target.gainL = FallLimited(e.now.gainL, target.gainL, maxDrop);
        target.gainR = FallLimited(e.now.gainR, target.gainR, maxDrop);
        target.whineGain = FallLimited(e.now.whineGain, target.whineGain, maxDrop);
        target.windGain = FallLimited(e.now.windGain, target.windGain, maxDrop);
        int type = shared.type.load(std::memory_order_relaxed);
        bool silentNow = e.now.gainL + e.now.gainR < ENGINE_SILENT;
        bool silentTarget = target.gainL + target.gainR < ENGINE_SILENT;
        if (silentNow && silentTarget) {
            e.now = target;
            e.now.gainL = 0.0f;
            e.now.gainR = 0.0f;
            continue;
        }

        // A slot reused for another engine type fades out before it switches.
        if (type != e.type) {
            if (!silentNow) {
                target.gainL = FallLimited(e.now.gainL, 0.0f, maxDrop);
                target.gainR = FallLimited(e.now.gainR, 0.0f, maxDrop);
            } else {
                e.type = type;
                e.started = false;
            }
        }

        // An engine coming in starts at its target pitch, at a random point of the loop.
        if (silentNow) {
            float gainL = e.now.gainL;
            float gainR = e.now.gainR;
            e.now = target;
            e.now.gainL = gainL;
            e.now.gainR = gainR;
            if (!e.started) {
                const Sample& low = EngineLoop((EngineType)e.type, EngineLayer::LOW);
                e.pos = low.loopStart + (uint32_t)i * 7919u % (low.loopEnd - low.loopStart);
                e.whinePos = WhineLoop().loopStart;
                e.windPos = WindLoop().loopStart + (uint32_t)i * 4513u % (WindLoop().loopEnd - WindLoop().loopStart);
                e.started = true;
            }
        }
        MixEngine(e, target, frames);
    }
}

// --- Music sequencer

// One playing track. Two slots let a new track start while the old one fades out.
struct SequencerSlot {
    const Song* song = nullptr;
    Music music = Music::NONE;
    bool running = false;              // still reading events
    uint32_t tick = 0;
    uint32_t eventIndex = 0;
    uint32_t framesToTick = 0;
    uint32_t tickCarry = 0;            // 0.16 fraction of a frame carried between ticks
    uint32_t framesPerTick = 0;        // 16.16
    float gain = 0.0f;
    float fadeStep = 0.0f;             // gain change per frame
    int channelVoice[MUSIC_CHANNELS];
};

static SequencerSlot slots[2];

// The track the game asked for, the one to start when the current one ends (-1: none), the last request the mixer
// acted on, and the track it is playing.
static std::atomic<int> requestedMusic{0};
static std::atomic<int> nextMusic{-1};
static Music lastRequest = Music::NONE;
static Music currentMusic = Music::NONE;

// Music and master levels from the game thread, and the levels the mixer is gliding through.
static std::atomic<float> musicGainTarget{0.8f};
static std::atomic<float> masterTarget{1.0f};
static float musicGain = 0.8f;
static float masterGain = 1.0f;

// Fade times (seconds) when switching tracks: normal, under a fanfare or sting, and of a fanfare or sting itself.
constexpr float FADE_OUT = 0.8f;
constexpr float FADE_OUT_FAST = 0.25f;
constexpr float FADE_OUT_ONE_SHOT = 1.5f;

// Fade-in times (seconds) of a new track: on a switch, and after a fanfare (under the results).
constexpr float FADE_IN = 0.3f;
constexpr float FADE_IN_NEXT = 0.8f;

void MixerSetMusic(Music music)
{
    nextMusic.store(-1, std::memory_order_release);
    requestedMusic.store((int)music, std::memory_order_release);
}

void MixerSetNextMusic(Music music)
{
    nextMusic.store((int)music, std::memory_order_release);
}

void MixerSetMusicGain(float gain)
{
    musicGainTarget.store(gain, std::memory_order_relaxed);
}

void MixerSetMasterVolume(float volume)
{
    masterTarget.store(volume, std::memory_order_relaxed);
}

// Releases every voice of a slot over `frames`.
static void ReleaseSlotVoices(int slot, uint32_t frames)
{
    for (Voice& v : musicVoices) {
        if (v.sample && v.slot == slot && !v.releasing)
            ReleaseVoice(v, frames);
    }
}

static bool SlotSounding(int slot)
{
    if (slots[slot].running)
        return true;
    for (const Voice& v : musicVoices) {
        if (v.sample && v.slot == slot)
            return true;
    }
    return false;
}

// Starts a track in a slot, fading it in over `fadeIn` seconds (0: at full level at once).
static void StartSlot(int slot, Music music, float fadeIn)
{
    SequencerSlot& s = slots[slot];
    ReleaseSlotVoices(slot, RELEASE_MIN);
    s.song = &GetSong(music);
    s.music = music;
    s.running = !s.song->events.empty();
    s.tick = 0;
    s.eventIndex = 0;
    s.framesToTick = 0;
    s.tickCarry = 0;
    s.framesPerTick = (uint32_t)((float)MIX_RATE * 60.0f / (s.song->bpm * (float)TICKS_PER_BEAT) * 65536.0f);
    s.gain = fadeIn > 0.0f ? 0.0f : 1.0f;
    s.fadeStep = fadeIn > 0.0f ? 1.0f / (fadeIn * (float)MIX_RATE) : 0.0f;
    for (int& c : s.channelVoice)
        c = -1;
}

static bool OneShot(Music music)
{
    return music == Music::VICTORY || music == Music::DEFEAT;
}

// Switches to another track: the sounding slots fade out, a free one starts it, fading it in over `fadeIn` seconds
// (a fanfare or sting always starts at full level).
static void SwitchTo(Music wanted, float fadeIn)
{
    if (wanted == currentMusic)
        return;
    currentMusic = wanted;
    int fresh = -1;
    for (int i = 0; i < 2; ++i) {
        SequencerSlot& s = slots[i];
        if (s.fadeStep < 0.0f || !SlotSounding(i)) {
            if (fresh < 0)
                fresh = i;
            continue;
        }
        float fade = OneShot(s.music) ? FADE_OUT_ONE_SHOT : (OneShot(wanted) ? FADE_OUT_FAST : FADE_OUT);
        s.fadeStep = -1.0f / (fade * (float)MIX_RATE);
    }

    // Both slots busy (quick switching): cut the one that is already fading.
    if (fresh < 0)
        fresh = slots[0].gain < slots[1].gain ? 0 : 1;
    if (wanted == Music::NONE) {
        slots[fresh].running = false;
        return;
    }
    StartSlot(fresh, wanted, OneShot(wanted) ? 0.0f : fadeIn);
}

// Acts on a new request from the game, or starts the queued next track once the playing one has ended.
static void UpdateMusicRequest()
{
    Music wanted = (Music)requestedMusic.load(std::memory_order_acquire);
    if (wanted != lastRequest) {
        // A track still being built starts as soon as it is ready.
        if (!SongReady(wanted))
            return;
        lastRequest = wanted;
        SwitchTo(wanted, FADE_IN);
        return;
    }
    int next = nextMusic.load(std::memory_order_acquire);
    if (next < 0 || slots[0].running || slots[1].running || !SongReady((Music)next))
        return;
    if (nextMusic.compare_exchange_strong(next, -1))
        SwitchTo((Music)next, FADE_IN_NEXT);
}

// A free music voice, else the one to steal (releasing ones first, then the oldest); -1 when all are already being
// stolen.
static int FindMusicVoice()
{
    int best = -1;
    for (int i = 0; i < MUSIC_VOICES; ++i) {
        const Voice& v = musicVoices[i];
        if (!v.sample)
            return i;
        if (v.hasNext)
            continue;
        if (best < 0) {
            best = i;
            continue;
        }
        const Voice& b = musicVoices[best];
        if (v.releasing != b.releasing ? v.releasing : v.serial < b.serial)
            best = i;
    }
    return best;
}

// Starts the events of the slot's current tick and moves to the next tick.
static void SequencerTick(int slotIndex)
{
    SequencerSlot& s = slots[slotIndex];
    const Song& song = *s.song;
    while (s.eventIndex < song.events.size() && song.events[s.eventIndex].tick <= s.tick) {
        const NoteEvent& e = song.events[s.eventIndex++];
        int previous = s.channelVoice[e.channel];
        if (previous >= 0) {
            Voice& old = musicVoices[previous];
            if (old.sample && old.slot == slotIndex && old.channel == e.channel && !old.releasing)
                ReleaseVoice(old, old.releaseFrames);
        }
        int index = FindMusicVoice();
        if (index < 0)
            continue;
        Voice& v = musicVoices[index];
        PlayCommand c;
        c.sample = e.sample;
        c.step = e.step;
        c.gainL = e.gainL;
        c.gainR = e.gainR;
        c.releaseFrames = e.releaseFrames;
        c.releaseIn = e.lengthTicks > 0 ? (uint32_t)(((uint64_t)e.lengthTicks * s.framesPerTick) >> 16) : NO_RELEASE;
        s.channelVoice[e.channel] = index;
        if (!v.sample) {
            StartVoice(v, c, (int8_t)slotIndex, e.channel);
            continue;
        }

        // All voices busy: the stolen one fades out first, then plays this note.
        v.next = c;
        v.hasNext = true;
        v.nextSlot = (int8_t)slotIndex;
        v.nextChannel = e.channel;
        ReleaseVoice(v, RELEASE_MIN);
    }
    ++s.tick;
    if (s.tick >= song.lengthTicks) {
        if (song.loops) {
            s.tick = song.loopTick;
            s.eventIndex = song.loopEvent;
        } else {
            s.running = false;
        }
    }
    uint32_t total = s.framesPerTick + s.tickCarry;
    s.framesToTick = total >> 16;
    s.tickCarry = total & 0xFFFF;
}

// Mixes the music voices, splitting the block at each sequencer tick so notes start on their exact frame.
static void MixMusic(int frames)
{
    int done = 0;
    while (done < frames) {
        int n = frames - done;
        for (int i = 0; i < 2; ++i) {
            SequencerSlot& s = slots[i];
            if (s.running && s.framesToTick == 0)
                SequencerTick(i);
            if (s.running && (int)s.framesToTick < n)
                n = (int)s.framesToTick;
        }
        for (Voice& v : musicVoices) {
            if (v.sample)
                MixVoice(v, musicL[v.slot] + done, musicR[v.slot] + done, (uint32_t)n);
        }
        for (SequencerSlot& s : slots) {
            if (s.running)
                s.framesToTick -= (uint32_t)n;
        }
        done += n;
    }
}

// --- Master

// Output statistics since the last TakeMixerStats: blocks mixed in all, frames, peak and sum of squares.
static uint32_t statBlocks = 0;
static uint32_t statFrames = 0;
static int32_t statPeak = 0;
static double statSquares = 0.0;

// The limiter's gain on the master (1: not limiting).
static float limiterGain = 1.0f;

// Make-up gain on the master ahead of the limiter: brings a fight to about -16 LUFS; the limiter catches the
// stacked blasts.
constexpr float MAKEUP = 1.4f;

// Frames per limiter step, and the share of the way back to unity gain the limiter recovers per step.
constexpr int LIMIT_STEP = 32;
constexpr float LIMIT_RECOVERY = 0.005f;

// Peak the limiter holds the master to (-1 dBFS), and where the final soft clip starts.
constexpr float LIMIT_LEVEL = 0.89f * 32767.0f;
constexpr float KNEE = 0.85f * 32767.0f;
constexpr int32_t KNEE_LEVEL = (int32_t)KNEE;

// Soft clip above the knee: never exceeds full scale.
static inline float SoftKnee(float x)
{
    float a = fabsf(x);
    if (a <= KNEE)
        return x;
    float u = (a - KNEE) / (32767.0f - KNEE);
    float y = KNEE + (32767.0f - KNEE) * u / (1.0f + u);
    return x < 0.0f ? -y : y;
}

// Sums effects and music into `out`, with master volume, the limiter and the soft clip. Per sample it is integer
// arithmetic; the gains (music level, track fades, master, limiter) move once per LIMIT_STEP frames.
static void MixMaster(int16_t* out, int frames)
{
    float musicNext = musicGainTarget.load(std::memory_order_relaxed);
    float masterNext = masterTarget.load(std::memory_order_relaxed);
    if (!Finite(musicNext) || musicNext < 0.0f || musicNext > 4.0f) {
        musicNext = 1.0f;
        ++repairs;
    }
    if (!Finite(masterNext) || masterNext < 0.0f || masterNext > 4.0f) {
        masterNext = 1.0f;
        ++repairs;
    }
    if (!Finite(musicGain) || !Finite(masterGain) || !Finite(limiterGain) || limiterGain <= 0.0f) {
        musicGain = musicNext;
        masterGain = masterNext;
        limiterGain = 1.0f;
        ++repairs;
    }
    for (SequencerSlot& slot : slots) {
        if (!Finite(slot.gain) || !Finite(slot.fadeStep)) {
            slot.gain = 0.0f;
            slot.fadeStep = 0.0f;
            ++repairs;
        }
    }
    int steps = (frames + LIMIT_STEP - 1) / LIMIT_STEP;
    float musicStep = (musicNext - musicGain) / (float)steps;
    float masterStep = (masterNext - masterGain) / (float)steps;
    int32_t buffer[2 * LIMIT_STEP];
    for (int base = 0; base < frames; base += LIMIT_STEP) {
        int n = frames - base < LIMIT_STEP ? frames - base : LIMIT_STEP;
        musicGain += musicStep;
        masterGain += masterStep;
        for (SequencerSlot& slot : slots) {
            slot.gain += slot.fadeStep * (float)n;
            if (slot.gain < 0.0f)
                slot.gain = 0.0f;
            if (slot.gain >= 1.0f && slot.fadeStep > 0.0f) {
                slot.gain = 1.0f;
                slot.fadeStep = 0.0f;
            }
        }
        int32_t m0 = (int32_t)(slots[0].gain * musicGain * 4096.0f);
        int32_t m1 = (int32_t)(slots[1].gain * musicGain * 4096.0f);
        int32_t peak = 0;
        for (int i = 0; i < n; ++i) {
            int f = base + i;
            int32_t l = mixL[f] + (int32_t)(((int64_t)musicL[0][f] * m0 + (int64_t)musicL[1][f] * m1) >> 12);
            int32_t r = mixR[f] + (int32_t)(((int64_t)musicR[0][f] * m0 + (int64_t)musicR[1][f] * m1) >> 12);
            buffer[2 * i] = l;
            buffer[2 * i + 1] = r;
            int32_t a = l < 0 ? -l : l;
            int32_t b = r < 0 ? -r : r;
            if (a > peak)
                peak = a;
            if (b > peak)
                peak = b;
        }

        // The limiter: drops at once to what this block's peak needs, recovers over about 0.15 s.
        float gain = masterGain * MAKEUP;
        float loudest = (float)peak * gain;
        float needed = loudest * limiterGain > LIMIT_LEVEL ? LIMIT_LEVEL / loudest : 1.0f;
        limiterGain = needed < limiterGain ? needed : limiterGain + (1.0f - limiterGain) * LIMIT_RECOVERY;
        int32_t g = (int32_t)(gain * limiterGain * 65536.0f);
        for (int i = 0; i < 2 * n; ++i) {
            int64_t y = ((int64_t)buffer[i] * g) >> 16;
            if (y > 1000000)
                y = 1000000;
            if (y < -1000000)
                y = -1000000;
            int32_t x = (int32_t)y;
            if (x > KNEE_LEVEL || x < -KNEE_LEVEL)
                x = (int32_t)SoftKnee((float)x);
            out[2 * base + i] = (int16_t)x;
            int32_t a = x < 0 ? -x : x;
            if (a > statPeak)
                statPeak = a;
            statSquares += (double)x * (double)x;
        }
    }
    musicGain = musicNext;
    masterGain = masterNext;
    for (int s = 0; s < 2; ++s) {
        if (slots[s].fadeStep < 0.0f && slots[s].gain <= 0.0f) {
            slots[s].running = false;
            slots[s].fadeStep = 0.0f;
            ReleaseSlotVoices(s, RELEASE_MIN);
        }
    }
}

// Mixes one block of up to BLOCK frames.
static void RenderBlock(int16_t* out, int frames)
{
    for (int i = 0; i < frames; ++i) {
        mixL[i] = 0;
        mixR[i] = 0;
        musicL[0][i] = 0;
        musicR[0][i] = 0;
        musicL[1][i] = 0;
        musicR[1][i] = 0;
    }
    UpdateEffectsFade();
    DrainQueue();
    UpdateMusicRequest();
    MixMusic(frames);
    for (Voice& v : sfxVoices) {
        if (v.sample)
            MixVoice(v, mixL, mixR, (uint32_t)frames);
    }
    MixEngines(frames);
    MixMaster(out, frames);
    ++statBlocks;
    statFrames += (uint32_t)frames;
}

void MixerRender(int16_t* out, int frames)
{
    while (frames > 0) {
        int n = frames < BLOCK ? frames : BLOCK;
        RenderBlock(out, n);
        out += 2 * n;
        frames -= n;
    }
}

int MixerActiveVoices()
{
    int count = 0;
    for (const Voice& v : sfxVoices) {
        if (v.sample)
            ++count;
    }
    for (const Voice& v : musicVoices) {
        if (v.sample)
            ++count;
    }
    return count;
}

MixerStats TakeMixerStats()
{
    MixerStats stats;
    stats.blocks = statBlocks;
    stats.frames = statFrames;
    stats.peak = statPeak;
    stats.rms = statFrames > 0 ? (float)sqrt(statSquares / (2.0 * (double)statFrames)) : 0.0f;
    stats.voices = MixerActiveVoices();
    stats.engines = 0;
    for (const EngineVoice& e : engineVoices) {
        if (e.now.gainL + e.now.gainR >= ENGINE_SILENT)
            ++stats.engines;
    }
    stats.music = (int)currentMusic;
    stats.request = (int)lastRequest;
    stats.musicGain = musicGain;
    stats.masterGain = masterGain;
    stats.limiterGain = limiterGain;
    stats.repairs = repairs;
    statFrames = 0;
    statPeak = 0;
    statSquares = 0.0;
    return stats;
}

} // namespace audio
