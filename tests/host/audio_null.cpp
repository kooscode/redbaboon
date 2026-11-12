// Silent audio for host tests (audio/audio.h): every call is accepted and ignored; engine voices get ids.
#include "audio/audio.h"

namespace audio {

// Engine voices handed out so far.
static int nextEngine = 0;

void Play(Sfx sfx, float volume, float pan, float pitch)
{
    (void)sfx;
    (void)volume;
    (void)pan;
    (void)pitch;
}

void FadeOutEffects(float seconds)
{
    (void)seconds;
}

void Play3D(Sfx sfx, const Vec3& position, float volume, float pitch)
{
    (void)sfx;
    (void)position;
    (void)volume;
    (void)pitch;
}

void SetListener(const Vec3& position, const Vec3& forward, const Vec3& up)
{
    (void)position;
    (void)forward;
    (void)up;
}

EngineId CreateEngine(EngineType type)
{
    (void)type;
    return nextEngine++;
}

void SetEngine(EngineId engine, const Vec3& position, const Vec3& velocity, float throttle, float volume, float boost)
{
    (void)engine;
    (void)position;
    (void)velocity;
    (void)throttle;
    (void)volume;
    (void)boost;
}

void DestroyEngine(EngineId engine)
{
    (void)engine;
}

void SetMusic(Music music)
{
    (void)music;
}

void SetMusicNext(Music music)
{
    (void)music;
}

void DuckMusic(float seconds)
{
    (void)seconds;
}

void SetMusicVolume(float volume)
{
    (void)volume;
}

void SetMasterVolume(float volume)
{
    (void)volume;
}

void Update(float dt)
{
    (void)dt;
}

bool Init()
{
    return true;
}

void WaitUntilLoaded()
{
    // Nothing is built.
}

} // namespace audio
