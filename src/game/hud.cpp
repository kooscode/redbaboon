// The HUD (docs/design.md §8, creative brief §6): score and lives, a brass-ringed radar, radio messages with a
// portrait that slide in, the gun reticle with a heat arc, the target bracket with a health bar, a lead reticle and
// an off-screen arrow, bars for damage and boost, rockets, the objective line, hit markers, score popups,
// kill banners that scale in, the red damage vignette and the low-health warning. Cream text with an ink shadow,
// integer text scales, everything inside the title-safe area. The art is content's HUD atlas, plus the
// harbourmaster's portrait drawn here at start-up.
#include "game/world.h"

#include "game/canvas.h"

#include <stdio.h>
#include <string.h>

namespace game {

constexpr uint32_t CREAM = 0xFFF4E4C1;
constexpr uint32_t BRASS = 0xFFC9A227;
constexpr uint32_t CRIMSON = 0xFFB3202A;
constexpr uint32_t INK = 0xFF1A1410;
constexpr uint32_t ALERT = 0xFFFF3B2F;
constexpr float SAFE_X = 32.0f;
constexpr float SAFE_Y = 24.0f;
constexpr float RADAR_SIZE = 96.0f;
constexpr float RADAR_RANGE = 2200.0f;
constexpr float RADAR_FACE = 0.36f;      // radius of the radar's glass face, as a fraction of its size
constexpr float BAR_WIDTH = 132.0f;     // the HULL and BOOST bars (content's frame art is 96 x 16)
constexpr float BAR_HEIGHT = 16.0f;
constexpr float BAR_LABEL_WIDTH = 96.0f; // room for the label (scale 2) to the bar's left
constexpr int MAX_POPUPS = 6;
constexpr int POPUPS_SHOWN = 2;
constexpr float POPUP_PITCH = 20.0f;   // line pitch of stacked popups (scale-2 text is 16 px tall)
constexpr float POPUP_TIME = 1.2f;
constexpr float SLIDE_TIME = 0.15f;
constexpr float CRITICAL_TEXT_TIME = 3.0f;
constexpr int VIGNETTE_SIZE = 128;
constexpr int RADIO_LINE = 34;          // characters per radio line
constexpr float METER_PULSE_TIME = 0.5f;
constexpr float TIP_TIME = 5.0f;        // seconds a tip stays on screen

// First-mission tips, in the order they usually come up.
enum Tip {
    TIP_THROTTLE,
    TIP_STUNTS,
    TIP_CRATE,
    TIP_ROCKETS,
    TIP_COUNT,
};

const char* const TIP_TEXT[TIP_COUNT] = {
    "R-STICK UP: BOOST  DOWN: BRAKE",
    "A + STICK: LOOP / ROLL / 180",
    "FLY THROUGH CRATES TO REPAIR",
    "LT: ROCKETS",
};

// The harbourmaster's portrait, drawn here (content's HUD atlas has the baboon and the gorilla).
constexpr int PORTRAIT_SIZE = 128;

struct Popup {
    char text[40];
    uint32_t color;
    float time;
};

struct HudState {
    Popup popups[MAX_POPUPS];
    int popupCount = 0;
    char banner[24] = {};
    uint32_t bannerColor = CREAM;
    float bannerTime = 99.0f;
    float bannerHold = 1.0f;   // seconds the banner stays
    float hitMarker = 0.0f;  // seconds left
    float damage = 0.0f;     // vignette strength 0..1
    float flash = 0.0f;      // white flash 0..1
    float warningTimer = 0.0f;
    float criticalTime = 0.0f; // seconds the hull has been below 25 %
    float meterPulse = 0.0f;   // seconds left of the boost bar's refused flash
    int tip = -1;              // the tip on screen (-1: none) and how long it has shown
    float tipTime = 0.0f;
    float sinceIntro = 0.0f;   // seconds since the intro ended
    float time = 0.0f;
    const RadioMessage* lastRadio = nullptr;
    const char* lastRadioText = nullptr;
};

// HUD state.
static HudState hud;

// Tips already shown since power-on (each is shown once), and tips waiting for the one on screen to go.
static bool tipShown[TIP_COUNT] = {};
static bool tipQueued[TIP_COUNT] = {};

// The harbourmaster's portrait texture, and the damage vignette (white, alpha rising toward the corners).
static render::TextureId harbourPortrait = render::NO_TEXTURE;
static render::TextureId vignette = render::NO_TEXTURE;

// --- Art

// The harbourmaster's portrait (an old grey monkey in a navy cap, in a brass rim like content's portraits) and the
// damage vignette.
void CreateHudArt()
{
    Canvas c;
    c.image.width = PORTRAIT_SIZE;
    c.image.height = PORTRAIT_SIZE;
    c.image.pixels.assign(PORTRAIT_SIZE * PORTRAIT_SIZE, 0x00000000);
    // Drawn on a 64-pixel layout, scaled up (textures under 128 pixels do not show on the Xbox renderer).
    constexpr float K = PORTRAIT_SIZE / 64.0f;
    c.Ellipse(32 * K, 32 * K, 31 * K, 31 * K, 0xFF2A4A5A);
    c.Ellipse(32 * K, 36 * K, 22 * K, 23 * K, 0xFF9A8A78);
    c.Ellipse(32 * K, 44 * K, 12 * K, 12 * K, 0xFFE0B8A0);
    c.Ellipse(25 * K, 34 * K, 3 * K, 3 * K, INK);
    c.Ellipse(39 * K, 34 * K, 3 * K, 3 * K, INK);
    c.Ellipse(32 * K, 17 * K, 23 * K, 9 * K, 0xFF1F3A5A);
    c.Line(12 * K, 23 * K, 52 * K, 23 * K, 3.0f * K, 0xFF10203A);
    c.Line(26 * K, 52 * K, 38 * K, 52 * K, 4.0f * K, 0xFFE8E8E8);
    c.Ring(28.0f * K, 31.5f * K, BRASS);
    harbourPortrait = render::CreateTexture(c.image, false, false);

    ImageData edge;
    edge.width = VIGNETTE_SIZE;
    edge.height = VIGNETTE_SIZE;
    edge.pixels.resize(VIGNETTE_SIZE * VIGNETTE_SIZE);
    for (int y = 0; y < VIGNETTE_SIZE; y++) {
        for (int x = 0; x < VIGNETTE_SIZE; x++) {
            float dx = (x + 0.5f) / VIGNETTE_SIZE * 2.0f - 1.0f;
            float dy = (y + 0.5f) / VIGNETTE_SIZE * 2.0f - 1.0f;
            float r = sqrtf((dx * dx + dy * dy) * 0.5f);
            float t = Clamp((r - 0.7f) / 0.3f, 0.0f, 1.0f);
            edge.pixels[y * VIGNETTE_SIZE + x] = PackARGB(1.0f, 1.0f, 1.0f, t * t * (3.0f - 2.0f * t));
        }
    }
    vignette = render::CreateTexture(edge, false, false);
}

// Draws a rectangle of content's HUD atlas; flipX/flipY mirror it.
static void DrawAtlas(const content::AtlasRect& r, float x, float y, float w, float h, uint32_t color,
                      bool flipX = false, bool flipY = false)
{
    constexpr float INV = 1.0f / 256.0f;
    float u0 = r.x * INV;
    float v0 = r.y * INV;
    float u1 = (r.x + r.w) * INV;
    float v1 = (r.y + r.h) * INV;
    if (flipX) {
        float t = u0;
        u0 = u1;
        u1 = t;
    }
    if (flipY) {
        float t = v0;
        v0 = v1;
        v1 = t;
    }
    render::DrawImage(assets.hud, x, y, w, h, u0, v0, u1, v1, color, render::Blend::ALPHA);
}

static void DrawPortrait(Speaker speaker, float x, float y, float size, uint32_t color)
{
    if (speaker == Speaker::HARBOUR) {
        render::DrawImage(harbourPortrait, x, y, size, size, 0.0f, 0.0f, 1.0f, 1.0f, color, render::Blend::ALPHA);
        return;
    }
    const content::AtlasRect& r = speaker == Speaker::RED ? content::HUD_PORTRAIT_BABOON
                                                          : content::HUD_PORTRAIT_GORILLA;
    DrawAtlas(r, x, y, size, size, color);
}

// --- Text

// Cream (or coloured) text with an ink drop shadow, at an integer scale.
void HudText(float x, float y, int scale, uint32_t color, const char* text)
{
    // Whole pixels (truncation: positions are on screen, so never negative here).
    float px = static_cast<float>(static_cast<int>(x));
    float py = static_cast<float>(static_cast<int>(y));
    render::DrawTextShadow(px, py, static_cast<float>(scale), color, text);
}

void HudTextCentered(float cx, float y, int scale, uint32_t color, const char* text)
{
    HudText(cx - render::TextWidth(static_cast<float>(scale), text) * 0.5f, y, scale, color, text);
}

static uint32_t WithAlpha(uint32_t color, float alpha)
{
    uint32_t a = static_cast<uint32_t>(Clamp(alpha, 0.0f, 1.0f) * ((color >> 24) & 0xFF));
    return (color & 0x00FFFFFF) | (a << 24);
}

// --- Events

// Asks for a tip: it shows when nothing else is showing, once per power-on.
static void ShowTip(int tip)
{
    if (!tipShown[tip] && !world.titleMode)
        tipQueued[tip] = true;
}

void ResetHud()
{
    hud = HudState();
}

void HudHitMarker()
{
    hud.hitMarker = 0.15f;
}

void HudDamage(float amount)
{
    hud.damage = Clamp(hud.damage + amount * 0.08f, 0.0f, 1.0f);
}

void HudMeterRefused()
{
    if (hud.meterPulse <= 0.0f)
        hud.meterPulse = METER_PULSE_TIME;
}

void HudCrateTip()
{
    ShowTip(TIP_CRATE);
}

void HudFlash(float amount)
{
    hud.flash = fmaxf(hud.flash, amount);
}

void HudBanner(const char* text, uint32_t color, float seconds)
{
    snprintf(hud.banner, sizeof(hud.banner), "%s", text);
    hud.bannerColor = color;
    hud.bannerTime = 0.0f;
    hud.bannerHold = seconds;
}

void HudPopup(const char* text, uint32_t color)
{
    if (hud.popupCount == MAX_POPUPS) {
        for (int i = 1; i < MAX_POPUPS; i++)
            hud.popups[i - 1] = hud.popups[i];
        hud.popupCount--;
    }
    Popup& p = hud.popups[hud.popupCount++];
    snprintf(p.text, sizeof(p.text), "%s", text);
    p.color = color;
    p.time = 0.0f;
}

// Notices the moments for the tips and moves the queue on.
static void UpdateTips(float dt)
{
    const Plane& p = Player();
    hud.sinceIntro = world.introTimer > 0.0f ? 0.0f : hud.sinceIntro + dt;
    if (p.alive && hud.sinceIntro > 1.0f)
        ShowTip(TIP_THROTTLE);
    if (p.alive && p.flight.meter >= 0.99f && tipShown[TIP_THROTTLE])
        ShowTip(TIP_STUNTS);
    if (p.alive && p.rockets > 0 && TargetValid(world.target) && tipShown[TIP_THROTTLE])
        ShowTip(TIP_ROCKETS);
    if (hud.tip >= 0) {
        hud.tipTime += dt;
        if (hud.tipTime > TIP_TIME)
            hud.tip = -1;
        return;
    }
    for (int i = 0; i < TIP_COUNT; i++) {
        if (tipQueued[i] && !tipShown[i]) {
            tipShown[i] = true;
            tipQueued[i] = false;
            hud.tip = i;
            hud.tipTime = 0.0f;
            return;
        }
    }
}

// The tip on screen: one short line at the bottom centre, just above the bars and the rocket count, sliding up.
static void DrawTip()
{
    if (hud.tip < 0)
        return;
    const char* text = TIP_TEXT[hud.tip];
    float w = static_cast<float>(render::ScreenWidth());
    float h = static_cast<float>(render::ScreenHeight());
    float t = Clamp(hud.tipTime / SLIDE_TIME, 0.0f, 1.0f);
    float fade = Clamp((TIP_TIME - hud.tipTime) / 0.4f, 0.0f, 1.0f);
    float tw = static_cast<float>(render::TextWidth(1.0f, text));
    float y = h - SAFE_Y - 70.0f + (1.0f - t) * 12.0f;
    render::DrawRect(w * 0.5f - tw * 0.5f - 8.0f, y - 5.0f, tw + 16.0f, 18.0f, WithAlpha(0xB01A1410, fade));
    render::DrawRect(w * 0.5f - tw * 0.5f - 8.0f, y - 5.0f, 2.0f, 18.0f, WithAlpha(BRASS, fade));
    HudTextCentered(w * 0.5f, y, 1, WithAlpha(CREAM, fade), text);
}

void UpdateHud(float dt)
{
    hud.time += dt;
    hud.hitMarker -= dt;
    hud.damage = fmaxf(0.0f, hud.damage - dt * 1.5f);
    const Plane& player = Player();
    bool critical = player.alive && player.health < player.maxHealth * 0.25f;
    hud.criticalTime = critical ? hud.criticalTime + dt : 0.0f;
    hud.meterPulse = fmaxf(0.0f, hud.meterPulse - dt);
    UpdateTips(dt);
    hud.flash = fmaxf(0.0f, hud.flash - dt * 8.0f);
    hud.bannerTime += dt;
    int kept = 0;
    for (int i = 0; i < hud.popupCount; i++) {
        if (i < POPUPS_SHOWN)
            hud.popups[i].time += dt;
        if (hud.popups[i].time < POPUP_TIME)
            hud.popups[kept++] = hud.popups[i];
    }
    hud.popupCount = kept;
    const Plane& p = Player();
    hud.warningTimer -= dt;
    if (p.alive && p.health < p.maxHealth * 0.25f && hud.warningTimer <= 0.0f && !world.titleMode) {
        hud.warningTimer = 1.0f;
        audio::Play(audio::Sfx::WARNING, 0.5f);
    }
}

// --- Pieces

// A bar's colour for a fill of 0..1: red when empty, yellow at half, green when full (a smooth blend).
static uint32_t BarColor(float value)
{
    constexpr Vec3 RED = Vec3(0.85f, 0.22f, 0.18f);
    constexpr Vec3 YELLOW = Vec3(0.91f, 0.78f, 0.23f);
    constexpr Vec3 GREEN = Vec3(0.36f, 0.76f, 0.24f);
    float t = Clamp(value, 0.0f, 1.0f);
    Vec3 c = t < 0.5f ? Lerp(RED, YELLOW, t * 2.0f) : Lerp(YELLOW, GREEN, (t - 0.5f) * 2.0f);
    return PackARGB(c.x, c.y, c.z);
}

// A labelled horizontal bar in the brass frame: full and green at 1, shrinking toward red at 0. low: the fill
// pulses (a low state); flash 0..1: the frame flashes red and swells (a refused boost or stunt).
static void DrawBar(float x, float y, float value, const char* label, bool low, float flash)
{
    constexpr float FRAME_INSET = 3.0f;
    float grow = flash * 3.0f;
    HudText(x, y, 2, CREAM, label);
    float bx = x + BAR_LABEL_WIDTH;
    uint32_t frame = flash > 0.0f ? PackARGB(1.0f, 1.0f - flash * 0.7f, 1.0f - flash * 0.7f) : 0xFFFFFFFF;
    render::DrawRect(bx + FRAME_INSET, y + FRAME_INSET, BAR_WIDTH - FRAME_INSET * 2.0f,
                     BAR_HEIGHT - FRAME_INSET * 2.0f, 0xC01A1410);
    float fill = Clamp(value, 0.0f, 1.0f) * (BAR_WIDTH - FRAME_INSET * 2.0f);
    uint32_t color = BarColor(value);
    if (low)
        color = WithAlpha(color, 0.55f + 0.45f * sinf(hud.time * 2.0f * PI * 1.2f));
    if (fill > 0.5f)
        DrawAtlas(content::HUD_BAR_FILL, bx + FRAME_INSET, y + FRAME_INSET, fill, BAR_HEIGHT - FRAME_INSET * 2.0f,
                  color);
    DrawAtlas(content::HUD_BAR_FRAME, bx - grow, y - grow, BAR_WIDTH + grow * 2.0f, BAR_HEIGHT + grow * 2.0f, frame);
}

// Gun heat as an arc of dots under the reticle.
static void DrawHeatArc(float cx, float cy, const Plane& p)
{
    if (p.gunHeat <= 0.01f)
        return;
    bool locked = p.overheat > 0.0f;
    bool blink = locked && fmodf(hud.time, 0.3f) < 0.15f;
    constexpr int DOTS = 18;
    for (int i = 0; i < DOTS; i++) {
        float t = (i + 0.5f) / DOTS;
        if (t > p.gunHeat)
            break;
        float a = (200.0f + t * 140.0f) * DEG;
        float x = cx + cosf(a) * 24.0f;
        float y = cy - sinf(a) * 24.0f;
        uint32_t color = t < 0.6f ? CREAM : (t < 0.85f ? 0xFFFFB23F : ALERT);
        if (blink)
            color = ALERT;
        render::DrawRect(x - 1.5f, y - 1.5f, 3.0f, 3.0f, color);
    }
}

static void DrawReticle(const Plane& p)
{
    // Boresight: where the guns point, 300 m ahead.
    float sx = render::ScreenWidth() * 0.5f;
    float sy = render::ScreenHeight() * 0.5f;
    Vec3 ahead = p.flight.position + QuatForward(p.flight.rotation) * 300.0f;
    float px = 0.0f;
    float py = 0.0f;
    if (render::Project(ahead, px, py)) {
        sx = px;
        sy = py;
    }
    DrawAtlas(content::HUD_RETICLE, sx - 24.0f, sy - 24.0f, 48.0f, 48.0f, 0xE0FFFFFF);
    DrawHeatArc(sx, sy, p);
    if (hud.hitMarker > 0.0f)
        DrawAtlas(content::HUD_HIT_MARKER, sx - 16.0f, sy - 16.0f, 32.0f, 32.0f, 0xFFFFFFFF);
}

// A filled triangle, drawn as one horizontal span per pixel row.
static void FillTriangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color)
{
    float top = fminf(y0, fminf(y1, y2));
    float bottom = fmaxf(y0, fmaxf(y1, y2));
    const float xs[3] = {x0, x1, x2};
    const float ys[3] = {y0, y1, y2};
    for (float y = top + 0.5f; y < bottom; y += 1.0f) {
        float left = 1e9f;
        float right = -1e9f;
        for (int e = 0; e < 3; e++) {
            float ax = xs[e];
            float ay = ys[e];
            float bx = xs[(e + 1) % 3];
            float by = ys[(e + 1) % 3];
            if ((y < ay) == (y < by))
                continue;
            float x = ax + (bx - ax) * (y - ay) / (by - ay);
            left = fminf(left, x);
            right = fmaxf(right, x);
        }
        if (right > left)
            render::DrawRect(left, y - 0.5f, right - left, 1.0f, color);
    }
}

// The off-screen target arrow: a pulsing crimson triangle with a brass outline on an ellipse at 80 % of the safe
// area, pointing toward the target, with its distance under it.
static void DrawEdgeArrow(float dx, float dy, float distance)
{
    constexpr float SIZE = 18.0f;
    float w = static_cast<float>(render::ScreenWidth());
    float h = static_cast<float>(render::ScreenHeight());
    float cx = w * 0.5f;
    float cy = h * 0.5f;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-3f)
        return;
    dx /= len;
    dy /= len;
    float ax = cx + dx * (cx - SAFE_X) * 0.8f;
    float ay = cy + dy * (cy - SAFE_Y) * 0.8f;
    float pulse = 1.0f + 0.15f * sinf(hud.time * 8.0f);
    float s = SIZE * pulse;
    // Tip toward the target, base behind it.
    float tipX = ax + dx * s * 0.6f;
    float tipY = ay + dy * s * 0.6f;
    float baseX = ax - dx * s * 0.4f;
    float baseY = ay - dy * s * 0.4f;
    float sideX = -dy * s * 0.5f;
    float sideY = dx * s * 0.5f;
    // The brass outline: the same triangle, 2 px larger all round.
    float o = 2.0f;
    float ox = dx * o;
    float oy = dy * o;
    FillTriangle(tipX + ox * 1.5f, tipY + oy * 1.5f, baseX + sideX * 1.25f - ox, baseY + sideY * 1.25f - oy,
                 baseX - sideX * 1.25f - ox, baseY - sideY * 1.25f - oy, BRASS);
    FillTriangle(tipX, tipY, baseX + sideX, baseY + sideY, baseX - sideX, baseY - sideY, CRIMSON);
    char text[16];
    snprintf(text, sizeof(text), "%dm", static_cast<int>(distance));
    HudTextCentered(ax, ay + s * 0.7f + 2.0f, 1, CREAM, text);
}

// Open corner brackets 6 px outside the target's projected size (never over its pixels), its health, name and
// distance below, and the hollow lead reticle.
static void DrawTarget(const render::Camera& camera, const Plane& p)
{
    if (!TargetValid(world.target))
        return;
    Vec3 target = TargetPosition(world.target);
    float distance = Length(target - p.flight.position);
    float sx = 0.0f;
    float sy = 0.0f;
    bool front = render::Project(target, sx, sy);
    float w = static_cast<float>(render::ScreenWidth());
    float h = static_cast<float>(render::ScreenHeight());
    bool onScreen = front && sx > SAFE_X && sx < w - SAFE_X && sy > SAFE_Y && sy < h - SAFE_Y;
    if (!onScreen) {
        Vec3 local = Rotate(QuatConjugate(camera.rotation), target - camera.position);
        DrawEdgeArrow(local.x, -local.y, distance);
        return;
    }
    // Half-size of the target on screen: its radius over the depth, at the camera's focal length.
    Vec3 rel = Rotate(QuatConjugate(camera.rotation), target - camera.position);
    float focal = h * 0.5f / tanf(camera.fovY * 0.5f);
    float half = TargetRadius(world.target) * focal / fmaxf(-rel.z, 1.0f);
    half = fmaxf(half, 7.0f) + 6.0f;
    float arm = fmaxf(5.0f, half * 0.45f);
    constexpr float LINE = 2.0f;
    for (int corner = 0; corner < 4; corner++) {
        float signX = (corner & 1) ? 1.0f : -1.0f;
        float signY = (corner & 2) ? 1.0f : -1.0f;
        float x = sx + signX * half;
        float y = sy + signY * half;
        // Horizontal arm toward the centre, vertical arm toward the centre.
        render::DrawRect(signX > 0.0f ? x - arm : x - LINE * 0.5f, y - LINE * 0.5f, arm + LINE * 0.5f, LINE, CRIMSON);
        render::DrawRect(x - LINE * 0.5f, signY > 0.0f ? y - arm : y - LINE * 0.5f, LINE, arm + LINE * 0.5f, CRIMSON);
    }
    // Health bar, then the name and distance, all below the brackets.
    float health = TargetHealth(world.target);
    float barW = fmaxf(half * 2.0f, 30.0f);
    float below = sy + half + 4.0f;
    render::DrawRect(sx - barW * 0.5f, below, barW, 4.0f, 0xA01A1410);
    render::DrawRect(sx - barW * 0.5f + 1.0f, below + 1.0f, (barW - 2.0f) * health, 2.0f, CRIMSON);
    const char* name = world.target.kind == TargetKind::PLANE ? world.planes[world.target.index].name : nullptr;
    if (world.target.kind == TargetKind::ENGINE)
        name = "ENGINE";
    char text[40];
    if (name)
        snprintf(text, sizeof(text), "%s %dm", name, static_cast<int>(distance));
    else
        snprintf(text, sizeof(text), "%dm", static_cast<int>(distance));
    HudTextCentered(sx, below + 8.0f, 1, CREAM, text);

    // Lead reticle: where to aim so the bullets meet the target (a hollow circle).
    Vec3 lead = LeadPoint(p.flight.position, p.flight.velocity, target, TargetVelocity(world.target), BULLET_SPEED);
    float lx = 0.0f;
    float ly = 0.0f;
    if (distance < 700.0f && render::Project(lead, lx, ly))
        DrawAtlas(content::HUD_LEAD, lx - 8.0f, ly - 8.0f, 16.0f, 16.0f, CREAM);
}

// A blip on the north-up radar (north, -Z, is up); blips beyond the range sit on the rim.
static void RadarBlip(float cx, float cy, float dx, float dz, float size, uint32_t color)
{
    float maxR = RADAR_SIZE * RADAR_FACE;
    float x = dx * (maxR / RADAR_RANGE);
    float y = dz * (maxR / RADAR_RANGE);
    float r = sqrtf(x * x + y * y);
    if (r > maxR) {
        x *= maxR / r;
        y *= maxR / r;
    }
    DrawAtlas(content::HUD_RADAR_BLIP, cx + x - size * 0.5f, cy + y - size * 0.5f, size, size, color);
}

static void DrawRadar(const Plane& p)
{
    float x = render::ScreenWidth() - SAFE_X - RADAR_SIZE;
    float y = SAFE_Y;
    DrawAtlas(content::HUD_RADAR, x, y, RADAR_SIZE, RADAR_SIZE, 0xFFFFFFFF);
    float cx = x + RADAR_SIZE * 0.5f;
    float cy = y + RADAR_SIZE * 0.5f;
    const Vec3& me = p.flight.position;
    int next = NextRing();
    for (int i = 0; i < RingCountTotal(); i++) {
        if (i != next && i != next + 1)
            continue;
        Vec3 r = RingWorldPosition(i);
        RadarBlip(cx, cy, r.x - me.x, r.z - me.z, i == next ? 8.0f : 6.0f, 0xFFFFD23F);
    }
    const Zeppelin& z = world.zeppelin;
    if (z.state == ZeppelinState::ARRIVING || z.state == ZeppelinState::CIRCLING || z.state == ZeppelinState::CRASHING)
        RadarBlip(cx, cy, z.position.x - me.x, z.position.z - me.z, 14.0f, 0xFFA9AEB3);
    for (int i = 1; i < MAX_PLANES; i++) {
        const Plane& e = world.planes[i];
        if (!e.alive)
            continue;
        uint32_t color = e.kind == PlaneKind::ACE ? 0xFFB070FF : ALERT;
        RadarBlip(cx, cy, e.flight.position.x - me.x, e.flight.position.z - me.z, 8.0f, color);
    }
    // The player: a dot with a short line of dots along the heading.
    Vec3 fwd = QuatForward(p.flight.rotation);
    Vec3 flat = Normalize(Vec3(fwd.x, 0.0f, fwd.z));
    for (int i = 1; i <= 4; i++)
        render::DrawRect(cx + flat.x * i * 3.0f - 1.0f, cy + flat.z * i * 3.0f - 1.0f, 2.0f, 2.0f, CREAM);
    DrawAtlas(content::HUD_RADAR_BLIP, cx - 5.0f, cy - 5.0f, 10.0f, 10.0f, CREAM);
}

// Wraps a radio line onto at most two lines of RADIO_LINE characters, breaking at the space that makes the two
// lines most even (so a short phrase such as "GET 'EM!" is never left alone or split). Returns the length of the
// longest line.
static int WrapRadio(const char* text, char* out, int size)
{
    int length = static_cast<int>(strlen(text));
    if (length >= size)
        length = size - 1;
    memcpy(out, text, length);
    out[length] = '\0';
    if (length <= RADIO_LINE)
        return length;
    int best = -1;
    int bestLongest = 1 << 30;
    for (int i = 1; i < length - 1; i++) {
        if (out[i] != ' ')
            continue;
        int longest = i > length - i - 1 ? i : length - i - 1;
        if (longest < bestLongest) {
            bestLongest = longest;
            best = i;
        }
    }
    if (best < 0)
        return length;
    out[best] = '\n';
    return bestLongest;
}

static void DrawRadio()
{
    const RadioMessage* m = CurrentRadio();
    if (!m || !m->text)
        return;
    // Slide in from the left (ease-out), fade at the end. Below the objective row, never across it.
    float t = Clamp(m->time / SLIDE_TIME, 0.0f, 1.0f);
    float ease = 1.0f - (1.0f - t) * (1.0f - t);
    float fade = Clamp((4.0f - m->time) / 0.3f, 0.0f, 1.0f);
    float x = SAFE_X - (1.0f - ease) * 300.0f;
    float y = SAFE_Y + 72.0f;
    char text[96];
    int longest = WrapRadio(m->text, text, sizeof(text));
    float w = 48.0f + 12.0f + longest * 7.0f + 12.0f;
    render::DrawRect(x, y, w, 52.0f, WithAlpha(0xB01A1410, fade));
    render::DrawRect(x, y, w, 2.0f, WithAlpha(BRASS, fade));
    render::DrawRect(x, y + 50.0f, w, 2.0f, WithAlpha(BRASS, fade));
    DrawPortrait(m->speaker, x + 4.0f, y + 4.0f, 44.0f,
             WithAlpha(0xFFFFFFFF, fade));
    static const char* const NAMES[3] = {"RED", "HARBOUR", "PIRATE"};
    uint32_t nameColor = m->speaker == Speaker::PIRATE ? 0xFFB070FF : BRASS;
    HudText(x + 60.0f, y + 10.0f, 1, WithAlpha(nameColor, fade), NAMES[static_cast<int>(m->speaker)]);
    HudText(x + 60.0f, y + 24.0f, 1, WithAlpha(CREAM, fade), text);
}

static void DrawPopupsAndBanner()
{
    float cx = render::ScreenWidth() * 0.5f;
    float cy = render::ScreenHeight() * 0.5f;
    // Two lines under the plane, oldest on top; the rest wait their turn.
    for (int i = 0; i < hud.popupCount && i < POPUPS_SHOWN; i++) {
        const Popup& p = hud.popups[i];
        float alpha = Clamp((POPUP_TIME - p.time) / 0.4f, 0.0f, 1.0f) * Clamp(p.time / 0.1f, 0.0f, 1.0f);
        HudTextCentered(cx, cy + 108.0f + i * POPUP_PITCH, 2, WithAlpha(p.color, alpha), p.text);
    }
    if (hud.bannerTime < hud.bannerHold && hud.banner[0]) {
        // Scales in: one step larger for the first 0.2 s.
        int scale = hud.bannerTime < 0.2f ? 4 : 3;
        float alpha = Clamp((hud.bannerHold - hud.bannerTime) / 0.25f, 0.0f, 1.0f);
        HudTextCentered(cx, cy - 110.0f, scale, WithAlpha(hud.bannerColor, alpha), hud.banner);
    }
}

// Crimson in the corners after damage (strength 0..1): nothing inside 70 % of the screen's radius, alpha 0.35 at
// the corners at full strength.
static void DrawVignette(float strength)
{
    if (strength <= 0.01f)
        return;
    float w = static_cast<float>(render::ScreenWidth());
    float h = static_cast<float>(render::ScreenHeight());
    uint32_t color = WithAlpha(CRIMSON, Clamp(strength, 0.0f, 1.0f) * 0.35f);
    render::DrawImage(vignette, 0.0f, 0.0f, w, h, 0.0f, 0.0f, 1.0f, 1.0f, color, render::Blend::ALPHA);
}

static void DrawStatus(const Plane& p)
{
    char text[48];
    snprintf(text, sizeof(text), "SCORE %06d", world.stats.score);
    HudText(SAFE_X, SAFE_Y, 2, CREAM, text);
    for (int i = 0; i < world.lives; i++)
        DrawPortrait(Speaker::RED, SAFE_X + i * 26.0f, SAFE_Y + 22.0f, 24.0f, 0xFFFFFFFF);

    float h = static_cast<float>(render::ScreenHeight());
    float w = static_cast<float>(render::ScreenWidth());
    // HULL: the hull left (full and green undamaged, red when nearly shot down); BOOST: the meter boost and
    // stunts share (green full, red empty).
    float health = p.alive ? p.health / p.maxHealth : 0.0f;
    float flash = hud.meterPulse > 0.0f ? sinf(hud.meterPulse / METER_PULSE_TIME * PI) : 0.0f;
    float barsY = h - SAFE_Y - BAR_HEIGHT * 2.0f - 8.0f;
    DrawBar(SAFE_X, barsY, health, "HULL", health < 0.25f, 0.0f);
    DrawBar(SAFE_X, barsY + BAR_HEIGHT + 8.0f, p.flight.meter, "BOOST", p.flight.meter < STUNT_COST, flash);

    // The objective: top centre, under the score row, clear of the plane and the reticle.
    const char* objective = ObjectiveText();
    if (objective[0])
        HudTextCentered(w * 0.5f, SAFE_Y + 26.0f, 2, CREAM, objective);

    snprintf(text, sizeof(text), "ROCKETS %d", p.rockets);
    HudText(w - SAFE_X - render::TextWidth(2.0f, text), h - SAFE_Y - 16.0f, 2, p.rockets > 0 ? CREAM : ALERT, text);
}

static void DrawObjectivePanel()
{
    float w = static_cast<float>(render::ScreenWidth());
    float cx = w * 0.5f;
    float y = 150.0f;
    render::DrawRect(cx - 180.0f, y - 12.0f, 360.0f, 150.0f, 0xC01A1410);
    render::DrawRect(cx - 180.0f, y - 12.0f, 360.0f, 2.0f, BRASS);
    HudTextCentered(cx, y, 2, BRASS, "MISSION");
    const char* objective = ObjectiveText();
    HudTextCentered(cx, y + 30.0f, 1, CREAM, objective[0] ? objective : "Down the pirates");
    char text[48];
    snprintf(text, sizeof(text), "SCORE %d", world.stats.score);
    HudTextCentered(cx, y + 56.0f, 2, CREAM, text);
    snprintf(text, sizeof(text), "KILLS %d   RINGS %d/%d", world.stats.kills, RingsPassed(), RingCountTotal());
    HudTextCentered(cx, y + 86.0f, 1, CREAM, text);
    snprintf(text, sizeof(text), "STUNTS %d   LIVES %d", world.stats.stunts, world.lives);
    HudTextCentered(cx, y + 104.0f, 1, CREAM, text);
}

// --- HUD

void DrawHud()
{
    if (world.titleMode)
        return;
    const render::Camera& camera = CurrentCamera();
    const Plane& p = Player();
    float w = static_cast<float>(render::ScreenWidth());
    float h = static_cast<float>(render::ScreenHeight());
    Phase phase = CurrentPhase();
    bool cinema = phase == Phase::CRASH || phase == Phase::VICTORY || world.introTimer > 0.0f;

    if (hud.flash > 0.01f)
        render::DrawRect(0.0f, 0.0f, w, h, WithAlpha(0xFFFFFFFF, hud.flash));
    // Damage: a crimson flash in the corners; below 25 % it pulses gently at 1.2 Hz.
    float vignetteStrength = hud.damage;
    if (p.alive && p.health < p.maxHealth * 0.25f)
        vignetteStrength = fmaxf(vignetteStrength, 0.55f + 0.45f * sinf(hud.time * 2.0f * PI * 1.2f));
    DrawVignette(vignetteStrength);

    // The title card sits in the sky at the top, clear of the plane and the harbour.
    if (world.introTimer > 0.0f) {
        float alpha = Clamp(world.introTimer - 0.5f, 0.0f, 1.0f) *
                      Clamp((INTRO_TIME - world.introTimer) * 2.0f, 0.0f, 1.0f);
        HudTextCentered(w * 0.5f, h * 0.08f, 3, WithAlpha(CREAM, alpha), "RAID ON");
        HudTextCentered(w * 0.5f, h * 0.08f + 32.0f, 4, WithAlpha(BRASS, alpha), "ISLA PLATANO");
    }
    if (!cinema && p.alive && world.revealTimer <= 0.0f) {
        DrawTarget(camera, p);
        DrawReticle(p);
        DrawRadar(p);
        DrawStatus(p);
    }
    if (!p.alive && phase != Phase::DEFEAT && !cinema) {
        HudTextCentered(w * 0.5f, h * 0.35f, 3, ALERT, "SHOT DOWN");
        char text[32];
        snprintf(text, sizeof(text), "LIVES %d", world.lives);
        HudTextCentered(w * 0.5f, h * 0.35f + 34.0f, 2, CREAM, text);
    }
    if (world.outOfBounds && p.alive && fmodf(hud.time, 0.8f) < 0.5f)
        HudTextCentered(w * 0.5f, h * 0.25f, 2, ALERT, "TURN BACK");
    // HULL CRITICAL flashes for the first 3 s; after that the red HULL bar and the vignette say it.
    bool critical = p.alive && p.health < p.maxHealth * 0.25f;
    if (critical && hud.criticalTime < CRITICAL_TEXT_TIME && fmodf(hud.time, 0.6f) < 0.4f && !cinema)
        HudTextCentered(w * 0.5f, h * 0.25f + 22.0f, 2, ALERT, "HULL CRITICAL");
    DrawRadio();
    if (!cinema && p.alive)
        DrawTip();
    // Not over the boss's arrival cut (a score popup there reads as part of the tower shot).
    if (world.revealTimer <= 0.0f)
        DrawPopupsAndBanner();
    if (world.showObjective)
        DrawObjectivePanel();
}

} // namespace game
