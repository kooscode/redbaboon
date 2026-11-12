// The game's shared state and its top-level step and draw: one fixed step of everything in dependency order (player
// input, planes, zeppelin, projectiles, effects, mission script, clouds, camera, HUD), slow motion, and drawing the
// frame (sky, world, screen effects, HUD) interpolated between steps. Also the title screen's island fly-by.
#include "game/world.h"

namespace game {

constexpr float SLOW_MOTION = 0.35f;

World world;

// --- Helpers

void Rumble(float low, float high, int milliseconds)
{
    if (world.titleMode)
        return;
    input::Rumble(low, high, milliseconds);
}

void SetInvertY(bool invert)
{
    world.invertY = invert;
}

bool InvertY()
{
    return world.invertY;
}

// Clears every part of the world for a new mission or the title scene.
static void ResetWorld(bool titleMode, uint32_t seed)
{
    bool invert = world.invertY;
    world = World();
    world.invertY = invert;
    world.titleMode = titleMode;
    world.rng = Rng(seed);
    ClearPlanes();
    ClearProjectiles();
    ClearEffects();
    ClearZeppelin();
    ResetHud();
    ResetCamera();
}

// Steps everything but the player's input by dt of game time (realDt: wall-clock step, for the HUD).
static void StepWorld(float dt, float realDt)
{
    world.time += dt;
    world.introTimer = fmaxf(0.0f, world.introTimer - dt);
    world.shotAt += dt;
    world.revealTimer = fmaxf(0.0f, world.revealTimer - realDt);
    UpdatePlanes(dt);
    UpdateZeppelin(dt);
    UpdateProjectiles(dt);
    UpdateEffects(dt);
    if (!world.titleMode)
        UpdateScript(dt);
    UpdateClouds(dt);
    UpdateCamera(dt);
    UpdateHud(realDt);
    const render::Camera& camera = CurrentCamera();
    audio::SetListener(camera.position, QuatForward(camera.rotation), QuatUp(camera.rotation));
}

static void DrawScene(float alpha)
{
    world.drawAlpha = Clamp(alpha, 0.0f, 1.0f);
    const render::Camera& camera = CurrentCamera();
    render::BeginFrame(camera, WorldEnvironment());
    render::DrawSky();
    DrawWorld(camera);
    DrawScreenEffects(camera);
}

// --- Mission

void StartMission(uint32_t seed)
{
    audio::FadeOutEffects();
    ResetWorld(false, seed);
    ResetScript();
    UpdateCamera(STEP);
    LogEvent("seed %08x", seed);
}

void UpdateMission(const input::State& pad, float dt)
{
    float scale = world.timeScale;
    if (world.slowMo > 0.0f) {
        world.slowMo -= dt;
        scale = SLOW_MOTION;
    }
    float gameDt = dt * scale;
    UpdatePlayerInput(pad, gameDt);
    StepWorld(gameDt, dt);
}

void DrawMission(float alpha)
{
    DrawScene(alpha);
    DrawHud();
}

void PauseMission()
{
    SilenceEngines();
    ClearZeppelinSound();
    audio::FadeOutEffects();
}

void ShowResultsFlyby()
{
    world.titleMode = true;
    world.cinematic = true;
    SilenceEngines();
    audio::FadeOutEffects();
}

void StopMission()
{
    SilenceEngines();
    ClearZeppelin();
    audio::FadeOutEffects();
    audio::SetMusic(audio::Music::NONE);
}

// --- Title

void StartTitle()
{
    ResetWorld(true, FIXED_SEED);
    // A pair of pirates circling the harbour and the zeppelin cruising round the island, for life in the shot.
    for (int i = 0; i < 2; i++) {
        Vec3 at = assets.harbour + Vec3(300.0f * i, 200.0f + 30.0f * i, 400.0f);
        int index = SpawnPlane(i ? PlaneKind::ACE : PlaneKind::FIGHTER, at, Vec3(1.0f, 0.0f, 0.0f), 65.0f);
        if (index > 0) {
            world.planes[index].ai.mode = AiMode::RAID;
            world.planes[index].ai.raidAngle = i * PI;
        }
    }
    SpawnZeppelin();
    world.zeppelin.state = ZeppelinState::CIRCLING;
    world.zeppelin.position = world.zeppelin.circleCenter +
                              Vec3(cosf(world.zeppelin.circleAngle) * world.zeppelin.circleRadius,
                                   world.zeppelin.altitude,
                                   sinf(world.zeppelin.circleAngle) * world.zeppelin.circleRadius);
    world.zeppelin.prevPosition = world.zeppelin.position;
    audio::SetMusic(audio::Music::TITLE);
    UpdateCamera(STEP);
}

void UpdateTitle(float dt)
{
    StepWorld(dt, dt);
}

void DrawTitleScene(float alpha)
{
    DrawScene(alpha);
}

} // namespace game
