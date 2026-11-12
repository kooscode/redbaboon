// The audio interface the game calls (audio.h): picks effect variants, computes 3D gain, pan, distance filtering,
// delay and Doppler on the game thread, and hands the results to the mixer without ever blocking
// (docs/design.md §6, creative brief §7).
#include "audio/audio.h"

#include "audio/mixer.h"
#include "audio/music.h"
#include "audio/sfx.h"

#include <math.h>
#include <stdio.h>

namespace audio {

// True once the bank is built and the device is open; until then every call does nothing.
static bool ready = false;

// Random numbers for variants and pitch jitter.
static Rng rng(0x5EED1234u);

// Seconds since Init, advanced by Update.
static float elapsed = 0.0f;

// --- Effect tables

// Per effect: base gain, priority (higher wins a voice), pitch jitter (fraction), most instances at once, and
// the distance (m) up to which a 3D sound plays at full volume.
struct SfxStyle {
    float gain;
    float priority;
    float jitter;
    int maxInstances;
    float referenceDistance;
    bool delayed;           // arrives late from far away (speed of sound)
    bool bark;              // a voice bark (at most one every BARK_INTERVAL)
};

static const SfxStyle SFX_STYLES[(int)Sfx::COUNT] = {
    {0.45f, 3.0f, 0.0f, 6, 25.0f, false, false},       // GUN: the game varies the pitch
    {0.70f, 2.0f, 0.04f, 3, 40.0f, false, false},      // ROCKET
    {0.55f, 3.0f, 0.05f, 4, 30.0f, false, false},      // HIT
    {1.00f, 2.8f, 0.06f, 5, 150.0f, true, false},      // EXPLOSION
    {1.00f, 3.6f, 0.0f, 2, 400.0f, true, false},       // BIG_EXPLOSION
    {0.65f, 1.5f, 0.06f, 3, 80.0f, true, false},       // SPLASH
    {0.55f, 2.5f, 0.0f, 2, 30.0f, false, false},       // PICKUP
    {0.55f, 2.5f, 0.0f, 2, 30.0f, false, false},       // RING
    {0.55f, 2.0f, 0.05f, 2, 30.0f, false, false},      // STUNT
    {0.40f, 4.0f, 0.0f, 1, 30.0f, false, false},       // WARNING
    {0.70f, 2.2f, 0.03f, 1, 30.0f, false, true},       // MONKEY_CHEER
    {0.65f, 2.2f, 0.03f, 1, 30.0f, false, true},       // MONKEY_HURT
    {0.75f, 2.2f, 0.03f, 1, 60.0f, false, true},       // ENEMY_TAUNT
    {0.35f, 4.0f, 0.0f, 2, 30.0f, false, false},       // MENU_MOVE
    {0.45f, 4.0f, 0.0f, 2, 30.0f, false, false},       // MENU_SELECT
    {0.55f, 3.0f, 0.0f, 1, 30.0f, false, false},       // OVERHEAT
    {0.60f, 3.0f, 0.03f, 1, 30.0f, false, false},      // GLASS_CRACK
    {0.60f, 2.0f, 0.05f, 2, 40.0f, false, false},      // SPUTTER
    {0.40f, 4.0f, 0.0f, 1, 30.0f, false, false},       // RADIO_BLIP
    {0.75f, 3.2f, 0.05f, 2, 30.0f, false, false},      // CRUNCH
    {0.70f, 3.5f, 0.0f, 1, 30.0f, false, false},       // KILL_STING
    {1.00f, 5.0f, 0.0f, 1, 30.0f, false, false},       // ZEPPELIN_DEATH
};

// Least time between two voice barks (seconds).
constexpr float BARK_INTERVAL = 3.0f;

// When the last bark played, and the last variant of each effect (never the same twice in a row).
static float lastBark = -1000.0f;
static int lastVariant[(int)Sfx::COUNT];

// Speed of sound (m/s), for explosion delays and Doppler.
constexpr float SOUND_SPEED = 340.0f;

// Longest delay given to a distant explosion (frames).
constexpr uint32_t MAX_DELAY = MIX_RATE * 2;

// Quietest sound still started.
constexpr float AUDIBLE = 0.004f;

// An explosion (big or not) louder than this ducks the music for KILL_DUCK_TIME seconds, as kill stings do.
constexpr float BLAST_DUCK_LEVEL = 0.25f;
constexpr float KILL_DUCK_TIME = 0.6f;

// The zeppelin's end ducks the music for its whole length.
constexpr float ZEPPELIN_DUCK_TIME = 6.0f;

// True for a usable float (not NaN or infinite): one NaN from the game (a camera or plane state) would otherwise
// reach the mixer and stay there.
static bool Finite(float x)
{
    return x == x && x - x == 0.0f;
}

static bool Finite(const Vec3& v)
{
    return Finite(v.x) && Finite(v.y) && Finite(v.z);
}

// --- Listener

static Vec3 listenerPosition;
static Vec3 listenerForward(0.0f, 0.0f, -1.0f);
static Vec3 listenerRight(1.0f, 0.0f, 0.0f);
static Vec3 listenerVelocity;
static Vec3 previousListener;
static bool listenerKnown = false;

void SetListener(const Vec3& position, const Vec3& forward, const Vec3& up)
{
    if (!Finite(position) || !Finite(forward) || !Finite(up))
        return;
    listenerPosition = position;
    listenerForward = Normalize(forward);
    Vec3 right = Normalize(Cross(forward, up));
    if (LengthSq(right) > 0.0f)
        listenerRight = right;
}

// Constant-power pan: gains for -1 (left) .. 1 (right), 1/sqrt(2) each at the centre.
static void PanGains(float pan, float gain, float& left, float& right)
{
    float angle = (Clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.25f * PI;
    left = cosf(angle) * gain;
    right = sinf(angle) * gain;
}

// One-pole lowpass coefficient at the mixer rate for a cutoff.
static float LowpassCoefficient(float hz)
{
    return 1.0f - expf(-2.0f * PI * hz / (float)MIX_RATE);
}

// Cutoff for a sound `distance` metres away (air absorbs highs), darker behind the listener.
static float DistanceCutoff(float distance, float behind)
{
    float hz = 14000.0f / (1.0f + distance / 180.0f);
    hz *= 1.0f - 0.35f * behind;
    return hz < 500.0f ? 500.0f : hz;
}

// --- Effects

// Picks a variant, never the one played last.
static int PickVariant(Sfx sfx)
{
    int count = SfxVariantCount(sfx);
    if (count <= 1)
        return 0;
    if (lastVariant[(int)sfx] < 0) {
        lastVariant[(int)sfx] = (int)(rng.Next() % (uint32_t)count);
        return lastVariant[(int)sfx];
    }
    int variant = (int)(rng.Next() % (uint32_t)(count - 1));
    if (variant >= lastVariant[(int)sfx])
        ++variant;
    lastVariant[(int)sfx] = variant;
    return variant;
}

// Queues an effect with final gains, pitch, filter and delay. Barks over the limit are dropped.
static void Queue(Sfx sfx, float left, float right, float pitch, float lowpass, uint32_t delay)
{
    const SfxStyle& style = SFX_STYLES[(int)sfx];
    if (!Finite(left) || !Finite(right) || !Finite(pitch) || !Finite(lowpass) || pitch <= 0.01f || pitch > 8.0f)
        return;
    float loudness = left > right ? left : right;
    if (loudness < AUDIBLE)
        return;
    if (style.bark) {
        if (elapsed - lastBark < BARK_INTERVAL)
            return;
        lastBark = elapsed;
    }
    const Sample& sample = SfxVariant(sfx, PickVariant(sfx));
    float jitter = 1.0f + style.jitter * rng.Bipolar();
    PlayCommand c;
    c.sample = &sample;
    c.step = (uint32_t)(pitch * jitter * (float)sample.rate / (float)MIX_RATE * 65536.0f);
    c.gainL = (uint16_t)(Clamp(left, 0.0f, 1.0f) * 32767.0f);
    c.gainR = (uint16_t)(Clamp(right, 0.0f, 1.0f) * 32767.0f);
    c.lowpass = (uint16_t)(Clamp(lowpass, 0.0f, 1.0f) * 16384.0f);
    c.delayFrames = delay;
    c.priority = style.priority + loudness;
    c.sfx = (uint8_t)sfx;
    c.maxInstances = (uint8_t)style.maxInstances;
    if (!MixerPlay(c))
        return;

    // Kills and near blasts get room: the music dips for a moment.
    bool big = sfx == Sfx::BIG_EXPLOSION || sfx == Sfx::EXPLOSION;
    bool blast = big && loudness > BLAST_DUCK_LEVEL;
    if (blast || sfx == Sfx::KILL_STING)
        DuckMusic(KILL_DUCK_TIME);
    if (sfx == Sfx::ZEPPELIN_DEATH)
        DuckMusic(ZEPPELIN_DUCK_TIME);
}

void Play(Sfx sfx, float volume, float pan, float pitch)
{
    if (!ready || sfx >= Sfx::COUNT || !SfxReady(sfx))
        return;
    float left = 0.0f;
    float right = 0.0f;
    PanGains(pan, volume * SFX_STYLES[(int)sfx].gain * 1.41421356f, left, right);
    Queue(sfx, left, right, pitch, 1.0f, 0);
}

void FadeOutEffects(float seconds)
{
    if (!ready || !Finite(seconds))
        return;
    MixerFadeOutEffects(seconds);
}

void Play3D(Sfx sfx, const Vec3& position, float volume, float pitch)
{
    if (!ready || sfx >= Sfx::COUNT || !SfxReady(sfx))
        return;
    const SfxStyle& style = SFX_STYLES[(int)sfx];
    if (!Finite(position))
        return;
    Vec3 offset = position - listenerPosition;
    float distance = Length(offset);
    Vec3 direction = distance > 0.01f ? offset / distance : listenerForward;
    float attenuation = distance <= style.referenceDistance ? 1.0f : style.referenceDistance / distance;
    float behind = Clamp(-Dot(direction, listenerForward), 0.0f, 1.0f);
    float pan = Dot(direction, listenerRight) * 0.85f;
    float left = 0.0f;
    float right = 0.0f;
    PanGains(pan, volume * style.gain * attenuation * (1.0f - 0.15f * behind) * 1.41421356f, left, right);
    uint32_t delay = 0;
    if (style.delayed) {
        float frames = distance / SOUND_SPEED * (float)MIX_RATE;
        delay = frames > (float)MAX_DELAY ? MAX_DELAY : (uint32_t)frames;
    }
    Queue(sfx, left, right, pitch, LowpassCoefficient(DistanceCutoff(distance, behind)), delay);
}

// --- Engines

// Engine bookkeeping on the game thread.
struct Engine {
    bool used = false;
    EngineType type = EngineType::PIRATE;
    Vec3 position;
    Vec3 velocity;
    float throttle = 0.0f;
    float volume = 0.0f;
    float boost = 0.0f;
    float detune = 1.0f;          // each pirate a little different, so a formation beats instead of droning
};

static Engine engines[MAX_ENGINES];

// Overall engine level (kept low: the drone is always there), the extra level at full boost (+3 dB), and the
// distance up to which an engine plays at full volume.
constexpr float ENGINE_GAIN = 0.3f;
constexpr float BOOST_GAIN = 0.41f;
constexpr float ENGINE_REFERENCE = 22.0f;

// Doppler is scaled down to keep it "light", and clamped.
constexpr float DOPPLER_SCALE = 0.65f;

// Distance under which a passing plane whooshes (m), and the relative speed of a full whoosh (m/s).
constexpr float WHOOSH_DISTANCE = 45.0f;
constexpr float WHOOSH_SPEED = 90.0f;

EngineId CreateEngine(EngineType type)
{
    if (!ready)
        return -1;
    for (int i = 0; i < MAX_ENGINES; ++i) {
        Engine& e = engines[i];
        if (e.used)
            continue;
        e = Engine();
        e.used = true;
        e.type = type;
        e.detune = type == EngineType::PIRATE ? rng.Range(0.95f, 1.05f) : 1.0f;
        return i;
    }
    return -1;
}

void SetEngine(EngineId engine, const Vec3& position, const Vec3& velocity, float throttle, float volume,
               float boost)
{
    if (engine < 0 || engine >= MAX_ENGINES || !engines[engine].used)
        return;
    Engine& e = engines[engine];
    if (!Finite(position) || !Finite(velocity) || !Finite(throttle) || !Finite(volume) || !Finite(boost))
        return;
    e.position = position;
    e.velocity = velocity;
    e.throttle = Clamp(throttle, 0.0f, 1.0f);
    e.volume = Clamp(volume, 0.0f, 1.0f);
    e.boost = Clamp(boost, 0.0f, 1.0f);
}

void DestroyEngine(EngineId engine)
{
    if (engine < 0 || engine >= MAX_ENGINES)
        return;
    engines[engine].used = false;
}

// Loudness of an engine at the listener before panning.
// The zeppelin's fall is heard from far away and sits under the music, not on top of it.
constexpr float FALL_REFERENCE = 300.0f;
constexpr float FALL_GAIN = 1.6f;

static float EngineLoudness(const Engine& e)
{
    float distance = Length(e.position - listenerPosition);
    float reference = e.type == EngineType::ZEPPELIN_FALL ? FALL_REFERENCE : ENGINE_REFERENCE;
    float attenuation = distance <= reference ? 1.0f : reference / distance;
    return e.volume * attenuation;
}

static EngineParams EngineSound(const Engine& e)
{
    EngineParams p;
    Vec3 offset = e.position - listenerPosition;
    float distance = Length(offset);
    Vec3 direction = distance > 0.5f ? offset / distance : listenerForward;
    float behind = Clamp(-Dot(direction, listenerForward), 0.0f, 1.0f);
    float pan = distance > 0.5f ? Dot(direction, listenerRight) * 0.8f : 0.0f;
    float level = EngineLoudness(e) * ENGINE_GAIN * (1.0f + BOOST_GAIN * e.boost);
    if (e.type == EngineType::ZEPPELIN_FALL)
        level = EngineLoudness(e) * ENGINE_GAIN * FALL_GAIN;
    PanGains(pan, level * 1.41421356f, p.gainL, p.gainR);

    // Doppler: source moving away (+) lowers the pitch, listener moving toward it (+) raises it.
    float away = Dot(e.velocity, direction) * DOPPLER_SCALE;
    float toward = Dot(listenerVelocity, direction) * DOPPLER_SCALE;
    float doppler = Clamp((SOUND_SPEED + toward) / (SOUND_SPEED + away), 0.6f, 1.6f);

    float throttle = e.throttle;
    float pitch = e.type == EngineType::PLAYER ? 0.8f + 0.55f * throttle : 0.78f + 0.5f * throttle;
    if (e.type == EngineType::ZEPPELIN_FALL)
        pitch = 0.9f + 0.45f * throttle;
    p.rate = pitch * e.detune * doppler * (1.0f + 0.06f * e.boost);
    p.throttleMix = throttle * throttle * (3.0f - 2.0f * throttle);
    p.lowpass = LowpassCoefficient(DistanceCutoff(distance, behind));
    p.whineGain = e.boost * 0.30f;
    p.whineRate = (900.0f + 500.0f * e.boost) / WHINE_BASE_HZ * doppler;

    // Fly-by whoosh: rushing air that swells as a fast plane passes close.
    float closeness = Clamp(1.0f - distance / WHOOSH_DISTANCE, 0.0f, 1.0f);
    float speed = Clamp(Length(e.velocity - listenerVelocity) / WHOOSH_SPEED, 0.0f, 1.0f);
    p.windGain = closeness * closeness * speed * e.volume * 0.9f;
    p.windRate = doppler;
    return p;
}

// Picks the AUDIBLE_ENGINES loudest engines and sends every slot its sound (silence for the rest).
static void UpdateEngines()
{
    int chosen[AUDIBLE_ENGINES];
    float loudness[AUDIBLE_ENGINES];
    int count = 0;
    for (int i = 0; i < MAX_ENGINES; ++i) {
        if (!engines[i].used)
            continue;
        float l = EngineLoudness(engines[i]);
        if (l < AUDIBLE)
            continue;
        int at = count < AUDIBLE_ENGINES ? count++ : AUDIBLE_ENGINES;
        if (at == AUDIBLE_ENGINES) {
            int weakest = 0;
            for (int k = 1; k < AUDIBLE_ENGINES; ++k) {
                if (loudness[k] < loudness[weakest])
                    weakest = k;
            }
            if (loudness[weakest] >= l)
                continue;
            at = weakest;
        }
        chosen[at] = i;
        loudness[at] = l;
    }
    for (int i = 0; i < MAX_ENGINES; ++i) {
        bool audible = false;
        for (int k = 0; k < count; ++k) {
            if (chosen[k] == i)
                audible = true;
        }
        EngineParams p = audible ? EngineSound(engines[i]) : EngineParams();
        MixerSetEngine(i, engines[i].type, p);
    }
}

// --- Music and levels

static Music music = Music::NONE;
static float musicVolume = 1.0f;
static float duckTime = 0.0f;

// Level of each track under the effects: the fight tracks sit 5 dB lower than the title and the stings. NONE keeps
// the level of the track fading out.
static const float TRACK_GAINS[(int)Music::DEFEAT + 1] = {0.0f, 1.0f, 0.56f, 0.56f, 0.9f, 0.9f};
static float trackGain = 1.0f;

// Ducked music level (-6 dB).
constexpr float DUCK_GAIN = 0.5f;

void SetMusic(Music track)
{
    if (!ready || track == music)
        return;
    music = track;
    if (track != Music::NONE)
        trackGain = TRACK_GAINS[(int)track];
    MixerSetMusic(track);
}

void SetMusicNext(Music track)
{
    if (!ready || track == music)
        return;
    music = track;
    if (track != Music::NONE)
        trackGain = TRACK_GAINS[(int)track];
    MixerSetNextMusic(track);
}

void DuckMusic(float seconds)
{
    if (seconds > duckTime)
        duckTime = seconds;
}

void SetMusicVolume(float volume)
{
    musicVolume = Clamp(volume, 0.0f, 1.0f);
}

void SetMasterVolume(float volume)
{
    MixerSetMasterVolume(Clamp(volume, 0.0f, 1.0f));
}

// --- Diagnostics

// Seconds between two diagnostics lines.
constexpr float LOG_INTERVAL = 5.0f;

// Seconds since the last diagnostics line.
static float logTimer = 0.0f;

// Every LOG_INTERVAL: the mixer's output level and state and the device's, one line to the debug log.
static void LogDiagnostics(float dt)
{
    logTimer += dt;
    if (logTimer < LOG_INTERVAL)
        return;
    logTimer = 0.0f;
    MixerStats m = TakeMixerStats();
    char device[320];
    DeviceStatus(device, sizeof(device));
    char line[640];
    snprintf(line, sizeof(line),
             "RedBaboon audio: blocks %u peak %d rms %d voices %d engines %d music %d/%d gain music %d%% master %d%% "
             "limiter %d%% duck %d repairs %u | %s",
             m.blocks, m.peak, (int)m.rms, m.voices, m.engines, m.music, m.request, (int)(m.musicGain * 100.0f),
             (int)(m.masterGain * 100.0f), (int)(m.limiterGain * 100.0f), duckTime > 0.0f ? 1 : 0, m.repairs, device);
    DeviceLog(line);
}

// --- Frame update and start-up

void Update(float dt)
{
    if (!ready)
        return;
    elapsed += dt;
    if (!Finite(dt) || dt < 0.0f)
        dt = 0.0f;
    if (dt > 0.0f && listenerKnown) {
        Vec3 velocity = (listenerPosition - previousListener) / dt;
        if (!Finite(velocity) || LengthSq(velocity) > 400.0f * 400.0f)
            velocity = Vec3();
        listenerVelocity = Lerp(listenerVelocity, velocity, Clamp(dt * 10.0f, 0.0f, 1.0f));
        if (!Finite(listenerVelocity))
            listenerVelocity = Vec3();
    }
    previousListener = listenerPosition;
    listenerKnown = true;
    duckTime = duckTime > dt ? duckTime - dt : 0.0f;
    float level = musicVolume * trackGain;
    MixerSetMusicGain(level * (duckTime > 0.0f ? DUCK_GAIN : 1.0f));
    UpdateEngines();
    LogDiagnostics(dt);
}

// Builds the whole bank, the title theme and the menu sounds first (they are wanted on the loading screen).
static void BuildBank()
{
    BuildTitleMusic();
    BuildMenuSounds();
    BuildSfxBank();
    BuildMusic();
}

bool Init()
{
    if (ready)
        return true;
    for (int& v : lastVariant)
        v = -1;
    MixerSetMusicGain(musicVolume);
    if (!DeviceOpen())
        return false;
    ready = true;
    DeviceRunInBackground(BuildBank);
    return true;
}

void WaitUntilLoaded()
{
    if (ready)
        DeviceWaitForBackground();
}

} // namespace audio
