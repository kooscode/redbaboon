// Menus and full screens (docs/design.md §8, creative brief §6): loading, the title (logo over the island fly-by,
// PRESS START pulsing), the briefing, the pause menu with a banana cursor, the results with an ink-stamped rank, the
// defeat screen and the attract demo's banner. Short text: a title, then a list or one line.
#include "game/world.h"

#include <stdio.h>

namespace game {

constexpr uint32_t CREAM = 0xFFF4E4C1;
constexpr uint32_t BRASS = 0xFFC9A227;
constexpr uint32_t CRIMSON = 0xFFB3202A;
constexpr uint32_t PANEL = 0x991A1410;    // 60 % ink: the scene stays visible behind panels
constexpr uint32_t DIM = 0x801A1410;
constexpr int RESULTS_ROWS = 7;
constexpr float RESULTS_FIRST_ROW = 0.5f;   // seconds before the first row counts up
constexpr float RESULTS_ROW_GAP = 0.25f;
constexpr float RESULTS_COUNT_TIME = 0.4f;  // each value rolls up from 0 over this long
constexpr float RESULTS_TICK = 0.05f;       // a tick sound this often while a value rolls
constexpr float RESULTS_STAMP_TIME = 0.15f; // the rank stamp's slam

// --- Helpers

static float Width()
{
    return static_cast<float>(render::ScreenWidth());
}

static float Height()
{
    return static_cast<float>(render::ScreenHeight());
}

// A dark panel with brass rules top and bottom.
static void Panel(float x, float y, float w, float h)
{
    render::DrawRect(x, y, w, h, PANEL);
    render::DrawRect(x, y, w, 2.0f, BRASS);
    render::DrawRect(x, y + h - 2.0f, w, 2.0f, BRASS);
}

// Draws a rectangle of content's HUD atlas.
static void DrawAtlas(const content::AtlasRect& r, float x, float y, float w, float h, uint32_t color)
{
    constexpr float INV = 1.0f / 256.0f;
    render::DrawImage(assets.hud, x, y, w, h, r.x * INV, r.y * INV, (r.x + r.w) * INV, (r.y + r.h) * INV, color,
                      render::Blend::ALPHA);
}

// The banana menu cursor, bobbing.
static void BananaCursor(float x, float y, float time)
{
    float bob = sinf(time * 6.0f) * 2.0f;
    DrawAtlas(content::HUD_BANANA, x, y + bob - 8.0f, 32.0f, 32.0f, 0xFFFFFFFF);
}

static void Menu(const char* const* items, int count, int selection, float top, float time)
{
    float cx = Width() * 0.5f;
    for (int i = 0; i < count; i++) {
        float y = top + i * 32.0f;
        uint32_t color = i == selection ? CREAM : 0xFFA89A80;
        HudTextCentered(cx, y, 2, color, items[i]);
        if (i == selection)
            BananaCursor(cx - render::TextWidth(2.0f, items[i]) * 0.5f - 34.0f, y, time);
    }
}

static void Logo(float y, float alpha)
{
    float w = 512.0f;
    float h = 128.0f;
    uint32_t a = static_cast<uint32_t>(Clamp(alpha, 0.0f, 1.0f) * 255.0f);
    render::DrawImage(assets.title, (Width() - w) * 0.5f, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, (a << 24) | 0x00FFFFFF,
                      render::Blend::ALPHA);
}

// --- Screens

void DrawLoading(const char* what, float fraction)
{
    render::Camera camera;
    render::Environment env = WorldEnvironment();
    render::BeginFrame(camera, env);
    render::DrawSky();
    float w = Width();
    float h = Height();
    render::DrawRect(0.0f, 0.0f, w, h, 0xC81A1410);
    if (assets.title != render::NO_TEXTURE)
        Logo(h * 0.14f, 1.0f);
    else
        HudTextCentered(w * 0.5f, h * 0.25f, 4, CRIMSON, "RED BABOON");
    // A brass bar with the propeller spinning at its end.
    float barW = 320.0f;
    float x = (w - barW) * 0.5f;
    float y = h * 0.62f;
    float done = Clamp(fraction, 0.0f, 1.0f);
    render::DrawRect(x - 3.0f, y - 3.0f, barW + 6.0f, 14.0f, BRASS);
    render::DrawRect(x - 1.0f, y - 1.0f, barW + 2.0f, 10.0f, 0xFF1A1410);
    render::DrawRect(x, y, barW * done, 8.0f, 0xFFFFD23F);
    float px = x + barW * done;
    float py = y + 4.0f;
    float spin = fraction * 40.0f;
    for (int blade = 0; blade < 3; blade++) {
        float a = spin + blade * (2.0f * PI / 3.0f);
        for (int d = 2; d < 12; d++)
            render::DrawRect(px + cosf(a) * d - 1.5f, py + sinf(a) * d - 1.5f, 3.0f, 3.0f, CREAM);
    }
    render::DrawRect(px - 3.0f, py - 3.0f, 6.0f, 6.0f, BRASS);
    HudTextCentered(w * 0.5f, y + 24.0f, 2, CREAM, what);
    HudTextCentered(w * 0.5f, h - 64.0f, 1, 0xFFA89A80, "TIP: BARREL ROLLS DODGE BULLETS");
}

void DrawTitleScreen(float time)
{
    float alpha = Clamp(time * 0.8f, 0.0f, 1.0f);
    Logo(Height() * 0.16f, alpha);
    // PRESS START pulses at 0.8 Hz.
    float pulse = 0.55f + 0.45f * sinf(time * 2.0f * PI * 0.8f);
    uint32_t a = static_cast<uint32_t>(pulse * alpha * 255.0f);
    HudTextCentered(Width() * 0.5f, Height() * 0.72f, 2, (a << 24) | (CREAM & 0x00FFFFFF), "PRESS START");
}

// A button prompt centred on cx: words, the button glyph, words ("PRESS (A) TO FLY"), gently pulsing.
static void Prompt(float cx, float y, int scale, const char* before, char button, const char* after, float time)
{
    float pulse = 0.5f + 0.5f * sinf(time * 2.0f * PI * 0.8f);
    float glyph = 10.0f * scale + 4.0f + pulse * 3.0f;
    float gap = 4.0f * scale;
    float beforeWidth = static_cast<float>(render::TextWidth(static_cast<float>(scale), before));
    float afterWidth = static_cast<float>(render::TextWidth(static_cast<float>(scale), after));
    float total = beforeWidth + gap + glyph + gap + afterWidth;
    float x = cx - total * 0.5f;
    uint32_t color = PackARGB(1.0f, 0.96f - 0.1f * pulse, 0.76f - 0.25f * pulse);
    HudText(x, y, scale, color, before);
    DrawButtonGlyph(button, x + beforeWidth + gap + glyph * 0.5f, y + 4.0f * scale, glyph);
    HudText(x + beforeWidth + gap * 2.0f + glyph, y, scale, color, after);
}

void DrawBriefing(float time)
{
    float w = Width();
    float h = Height();
    render::DrawRect(0.0f, 0.0f, w, h, 0xB81A1410);
    HudTextCentered(w * 0.5f, 22.0f, 3, BRASS, "RAID ON ISLA PLATANO");
    HudTextCentered(w * 0.5f, 52.0f, 1, CREAM, "DOWN THE PIRATES - SINK THE ZEPPELIN");
    DrawControls(72.0f, h - 72.0f - 76.0f);
    if (time > 0.3f)
        Prompt(w * 0.5f, h - 58.0f, 3, "PRESS", 'A', "TO FLY", time);
}

void DrawControlsScreen(float time)
{
    float w = Width();
    float h = Height();
    render::DrawRect(0.0f, 0.0f, w, h, 0xD81A1410);
    HudTextCentered(w * 0.5f, 26.0f, 3, BRASS, "CONTROLS");
    DrawControls(64.0f, h - 64.0f - 64.0f);
    Prompt(w * 0.5f, h - 50.0f, 2, "", 'B', "BACK", time);
}

void DrawPauseMenu(int selection)
{
    static const char* const ITEMS_NORMAL[PAUSE_ITEMS] = {"RESUME", "INVERT Y: OFF", "CONTROLS", "RESTART", "QUIT"};
    static const char* const ITEMS_INVERTED[PAUSE_ITEMS] = {"RESUME", "INVERT Y: ON", "CONTROLS", "RESTART", "QUIT"};
    float w = Width();
    float h = Height();
    render::DrawRect(0.0f, 0.0f, w, h, DIM);
    Panel(w * 0.5f - 160.0f, h * 0.22f, 320.0f, 240.0f);
    HudTextCentered(w * 0.5f, h * 0.22f + 18.0f, 3, BRASS, "PAUSED");
    Menu(world.invertY ? ITEMS_INVERTED : ITEMS_NORMAL, PAUSE_ITEMS, selection, h * 0.22f + 64.0f, world.time);
}

// Seconds after the results appear at which a row starts counting up.
static float RowStart(int row)
{
    return RESULTS_FIRST_ROW + row * RESULTS_ROW_GAP;
}

static float StampStart()
{
    return RowStart(RESULTS_ROWS) + 0.3f;
}

// A value counting up from 0 over RESULTS_COUNT_TIME seconds, starting at `start`.
static int CountUp(int value, float time, float start)
{
    float t = Clamp((time - start) / RESULTS_COUNT_TIME, 0.0f, 1.0f);
    return static_cast<int>(value * t + 0.5f);
}

void UpdateResults(float time, float dt)
{
    // A tick for every step of the count-up, and a thud when the rank stamp lands.
    float before = time - dt;
    for (int row = 0; row < RESULTS_ROWS; row++) {
        float start = RowStart(row);
        if (time < start || before > start + RESULTS_COUNT_TIME)
            continue;
        if (static_cast<int>((time - start) / RESULTS_TICK) != static_cast<int>((before - start) / RESULTS_TICK) ||
            before < start)
            audio::Play(audio::Sfx::MENU_MOVE, 0.5f, 0.0f, 1.0f + row * 0.05f);
    }
    float stamp = StampStart() + RESULTS_STAMP_TIME;
    if (before < stamp && time >= stamp) {
        audio::Play(audio::Sfx::EXPLOSION, 0.8f, 0.0f, 0.6f);
        audio::Play(audio::Sfx::MONKEY_CHEER, 0.8f);
    }
}

void DrawResults(const Results& results, float time)
{
    float w = Width();
    float h = Height();
    // The stamp's landing shakes the panel for a moment.
    float sinceStamp = time - StampStart() - RESULTS_STAMP_TIME;
    float shake = sinceStamp > 0.0f && sinceStamp < 0.25f ? (1.0f - sinceStamp / 0.25f) * 4.0f : 0.0f;
    float sx = shake * sinf(time * 90.0f);
    float sy = shake * cosf(time * 70.0f);
    float panelW = 400.0f;
    float px = (w - panelW) * 0.5f + sx;
    float py = 40.0f + sy;
    Panel(px, py, panelW, h - 80.0f);
    HudTextCentered(w * 0.5f + sx, py + 14.0f, 3, BRASS, "MISSION COMPLETE");
    // The pilot and his line.
    DrawAtlas(content::HUD_PORTRAIT_BABOON, px + 16.0f, py + 50.0f, 40.0f, 40.0f, 0xFFFFFFFF);
    HudText(px + 64.0f, py + 64.0f, 1, CREAM, "\"OOK OOK! ISLA PLATANO IS FREE!\"");

    int accuracy = results.shotsFired > 0 ? results.shotsHit * 100 / results.shotsFired : 0;
    int seconds = static_cast<int>(results.time);
    char rows[RESULTS_ROWS][48];
    snprintf(rows[0], sizeof(rows[0]), "KILLS      %d", CountUp(results.kills, time, RowStart(0)));
    int rings = CountUp(results.rings, time, RowStart(1));
    snprintf(rows[1], sizeof(rows[1]), "RINGS      %d/%d", rings, results.ringCount);
    snprintf(rows[2], sizeof(rows[2]), "STUNTS     %d", CountUp(results.stunts, time, RowStart(2)));
    snprintf(rows[3], sizeof(rows[3]), "ACCURACY   %d%%", CountUp(accuracy, time, RowStart(3)));
    int counted = CountUp(seconds, time, RowStart(4));
    snprintf(rows[4], sizeof(rows[4]), "TIME       %d:%02d", counted / 60, counted % 60);
    snprintf(rows[5], sizeof(rows[5]), "BONUS      %d", CountUp(results.timeBonus, time, RowStart(5)));
    snprintf(rows[6], sizeof(rows[6]), "SCORE      %d", CountUp(results.score, time, RowStart(6)));
    for (int i = 0; i < RESULTS_ROWS; i++) {
        if (time < RowStart(i))
            break;
        HudText(px + 48.0f, py + 104.0f + i * 24.0f, 2, i == RESULTS_ROWS - 1 ? 0xFFFFD23F : CREAM, rows[i]);
    }

    // The rank stamp slams down: from 1.6 times its size to full over 0.15 s (integer text scales 6, 5, 4).
    float stampTime = time - StampStart();
    if (stampTime < 0.0f)
        return;
    float t = Clamp(stampTime / RESULTS_STAMP_TIME, 0.0f, 1.0f);
    int scale = t < 0.34f ? 6 : (t < 0.67f ? 5 : 4);
    const char* rank = RankName(results.rank);
    float tw = static_cast<float>(render::TextWidth(static_cast<float>(scale), rank));
    float cx = w * 0.5f + sx;
    float y = py + 104.0f + RESULTS_ROWS * 24.0f + 24.0f;
    float pad = 10.0f;
    DrawAtlas(content::HUD_STAMP_FRAME, cx - tw * 0.5f - pad * 2.0f, y - pad * 1.5f, tw + pad * 4.0f,
              8.0f * scale + pad * 3.0f, 0xFFFFFFFF);
    HudTextCentered(cx, y, scale, CRIMSON, rank);
    if (stampTime > 1.0f && fmodf(time, 1.0f) < 0.7f)
        Prompt(w * 0.5f, py + h - 80.0f - 30.0f, 2, "", 'A', "CONTINUE", time);
}

void DrawDefeat(const Results& results, int selection, float time)
{
    static const char* const ITEMS[DEFEAT_ITEMS] = {"RETRY", "QUIT"};
    float w = Width();
    float h = Height();
    render::DrawRect(0.0f, 0.0f, w, h, DIM);
    Panel(w * 0.5f - 180.0f, h * 0.25f, 360.0f, 220.0f);
    HudTextCentered(w * 0.5f, h * 0.25f + 20.0f, 3, CRIMSON, "SHOT DOWN!");
    char line[48];
    snprintf(line, sizeof(line), "SCORE %d", results.score);
    HudTextCentered(w * 0.5f, h * 0.25f + 64.0f, 2, CREAM, line);
    Menu(ITEMS, DEFEAT_ITEMS, selection, h * 0.25f + 120.0f, time);
}

void DrawDemoBanner(float time)
{
    if (fmodf(time, 1.6f) < 1.1f)
        HudTextCentered(Width() * 0.5f, Height() - 48.0f, 2, CREAM, "DEMO - PRESS START");
}

} // namespace game
