// Audio: synthesised sound effects, looping engine voices and music, mixed in software and played through the
// Xbox's AC'97. Platform independent apart from audio_xbox.cpp. See docs/design.md §6.
#pragma once

#include "core/vmath.h"

namespace audio {

enum class Sfx {
    GUN,             // banana machine gun, one shot (short, punchy; plays many times a second)
    ROCKET,          // banana rocket launch (whoosh)
    HIT,             // bullets hitting a plane (metal clank)
    EXPLOSION,       // plane explodes
    BIG_EXPLOSION,   // zeppelin / large target explodes (long rumble)
    SPLASH,          // something hits the water
    PICKUP,          // health / ammo pickup
    RING,            // flew through a stunt ring
    STUNT,           // stunt move started (wind swoosh)
    WARNING,         // low health / incoming beep
    MONKEY_CHEER,    // the baboon pilot whoops (kill)
    MONKEY_HURT,     // the baboon pilot screeches (hit hard)
    ENEMY_TAUNT,     // gorilla grunt
    MENU_MOVE,
    MENU_SELECT,
    OVERHEAT,        // guns overheated: steam hiss + clank
    GLASS_CRACK,     // canopy cracks (player health low)
    SPUTTER,         // engine on fire coughs and backfires
    RADIO_BLIP,      // radio message starts (squelch click + chirp)
    CRUNCH,          // the player's plane is hit (cockpit crunch)
    KILL_STING,      // the player shot a plane down: xylophone + brass stab (also ducks the music for 0.6 s)
    ZEPPELIN_DEATH,  // the zeppelin's end: three blasts, groaning metal, a long rumble and a huge splash (8 s)
    COUNT
};

enum class Music {
    NONE,
    TITLE,
    COMBAT,
    BOSS,
    VICTORY,
    DEFEAT,
};

// Plays a 2D sound. volume 0..1, pan -1 (left) .. 1 (right), pitch 1 = normal (a little random jitter is added).
// Never blocks. The voice barks (MONKEY_CHEER, MONKEY_HURT, ENEMY_TAUNT) play at most once every 3 s; extra calls
// are dropped, so call them freely.
void Play(Sfx sfx, float volume = 1.0f, float pan = 0.0f, float pitch = 1.0f);

// Fades out every effect that is playing (not the menu sounds) over `seconds`: call it on a screen change (results,
// pause, title, restart) with the engines silenced, so the scene's sound goes out instead of stopping.
void FadeOutEffects(float seconds = 0.6f);

// Plays a sound at a world position, attenuated, panned and lowpassed for the listener; explosions and splashes
// arrive late from far away (distance / 340 m/s).
void Play3D(Sfx sfx, const Vec3& position, float volume = 1.0f, float pitch = 1.0f);

// Listener position and orientation (usually the camera).
void SetListener(const Vec3& position, const Vec3& forward, const Vec3& up);

// Looping propeller-engine voices, one per plane. throttle 0..1 changes pitch and roughness; boost 0..1 adds the
// supercharger whine. Only the ~6 loudest engines are heard. An invalid id (-1 when all slots are taken, or when
// audio is off) is ignored by SetEngine and DestroyEngine. Silenced or destroyed voices fade out over 0.6 s.
//
// ZEPPELIN_FALL is the sound bed of the zeppelin's death, heard from far away: CreateEngine it when the zeppelin
// starts going down, SetEngine it every frame at the hull's position with throttle = how far the fall has got
// (0 at the start: fire, groans and the engines sputtering out; 1 at impact: a rising wind whistle over the fire),
// then DestroyEngine it and Play(Sfx::ZEPPELIN_DEATH) when the hull hits the sea.
using EngineId = int;

enum class EngineType {
    PIRATE,          // lumpy radial chug
    PLAYER,          // smooth inline growl
    ZEPPELIN_FALL,   // the burning zeppelin going down: fire, sputtering engines, groaning metal, rising wind
    COUNT
};

EngineId CreateEngine(EngineType type = EngineType::PIRATE);
void SetEngine(EngineId engine, const Vec3& position, const Vec3& velocity, float throttle, float volume,
               float boost = 0.0f);
void DestroyEngine(EngineId engine);

// Cross-fades to another music track (the old one fades out over 0.8 s, the new one in over 0.3 s; VICTORY and
// DEFEAT start at full level). VICTORY and DEFEAT play once, then the music stops (or SetMusicNext's track
// starts). Asking for the track already playing does nothing.
void SetMusic(Music music);

// Starts `music` when the playing track has played its last note (after the VICTORY fanfare, for example), fading
// in over 0.8 s while the fanfare's last chord rings out, or now if nothing is playing. A later SetMusic cancels it.
void SetMusicNext(Music music);

// Lowers the music by 6 dB for `seconds` (under radio messages).
void DuckMusic(float seconds);

void SetMusicVolume(float volume);
void SetMasterVolume(float volume);

// Per-frame bookkeeping (listener velocity, engines, ducking). Call once per frame from the main thread, after
// SetListener and SetEngine.
void Update(float dt);

// Opens the audio device and starts building the sound bank on a background thread (the title theme and menu sounds
// first, ready after ~0.2 s on the Xbox; everything after ~1 s), so it returns at once. Until a sound or a track is
// built, Play skips it and SetMusic waits for it. Returns false if audio is unavailable: then every call is a
// harmless no-op and the game runs silent. Call from the main thread before anything else.
bool Init();

// Waits until the whole sound bank is built (call before gameplay starts; normally it already is by then).
void WaitUntilLoaded();

} // namespace audio
