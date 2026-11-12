// Particle effects (docs/design.md §5, creative brief §3): explosions in layers (flash, fireball, rising smoke,
// spark streaks, tumbling debris with smoke trails), damage smoke and fire, muzzle flashes, hit sparks, flak bursts,
// splashes and spray. A fixed pool of billboards from the particle atlas (the renderer sorts the alpha ones).
#include "game/world.h"

namespace game {

// Particle atlas frames (content.h: 4 x 4).
constexpr int FRAME_SMOKE = 0;
constexpr int FRAME_FIRE = 4;
constexpr int FRAME_FLASH = 9;
constexpr int FRAME_BANANA = 10;
constexpr int FRAME_GLOW = 11;
constexpr int FRAME_DEBRIS = 12;
constexpr int ATLAS_SIZE = 4;

constexpr int MAX_PARTICLES = 1400;
// Big effects (fireballs, smoke columns, splashes) stop adding particles past this many live ones, so a busy
// moment (the zeppelin's death with kills around it) cannot pile up fill-rate cost; small ones still get the rest.
constexpr int BIG_EFFECT_BUDGET = 700;
constexpr float DEBRIS_TRAIL_GAP = 0.03f;  // seconds between a debris chunk's smoke puffs
constexpr float NEAR_FADE = 18.0f;         // alpha particles fade out within this distance of the camera
// The zeppelin's splash: one column, rising about 2 s and falling about 2 s. Big effects are kept lean for the GPU's
// fill rate (three 40-billboard columns dropped the sinking to 52 fps on the console, 2026-11-11); billboards are
// also capped in screen size (MAX_SCREEN).
constexpr int ZEPPELIN_COLUMN = 30;
constexpr int ZEPPELIN_RING = 16;
constexpr int ZEPPELIN_FOAM = 16;
constexpr float COLUMN_RISE = 2.0f;        // seconds for every part of the column to reach its height
constexpr float MAX_SCREEN = 0.35f;        // a billboard covers at most this much of the screen height

enum class Look {
    ALPHA,    // smoke, debris, spray: blended, sorted back to front
    ADDITIVE, // fire, flashes, glows
    STREAK,   // additive spark streaks along the velocity
};

struct Particle {
    Vec3 position;
    Vec3 velocity;
    float life;      // seconds left
    float maxLife;
    float size0;
    float size1;
    float rotation;
    float spin;
    float drag;      // velocity lost per second (fraction)
    float lift;      // m/s^2 upward (negative = falls)
    uint32_t color0; // colour at birth and at death (0xAARRGGBB)
    uint32_t color1;
    int frame;
    int frames;      // > 1: animates through the frames over its life
    Look look;
    float trail;     // > 0: leaves a smoke puff every this many seconds (debris)
    float trailTimer;
};

// The pool; live particles are packed at the front.
static Particle particles[MAX_PARTICLES];
static int particleCount = 0;

// Draw buffers, rebuilt every frame.
static render::Billboard alphaBoards[MAX_PARTICLES];
static render::Billboard addBoards[MAX_PARTICLES];
static render::Streak streaks[MAX_PARTICLES];

// --- Pool

static uint32_t LerpColor(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        float ca = static_cast<float>((a >> shift) & 0xFF);
        float cb = static_cast<float>((b >> shift) & 0xFF);
        uint32_t c = static_cast<uint32_t>(Lerp(ca, cb, t) + 0.5f);
        out |= (c & 0xFF) << shift;
    }
    return out;
}

// A new particle with neutral settings, or nullptr when the pool is full (effects are optional).
static Particle* NewParticle(const Vec3& position, const Vec3& velocity, float life, Look look)
{
    if (particleCount >= MAX_PARTICLES)
        return nullptr;
    Particle& p = particles[particleCount++];
    p = Particle();
    p.position = position;
    p.velocity = velocity;
    p.life = life;
    p.maxLife = life;
    p.size0 = 1.0f;
    p.size1 = 1.0f;
    p.color0 = 0xFFFFFFFF;
    p.color1 = 0xFFFFFFFF;
    p.frames = 1;
    p.look = look;
    p.rotation = world.rng.Range(0.0f, 2.0f * PI);
    return p.life > 0.0f ? &p : nullptr;
}

static Vec3 RandomDirection()
{
    for (int i = 0; i < 8; i++) {
        Vec3 v(world.rng.Range(-1, 1), world.rng.Range(-1, 1), world.rng.Range(-1, 1));
        float l = LengthSq(v);
        if (l > 0.01f && l <= 1.0f)
            return v / sqrtf(l);
    }
    return Vec3(0, 1, 0);
}

void ClearEffects()
{
    particleCount = 0;
}

// --- Effects

void SmokePuff(const Vec3& position, const Vec3& velocity, float size, float darkness)
{
    Particle* p = NewParticle(position, velocity, world.rng.Range(1.6f, 2.6f), Look::ALPHA);
    if (!p)
        return;
    float shade = Lerp(0.75f, 0.2f, darkness);
    p->size0 = size;
    p->size1 = size * 3.2f;
    p->color0 = PackARGB(shade, shade * 0.95f, shade * 0.9f, 0.75f);
    p->color1 = PackARGB(shade + 0.2f, shade + 0.18f, shade + 0.16f, 0.0f);
    p->frame = FRAME_SMOKE + static_cast<int>(world.rng.Next() % 4);
    p->spin = world.rng.Range(-0.6f, 0.6f);
    p->drag = 1.2f;
    p->lift = 1.5f;
}

void TrailPuff(const Vec3& position, const Vec3& velocity, bool burning)
{
    Particle* p = NewParticle(position, velocity, 2.0f, Look::ALPHA);
    if (!p)
        return;
    p->size0 = 3.0f;
    p->size1 = burning ? 12.0f : 10.0f;
    p->color0 = burning ? 0x99141210 : 0x993A332E;
    p->color1 = burning ? 0x00302C28 : 0x00605850;
    p->frame = FRAME_SMOKE + static_cast<int>(world.rng.Next() % 4);
    p->spin = world.rng.Range(-0.6f, 0.6f);
    p->drag = 1.0f;
    p->lift = 1.0f;
}

void Vapour(const Vec3& position, uint32_t color)
{
    Particle* p = NewParticle(position, Vec3(), 1.5f, Look::ALPHA);
    if (!p)
        return;
    p->size0 = 1.5f;
    p->size1 = 5.0f;
    p->color0 = color;
    p->color1 = color & 0x00FFFFFF;
    p->frame = FRAME_SMOKE + static_cast<int>(world.rng.Next() % 4);
    p->spin = world.rng.Range(-0.5f, 0.5f);
}

void EngineFire(const Vec3& position, const Vec3& velocity, float size)
{
    Particle* p = NewParticle(position, velocity, 0.4f, Look::ADDITIVE);
    if (!p)
        return;
    p->size0 = size;
    p->size1 = size * 0.6f;
    p->color0 = 0xFFFFE6A0;
    p->color1 = 0x00FF5A10;
    p->frame = FRAME_FIRE;
    p->frames = 4;
    p->spin = world.rng.Range(-3.0f, 3.0f);
    p->lift = 2.0f;
}

void PieceSmoke(const Vec3& position)
{
    if (particleCount > BIG_EFFECT_BUDGET)
        return;
    Particle* p = NewParticle(position, Vec3(0.0f, 1.0f, 0.0f), world.rng.Range(2.2f, 2.8f), Look::ALPHA);
    if (!p)
        return;
    p->size0 = 6.0f;
    p->size1 = 18.0f;
    p->color0 = 0xB0141210;
    p->color1 = 0x00403A36;
    p->frame = FRAME_SMOKE + static_cast<int>(world.rng.Next() % 4);
    p->spin = world.rng.Range(-0.6f, 0.6f);
    p->drag = 1.0f;
    p->lift = 1.5f;
}

void ZeppelinSplash(const Vec3& position)
{
    // The column: white water thrown up to heights spread evenly from 8 to 80 m, every part reaching its height at
    // the same moment (so it stands as one column), then falling back into the sea (sunk particles end there).
    for (int i = 0; i < ZEPPELIN_COLUMN; i++) {
        float height = Lerp(8.0f, 80.0f, (i + world.rng.Uniform()) / ZEPPELIN_COLUMN);
        Vec3 v(world.rng.Range(-3.0f, 3.0f), 2.0f * height / COLUMN_RISE, world.rng.Range(-3.0f, 3.0f));
        Vec3 at(position.x + world.rng.Range(-10.0f, 10.0f), 1.0f, position.z + world.rng.Range(-10.0f, 10.0f));
        Particle* p = NewParticle(at, v, 5.0f, Look::ALPHA);
        if (!p)
            break;
        p->size0 = world.rng.Range(18.0f, 24.0f);
        p->size1 = 32.0f;
        p->color0 = 0xF8F4F7F2;
        p->color1 = 0x40E0E8EC;
        p->frame = FRAME_SMOKE + static_cast<int>(world.rng.Next() % 4);
        p->spin = world.rng.Range(-0.4f, 0.4f);
        p->lift = -2.0f * height / (COLUMN_RISE * COLUMN_RISE);
    }
    // An orange blast inside the column: fire only, as dark smoke here would grey the water.
    for (int i = 0; i < 4; i++) {
        Vec3 at = position + Vec3(world.rng.Range(-10.0f, 10.0f), 10.0f + i * 8.0f, world.rng.Range(-10.0f, 10.0f));
        Particle* p = NewParticle(at, Vec3(0.0f, 12.0f, 0.0f), world.rng.Range(0.7f, 1.0f), Look::ADDITIVE);
        if (!p)
            break;
        p->size0 = world.rng.Range(30.0f, 40.0f);
        p->size1 = 55.0f;
        p->color0 = 0xFFFFF6D5;
        p->color1 = 0x00FF6A1A;
        p->frame = FRAME_FIRE;
        p->frames = 4;
        p->spin = world.rng.Range(-1.0f, 1.0f);
        p->drag = 2.5f;
    }
    // A ring of spray racing out low over the water to about 60 m.
    for (int i = 0; i < ZEPPELIN_RING; i++) {
        float a = i * (2.0f * PI / ZEPPELIN_RING) + world.rng.Range(-0.1f, 0.1f);
        Vec3 out(cosf(a), 0.0f, sinf(a));
        Vec3 at = Vec3(position.x, 2.0f, position.z) + out * 15.0f;
        Particle* p = NewParticle(at, out * 45.0f + Vec3(0.0f, 9.0f, 0.0f), world.rng.Range(2.2f, 2.8f), Look::ALPHA);
        if (!p)
            break;
        p->size0 = 8.0f;
        p->size1 = 22.0f;
        p->color0 = 0xE0F4F7F2;
        p->color1 = 0x00E0E8EC;
        p->frame = FRAME_SMOKE + static_cast<int>(world.rng.Next() % 4);
        p->drag = 1.0f;
        p->lift = -6.0f;
    }
    // Mist that hangs over the spot and drifts, and foam left on the water.
    for (int i = 0; i < 5; i++) {
        Vec3 at = position + Vec3(world.rng.Range(-30.0f, 30.0f), world.rng.Range(15.0f, 40.0f),
                                  world.rng.Range(-30.0f, 30.0f));
        Particle* p = NewParticle(at, Vec3(2.5f, 1.5f, 1.0f), world.rng.Range(7.0f, 8.0f), Look::ALPHA);
        if (!p)
            break;
        p->size0 = 40.0f;
        p->size1 = 80.0f;
        p->color0 = 0x90D8DCDF;
        p->color1 = 0x00D8DCDF;
        p->frame = FRAME_SMOKE + i % 4;
        p->spin = world.rng.Range(-0.1f, 0.1f);
    }
    // Foam over the waterline where the hull goes under.
    for (int i = 0; i < ZEPPELIN_FOAM; i++) {
        Vec3 at(position.x + world.rng.Range(-60.0f, 60.0f), 0.8f, position.z + world.rng.Range(-60.0f, 60.0f));
        Particle* p = NewParticle(at, Vec3(), world.rng.Range(10.0f, 14.0f), Look::ALPHA);
        if (!p)
            break;
        p->size0 = world.rng.Range(8.0f, 12.0f);
        p->size1 = 15.0f;
        p->color0 = 0xC0F4F7F2;
        p->color1 = 0x00F4F7F2;
        p->frame = FRAME_SMOKE + static_cast<int>(world.rng.Next() % 4);
        p->spin = world.rng.Range(-0.05f, 0.05f);
    }
    audio::Play3D(audio::Sfx::SPLASH, position, 1.0f, 0.6f);
}

// A big black smoke puff for the columns over the burning zeppelin.
void SmokeColumnPuff(const Vec3& position, const Vec3& velocity)
{
    if (particleCount > BIG_EFFECT_BUDGET)
        return;
    Particle* p = NewParticle(position, velocity + Vec3(0.0f, 10.0f, 0.0f), world.rng.Range(4.0f, 5.5f), Look::ALPHA);
    if (!p)
        return;
    p->size0 = world.rng.Range(20.0f, 30.0f);
    p->size1 = world.rng.Range(45.0f, 60.0f);
    p->color0 = 0xC0201C1A;
    p->color1 = 0x00403A36;
    p->frame = FRAME_SMOKE + static_cast<int>(world.rng.Next() % 4);
    p->spin = world.rng.Range(-0.3f, 0.3f);
    p->drag = 0.6f;
    p->lift = 2.0f;
}

void FirePuff(const Vec3& position, const Vec3& velocity, float size)
{
    if (size > 10.0f && particleCount > BIG_EFFECT_BUDGET)
        return;
    Particle* p = NewParticle(position, velocity, world.rng.Range(0.35f, 0.55f), Look::ADDITIVE);
    if (!p)
        return;
    p->size0 = size;
    p->size1 = size * 0.4f;
    p->color0 = 0xFFFFE6A0;
    p->color1 = 0x00FF5A10;
    p->frame = FRAME_FIRE;
    p->frames = 4;
    p->spin = world.rng.Range(-2.0f, 2.0f);
    p->drag = 2.0f;
    p->lift = 3.0f;
}

void Sparks(const Vec3& position, const Vec3& normal, int count)
{
    for (int i = 0; i < count; i++) {
        Vec3 dir = Normalize(normal * 0.8f + RandomDirection());
        Particle* p = NewParticle(position, dir * world.rng.Range(30.0f, 70.0f), world.rng.Range(0.15f, 0.35f),
                                  Look::STREAK);
        if (!p)
            return;
        p->size0 = 0.25f;
        p->size1 = 0.1f;
        p->color0 = 0xFFFFF0C0;
        p->color1 = 0x00FF9030;
        p->drag = 3.0f;
        p->lift = -20.0f;
    }
    Particle* flash = NewParticle(position, Vec3(), 0.08f, Look::ADDITIVE);
    if (flash) {
        flash->size0 = 4.0f;
        flash->size1 = 2.0f;
        flash->color0 = 0xFFFFF4D0;
        flash->color1 = 0x00FFC060;
        flash->frame = FRAME_FLASH;
    }
}

void MuzzleFlash(const Vec3& position, const Vec3& velocity)
{
    Particle* p = NewParticle(position, velocity, 1.0f / 30.0f, Look::ADDITIVE);
    if (!p)
        return;
    p->size0 = 2.2f;
    p->size1 = 1.6f;
    p->color0 = 0xFFFFF0B0;
    p->color1 = 0x80FFB040;
    p->frame = FRAME_FLASH;
}

// count spinning bananas flung out around velocity; they fall and fade over about 2 s.
static void BananaBurst(const Vec3& position, const Vec3& velocity, int count)
{
    for (int i = 0; i < count; i++) {
        Vec3 v = velocity * 0.5f + RandomDirection() * world.rng.Range(10.0f, 30.0f);
        Particle* p = NewParticle(position, v, world.rng.Range(1.5f, 2.5f), Look::ALPHA);
        if (!p)
            return;
        p->size0 = 1.4f;
        p->size1 = 1.4f;
        p->frame = FRAME_BANANA;
        p->spin = world.rng.Range(-8.0f, 8.0f);
        p->drag = 0.6f;
        p->lift = -9.8f;
        p->color1 = 0x00FFFFFF;
    }
}

// One puff of a debris chunk's smoke trail: 3 m growing to 8 m over 1.5 s, grey, half transparent.
static void DebrisSmoke(const Vec3& position)
{
    Particle* p = NewParticle(position, Vec3(0.0f, 1.0f, 0.0f), 1.5f, Look::ALPHA);
    if (!p)
        return;
    p->size0 = 3.0f;
    p->size1 = 8.0f;
    p->color0 = 0x805A5552;
    p->color1 = 0x005A5552;
    p->frame = FRAME_SMOKE + static_cast<int>(world.rng.Next() % 4);
    p->spin = world.rng.Range(-0.6f, 0.6f);
    p->drag = 1.0f;
}

// Tumbling plane chunks; every second one trails smoke.
static void Debris(const Vec3& position, const Vec3& velocity, int count, float speed)
{
    for (int i = 0; i < count; i++) {
        Vec3 v = velocity * 0.6f + RandomDirection() * world.rng.Range(speed * 0.4f, speed);
        v.y += speed * 0.3f;
        Particle* p = NewParticle(position, v, world.rng.Range(1.5f, 2.0f), Look::ALPHA);
        if (!p)
            return;
        p->size0 = world.rng.Range(1.2f, 2.4f);
        p->size1 = p->size0;
        p->frame = FRAME_DEBRIS + static_cast<int>(world.rng.Next() % 4);
        p->spin = world.rng.Range(-10.0f, 10.0f);
        p->drag = 0.3f;
        p->lift = -12.0f;
        p->color0 = 0xFFE0D8D0;
        p->color1 = 0xC0605850;
        // Every second chunk trails thick grey smoke.
        p->trail = i % 2 == 0 ? DEBRIS_TRAIL_GAP : 0.0f;
    }
}

void Explosion(const Vec3& position, float size, const Vec3& velocity)
{
    // Over budget: only the flash and the sound, so a kill still reads.
    bool busy = particleCount > BIG_EFFECT_BUDGET;
    // Flash: about 2.8 times the fireball, two frames.
    Particle* flash = NewParticle(position, velocity * 0.3f, 2.0f / 30.0f, Look::ADDITIVE);
    if (flash) {
        flash->size0 = size * 2.8f;
        flash->size1 = size * 2.4f;
        flash->color0 = 0xFFFFF6D5;
        flash->color1 = 0xC0FFB23F;
        flash->frame = FRAME_FLASH;
    }
    // Fireball: a few big animated fire puffs that grow by two thirds over 0.6-0.9 s, hot core to orange.
    int fireballs = busy ? 1 : 3 + static_cast<int>(size / 8.0f);
    for (int i = 0; i < fireballs; i++) {
        Vec3 v = velocity * 0.3f + RandomDirection() * (size * 0.6f);
        Particle* p = NewParticle(position + RandomDirection() * (size * 0.15f), v, world.rng.Range(0.6f, 0.9f),
                                  Look::ADDITIVE);
        if (!p)
            break;
        p->size0 = size * world.rng.Range(0.8f, 1.0f);
        p->size1 = size * 1.67f;
        p->color0 = 0xFFFFF6D5;
        p->color1 = 0x00FF6A1A;
        p->frame = FRAME_FIRE;
        p->frames = 4;
        p->spin = world.rng.Range(-1.0f, 1.0f);
        p->drag = 2.5f;
        p->lift = 2.0f;
    }
    // Smoke that rises and grows: dark brown to grey.
    int smokes = busy ? 0 : 2 + static_cast<int>(size / 15.0f);
    for (int i = 0; i < smokes; i++) {
        Vec3 v = velocity * 0.2f + RandomDirection() * (size * 0.3f) + Vec3(0.0f, size * 0.2f, 0.0f);
        Particle* p = NewParticle(position + RandomDirection() * (size * 0.2f), v, world.rng.Range(2.5f, 4.0f),
                                  Look::ALPHA);
        if (!p)
            break;
        p->size0 = size * 0.8f;
        p->size1 = size * 2.5f;
        p->color0 = 0xD05A3A2A;
        p->color1 = 0x008A8580;
        p->frame = FRAME_SMOKE + static_cast<int>(world.rng.Next() % 4);
        p->spin = world.rng.Range(-0.5f, 0.5f);
        p->drag = 1.5f;
        p->lift = 2.5f;
    }
    if (!busy) {
        Sparks(position, Vec3(0, 0.5f, 0), 6 + static_cast<int>(size * 0.4f));
        Debris(position, velocity, size >= 15.0f ? 6 : 3, 12.0f + size);
        BananaBurst(position, velocity, 3);
    }

    // Shake, flash and rumble scale with distance from the camera.
    float distance = Length(position - CurrentCamera().position);
    float near = Clamp(1.0f - distance / (60.0f + size * 8.0f), 0.0f, 1.0f);
    if (near > 0.0f) {
        CameraShake(0.6f * near);
        Rumble(near, near, 400);
    }
    audio::Play3D(size > 30.0f ? audio::Sfx::BIG_EXPLOSION : audio::Sfx::EXPLOSION, position, 1.0f,
                  world.rng.Range(0.9f, 1.1f));
}

void FlakBurst(const Vec3& position)
{
    Particle* flash = NewParticle(position, Vec3(), 0.1f, Look::ADDITIVE);
    if (flash) {
        flash->size0 = 14.0f;
        flash->size1 = 8.0f;
        flash->color0 = 0xFFFFC060;
        flash->color1 = 0x00FF8020;
        flash->frame = FRAME_FLASH;
    }
    for (int i = 0; i < 4; i++) {
        Particle* p = NewParticle(position + RandomDirection() * 3.0f, RandomDirection() * 4.0f, 3.0f, Look::ALPHA);
        if (!p)
            return;
        p->size0 = 9.0f;
        p->size1 = 20.0f;
        p->color0 = 0xE02B2B2B;
        p->color1 = 0x00404040;
        p->frame = FRAME_SMOKE + i;
        p->drag = 1.0f;
        p->lift = 0.3f;
    }
    Sparks(position, Vec3(), 5);
    audio::Play3D(audio::Sfx::EXPLOSION, position, 0.5f, 1.5f);
}

void Splash(const Vec3& position, float size)
{
    int count = 6 + static_cast<int>(size);
    for (int i = 0; i < count; i++) {
        Vec3 v(world.rng.Range(-0.4f, 0.4f) * size, world.rng.Range(1.5f, 3.5f) * size,
               world.rng.Range(-0.4f, 0.4f) * size);
        Particle* p = NewParticle(Vec3(position.x, 0.5f, position.z), v, world.rng.Range(1.2f, 2.2f), Look::ALPHA);
        if (!p)
            return;
        p->size0 = size * 0.8f;
        p->size1 = size * 2.0f;
        p->color0 = 0xF0F4F7F2;
        p->color1 = 0x00DDE8EE;
        p->frame = FRAME_SMOKE + static_cast<int>(world.rng.Next() % 4);
        p->drag = 0.8f;
        p->lift = -GRAVITY * 0.6f;
    }
    audio::Play3D(audio::Sfx::SPLASH, position, Clamp(size / 10.0f, 0.3f, 1.0f));
}

// Fine water spray behind a plane flying low over the sea.
void Spray(const Vec3& position, const Vec3& velocity)
{
    Vec3 v = velocity * 0.3f + Vec3(world.rng.Range(-3, 3), world.rng.Range(3, 7), world.rng.Range(-3, 3));
    Particle* p = NewParticle(Vec3(position.x, 0.3f, position.z), v, world.rng.Range(0.6f, 1.0f), Look::ALPHA);
    if (!p)
        return;
    p->size0 = 2.5f;
    p->size1 = 7.0f;
    p->color0 = 0xC0F4F7F2;
    p->color1 = 0x00F4F7F2;
    p->frame = FRAME_SMOKE + static_cast<int>(world.rng.Next() % 4);
    p->drag = 1.5f;
    p->lift = -6.0f;
}

// White vapour wisps when flying through a cloud.
void CloudWisp(const Vec3& position, const Vec3& velocity)
{
    Vec3 offset = RandomDirection() * 6.0f;
    Particle* p = NewParticle(position + velocity * 0.25f + offset, velocity * 0.85f, 0.5f, Look::ALPHA);
    if (!p)
        return;
    p->size0 = 4.0f;
    p->size1 = 9.0f;
    p->color0 = 0xA0FFFFFF;
    p->color1 = 0x00FFFFFF;
    p->frame = FRAME_SMOKE + static_cast<int>(world.rng.Next() % 4);
}

// A soft glow sprite (fake bloom on lamps, rocket exhausts).
void Glow(const Vec3& position, float size, uint32_t color)
{
    Particle* p = NewParticle(position, Vec3(), 1.2f / 60.0f, Look::ADDITIVE);
    if (!p)
        return;
    p->size0 = size;
    p->size1 = size;
    p->color0 = color;
    p->color1 = color;
    p->frame = FRAME_GLOW;
}

// --- Update and draw

void UpdateEffects(float dt)
{
    int i = 0;
    while (i < particleCount) {
        Particle& p = particles[i];
        p.life -= dt;
        bool sunk = p.lift < 0.0f && p.position.y < 0.0f && p.look != Look::STREAK;
        if (p.life <= 0.0f || sunk) {
            if (sunk && p.trail > 0.0f)
                Splash(p.position, 2.0f);
            particles[i] = particles[--particleCount];
            continue;
        }
        p.velocity *= 1.0f / (1.0f + p.drag * dt);
        p.velocity.y += p.lift * dt;
        p.position += p.velocity * dt;
        p.rotation += p.spin * dt;
        if (p.trail > 0.0f) {
            p.trailTimer -= dt;
            if (p.trailTimer <= 0.0f) {
                p.trailTimer = p.trail;
                DebrisSmoke(p.position);
            }
        }
        i++;
    }
}

void DrawEffects()
{
    const render::Camera& camera = CurrentCamera();
    Vec3 viewDir = QuatForward(camera.rotation);
    // Billboard size that covers MAX_SCREEN of the screen height at depth 1: bigger ones are shrunk (fill rate).
    float maxSize = 2.0f * tanf(camera.fovY * 0.5f) * MAX_SCREEN;
    int alphaCount = 0;
    int addCount = 0;
    int streakCount = 0;
    for (int i = 0; i < particleCount; i++) {
        const Particle& p = particles[i];
        float t = 1.0f - p.life / p.maxLife;
        uint32_t color = LerpColor(p.color0, p.color1, t);
        float size = Lerp(p.size0, p.size1, t);
        if (p.look == Look::STREAK) {
            render::Streak& s = streaks[streakCount++];
            s.from = p.position - p.velocity * 0.04f;
            s.to = p.position;
            s.width = size;
            s.color = color;
            continue;
        }
        float depth = Dot(p.position - camera.position, viewDir);
        if (depth < 0.5f)
            continue;
        size = fminf(size, maxSize * depth);
        render::Billboard* b = nullptr;
        if (p.look == Look::ALPHA) {
            b = &alphaBoards[alphaCount++];
        } else {
            b = &addBoards[addCount++];
        }
        // Alpha particles fade out right in front of the camera (no screen-filling smoke).
        if (p.look == Look::ALPHA) {
            float near = Clamp((Length(p.position - camera.position) - size * 0.5f) / NEAR_FADE, 0.0f, 1.0f);
            uint32_t alpha = static_cast<uint32_t>(((color >> 24) & 0xFF) * near);
            color = (color & 0x00FFFFFF) | (alpha << 24);
        }
        b->position = p.position;
        b->size = size;
        b->rotation = p.rotation;
        b->color = color;
        b->frame = p.frame + (p.frames > 1 ? static_cast<int>(t * p.frames * 0.999f) : 0);
    }

    if (alphaCount > 0)
        render::DrawBillboards(assets.particles, ATLAS_SIZE, ATLAS_SIZE, alphaBoards, alphaCount, render::Blend::ALPHA);
    if (addCount > 0)
        render::DrawBillboards(assets.particles, ATLAS_SIZE, ATLAS_SIZE, addBoards, addCount, render::Blend::ADDITIVE);
    if (streakCount > 0)
        render::DrawStreaks(assets.streak, streaks, streakCount, render::Blend::ADDITIVE);
}

} // namespace game
