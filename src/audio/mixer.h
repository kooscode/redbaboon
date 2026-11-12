// The software mixer (mixer.cpp): effect voices, engine voices and the music sequencer, mixed into 48 kHz stereo
// 16-bit with a soft limiter on the master. The game thread talks to it only through the Mixer* setters, which never
// block: effects go through a bounded lock-free queue (dropped when it is full, as when the device has stopped
// calling back), everything else through atomics the mixer reads at the start of each buffer.
#pragma once

#include "audio/audio.h"
#include "audio/synth.h"

namespace audio {

// Effect voices (with priority stealing), engine voices and music voices.
constexpr int SFX_VOICES = 24;
constexpr int MAX_ENGINES = 32;
constexpr int MUSIC_VOICES = 40;

// The most engines heard at once (the loudest).
constexpr int AUDIBLE_ENGINES = 6;

// How a voice starts: an effect from the game, or a note from the sequencer.
struct PlayCommand {
    const Sample* sample = nullptr;
    uint32_t step = 65536;             // source frames per output frame, 16.16 fixed point
    uint16_t gainL = 0;                // 0..32767
    uint16_t gainR = 0;
    uint16_t lowpass = 16384;          // one-pole lowpass coefficient, 2.14 fixed point; 16384 = open
    uint32_t releaseFrames = 64;       // fade-out length at note-off or when stolen
    uint32_t delayFrames = 0;          // silence before the sound starts (distant explosions)
    uint32_t releaseIn = 0xFFFFFFFFu;  // frames until note-off; 0xFFFFFFFF: plays to its end
    float priority = 0.0f;             // higher steals lower when all voices are busy
    uint8_t sfx = 0;                   // which effect (instance limits)
    uint8_t maxInstances = 255;        // most voices of this effect at once: the oldest is replaced
};

// Queues an effect. Returns false (dropped) when the queue is full.
bool MixerPlay(const PlayCommand& command);

// Fades out every playing effect except the menu sounds over `seconds`.
void MixerFadeOutEffects(float seconds);

// The sound of one engine, computed by the game thread from the plane and the listener.
struct EngineParams {
    float gainL = 0.0f;
    float gainR = 0.0f;
    float rate = 1.0f;          // playback rate of the drone (pitch and Doppler)
    float throttleMix = 0.0f;   // 0 idle layer .. 1 full-throttle layer
    float lowpass = 1.0f;       // one-pole lowpass coefficient (1 open), for distance
    float whineGain = 0.0f;     // supercharger whine
    float whineRate = 1.0f;
    float windGain = 0.0f;      // fly-by rush of air
    float windRate = 1.0f;
};

// Sets the target sound of engine slot 0..MAX_ENGINES-1; the mixer glides to it over one buffer. Zero gains
// silence it.
void MixerSetEngine(int slot, EngineType type, const EngineParams& params);

// Starts a music track (cross-fading from the current one); NONE fades out. Cancels a queued next track.
void MixerSetMusic(Music music);

// Starts `music` (fading in) once the playing track has played its last note (a one-shot: VICTORY, DEFEAT), or now
// if none is playing; the one-shot's ring-out fades out over 1.5 s under it, so there is no gap.
void MixerSetNextMusic(Music music);

// Music level (volume times ducking) and master level, 0..1; the mixer glides to them.
void MixerSetMusicGain(float gain);
void MixerSetMasterVolume(float volume);

// Mixes `frames` stereo frames (interleaved left, right) into `out`. Called by the audio device's thread only.
void MixerRender(int16_t* out, int frames);

// For tests: voices sounding now (effects + music).
int MixerActiveVoices();

// What the mixer has been doing, for the diagnostics log: read from another thread, so values may be a block apart.
struct MixerStats {
    uint32_t blocks;            // blocks mixed since start
    uint32_t frames;            // output frames since the last TakeMixerStats
    int32_t peak;               // largest output sample since then
    float rms;                  // output RMS since then (0..32767)
    int voices;                 // effect + music voices sounding
    int engines;                // engine voices sounding
    int music;                  // track playing (Music), and the last one asked for
    int request;
    float musicGain;
    float masterGain;
    float limiterGain;
    uint32_t repairs;           // non-finite values found and reset
};

// Returns the statistics and starts the next period.
MixerStats TakeMixerStats();

// --- Device (audio_xbox.cpp on the Xbox; a host test supplies its own)

// Opens the output device at MIX_RATE stereo S16 and starts calling MixerRender from its own thread. False: no
// audio.
bool DeviceOpen();

// Runs `work` on a background thread (the sound bank build), or right here where there are no threads.
void DeviceRunInBackground(void (*work)());

// Waits until the background work has finished.
void DeviceWaitForBackground();

// Writes a line to the debug log (the serial port on the Xbox).
void DeviceLog(const char* text);

// The output device's state in a few words (registers, queue), for the diagnostics log.
void DeviceStatus(char* text, int size);

} // namespace audio
