// HUD atlas (reticles, brass dials, radar, bars, icons, radio portraits, rank stamp frame; layout in content.h) and
// the "RED BABOON" title logo. Pulp art deco: cream and white art with ink edges, brass frames, crimson accents.
// docs/design.md §7; creative brief §6.
#include "content/content.h"
#include "content/emblems.h"
#include "content/noise.h"
#include "content/paint.h"

#include <math.h>

namespace content {

// Calls paint(p, pixel size) for every pixel of an atlas rectangle; p is -1..1 across the rectangle's width (y
// scaled by the same factor, so shapes stay round).
template <typename F>
static void PaintArea(Canvas& canvas, const AtlasRect& r, F paint)
{
    float s = 2.0f / r.w;
    for (int y = 0; y < r.h; y++) {
        for (int x = 0; x < r.w; x++) {
            Vec2 p = {(x + 0.5f) * s - 1.0f, (y + 0.5f) * s - r.h * s * 0.5f};
            Color& c = canvas.At(r.x + x, r.y + y);
            c = paint(c, p, s);
        }
    }
}

// White shape with an ink edge (readable over sky and sea; tinted by the HUD colour).
static Color Ink(Color c, float d, float aa)
{
    c = Over(c, Hex(PAL_INK, 0.85f), Coverage(d - aa * 1.2f, aa));
    return Over(c, Hex(0xFFFFFF), Coverage(d, aa));
}

static float Ring(Vec2 p, float r, float half)
{
    return Abs(Sqrt(p.x * p.x + p.y * p.y) - r) - half;
}

// Brass: a warm metal ramp lit from the top-left.
static Color Brass(Vec2 p, float r)
{
    float k = Clamp(0.55f - (p.x + p.y) / (2.0f * r) * 0.45f, 0.0f, 1.0f);
    return Mix(Hex(0x7A5A12), Hex(0xF0D070), k);
}

// --- Reticles and markers

static Color Reticle(Color c, Vec2 p, float aa)
{
    float d = Ring(p, 0.66f, 0.035f);
    for (int i = 0; i < 4; i++) {
        float a = i * PI * 0.5f;
        Vec2 dir = {FastCos(a), FastSin(a)};
        d = Min(d, SdSegment(p, Vec2{dir.x * 0.78f, dir.y * 0.78f}, Vec2{dir.x * 0.98f, dir.y * 0.98f}) - 0.035f);
    }
    d = Min(d, SdCircle(p, Vec2{0, 0}, 0.06f));
    return Ink(c, d, aa);
}

static Color Lead(Color c, Vec2 p, float aa)
{
    float d = Ring(p, 0.55f, 0.08f);
    d = Min(d, SdCircle(p, Vec2{0, 0}, 0.12f));
    return Ink(c, d, aa);
}

static Color HitMarker(Color c, Vec2 p, float aa)
{
    float d = 1e9f;
    for (int i = 0; i < 4; i++) {
        float a = PI * 0.25f + i * PI * 0.5f;
        Vec2 dir = {FastCos(a), FastSin(a)};
        d = Min(d, SdSegment(p, Vec2{dir.x * 0.35f, dir.y * 0.35f}, Vec2{dir.x * 0.85f, dir.y * 0.85f}) - 0.08f);
    }
    return Ink(c, d, aa);
}

static Color Arrow(Color c, Vec2 p, float aa)
{
    Vec2 tri[4] = {{0.0f, -0.85f}, {0.75f, 0.55f}, {0.0f, 0.2f}, {-0.75f, 0.55f}};
    return Ink(c, SdPolygon(p, tri, 4), aa);
}

static Color Bracket(Color c, Vec2 p, float aa)
{
    float d = Min(SdBox(p, Vec2{-0.35f, -0.82f}, Vec2{0.55f, 0.1f}), SdBox(p, Vec2{-0.82f, -0.35f}, Vec2{0.1f, 0.55f}));
    return Ink(c, d, aa);
}

static Color Needle(Color c, Vec2 p, float aa)
{
    // p spans -1..1 across 16 px; the needle runs from the pivot (y = 6) up to the tip (y = -7.5).
    Vec2 needle[3] = {{0.0f, -7.6f}, {0.38f, 5.8f}, {-0.38f, 5.8f}};
    float d = SdPolygon(p, needle, 3);
    c = Over(c, Hex(PAL_INK, 0.8f), Coverage(d - aa * 1.2f, aa));
    c = Over(c, Hex(PAL_UI_CRIMSON), Coverage(d, aa));
    float hub = SdCircle(p, Vec2{0.0f, 6.0f}, 0.65f);
    c = Over(c, Hex(PAL_INK), Coverage(hub - aa, aa));
    return Over(c, Brass(Vec2{p.x, p.y - 6.0f}, 0.65f), Coverage(hub, aa));
}

static Color Blip(Color c, Vec2 p, float aa)
{
    float r = Sqrt(p.x * p.x + p.y * p.y);
    c = Over(c, Color{1.0f, 1.0f, 1.0f, 0.6f}, SmoothStep(1.0f, 0.3f, r));
    return Over(c, Hex(0xFFFFFF), Coverage(r - 0.4f, aa));
}

static Color PlayerMarker(Color c, Vec2 p, float aa)
{
    Vec2 tri[4] = {{0.0f, -0.85f}, {0.7f, 0.75f}, {0.0f, 0.4f}, {-0.7f, 0.75f}};
    return Ink(c, SdPolygon(p, tri, 4), aa);
}

static Color HeatArc(Color c, Vec2 p, float aa)
{
    // The rectangle is 96 x 32: the arc's centre sits at the top middle (y = -1/3 in p units).
    Vec2 q = {p.x, p.y + 0.3333f};
    float r = Sqrt(q.x * q.x + q.y * q.y);
    float d = Abs(r - 0.58f) - 0.06f;
    float ang = FastAtan2(q.y, q.x);
    d = Max(d, -(ang - 0.35f));
    d = Max(d, ang - (PI - 0.35f));
    return Ink(c, d, aa);
}

// --- Brass instruments

static Color Dial(Color c, Vec2 p, float aa)
{
    float r = Sqrt(p.x * p.x + p.y * p.y);
    float ang = FastAtan2(p.x, -p.y);
    // Bezel, face, ticks over a 270 degree sweep from -135 (empty) to +135 (full), red zone at the low end.
    c = Over(c, Hex(PAL_INK), Coverage(r - 0.99f, aa));
    c = Over(c, Brass(p, 1.0f), Coverage(r - 0.95f, aa));
    c = Over(c, Hex(PAL_INK), Coverage(r - 0.82f, aa));
    Color face = Mix(Hex(PAL_UI_CREAM), Hex(0xC8B48A), SmoothStep(0.2f, 0.8f, r));
    c = Over(c, face, Coverage(r - 0.8f, aa));
    float sweep = 135.0f * DEG;
    if (Abs(ang) <= sweep + 0.02f) {
        float red = Max(Abs(r - 0.66f) - 0.07f, ang - (-sweep + 0.8f));
        c = Over(c, Hex(PAL_UI_CRIMSON), Coverage(red, aa));
        float k = (ang + sweep) / (2.0f * sweep) * 10.0f;
        float tick = Abs(k - Floor(k + 0.5f));
        bool major = static_cast<int>(Floor(k + 0.5f)) % 5 == 0;
        float len = major ? 0.22f : 0.12f;
        float d = Max(tick * (2.0f * sweep / 10.0f) * r - (major ? 0.045f : 0.025f), Abs(r - (0.78f - len * 0.5f)) -
                        len * 0.5f);
        c = Over(c, Hex(PAL_INK), Coverage(d, aa));
    }
    // Glass glint across the top-left.
    float glint = Abs(r - 0.6f) - 0.05f;
    glint = Max(glint, -(ang + 2.2f));
    glint = Max(glint, ang + 0.9f);
    return Over(c, Color{1.0f, 1.0f, 1.0f, 0.35f}, Coverage(glint, aa * 3.0f));
}

static Color Radar(Color c, Vec2 p, float aa)
{
    float r = Sqrt(p.x * p.x + p.y * p.y);
    c = Over(c, Hex(PAL_INK), Coverage(r - 0.99f, aa));
    c = Over(c, Brass(p, 1.0f), Coverage(r - 0.95f, aa));
    // Rivets on the bezel.
    for (int i = 0; i < 12; i++) {
        float a = i * PI / 6.0f;
        float d = SdCircle(p, Vec2{FastCos(a) * 0.885f, FastSin(a) * 0.885f}, 0.025f);
        c = Over(c, Hex(0x5A4210), Coverage(d, aa));
    }
    c = Over(c, Hex(PAL_INK), Coverage(r - 0.83f, aa));
    Color glass = Mix(Hex(0x1E3A34, 0.85f), Hex(0x0E1A18, 0.85f), r);
    c = Over(c, glass, Coverage(r - 0.8f, aa));
    if (r < 0.8f) {
        float grid = Min(Ring(p, 0.27f, 0.006f), Ring(p, 0.54f, 0.006f));
        grid = Min(grid, Min(Abs(p.x), Abs(p.y)) - 0.006f);
        c = Over(c, Color{0.6f, 0.9f, 0.7f, 0.35f}, Coverage(grid, aa));
        // North mark.
        float n = SdText(Vec2{(p.x + 0.06f) / 0.03f, (p.y + 0.76f) / 0.03f}, "N", 1.0f) * 0.03f - 0.012f;
        c = Over(c, Hex(PAL_UI_CREAM), Coverage(n, aa));
    }
    float glint = Max(Abs(r - 0.68f) - 0.04f, Max(-(FastAtan2(p.x, -p.y) + 1.9f), FastAtan2(p.x, -p.y) + 0.8f));
    return Over(c, Color{1.0f, 1.0f, 1.0f, 0.18f}, Coverage(glint, aa * 3.0f));
}

static Color BarFrame(Color c, Vec2 p, float aa)
{
    // 96 x 16: p.y spans -1/6..1/6.
    float outer = SdBox(p, Vec2{0, 0}, Vec2{0.98f, 0.15f}, 0.0f, 0.06f);
    float inner = SdBox(p, Vec2{0, 0}, Vec2{0.92f, 0.1f}, 0.0f, 0.03f);
    float d = Max(outer, -inner);
    c = Over(c, Hex(PAL_INK), Coverage(Max(outer - aa, -inner - aa), aa));
    return Over(c, Brass(Vec2{p.x * 0.1f, p.y * 6.0f}, 1.0f), Coverage(d, aa));
}

static Color BarFill(Color, Vec2 p, float)
{
    float k = (p.y + 1.0f / 12.0f) * 6.0f;
    float shade = 0.8f + 0.2f * SmoothStep(0.0f, 0.35f, k) - 0.15f * SmoothStep(0.6f, 1.0f, k);
    return Color{shade, shade, shade, 1.0f};
}

// --- Icons and portraits

static Color RocketIcon(Color c, Vec2 p, float aa)
{
    Vec2 fin[3] = {{0.55f, 0.15f}, {0.9f, 0.55f}, {0.45f, 0.45f}};
    c = Over(c, Hex(PAL_INK), Coverage(SdPolygon(p, fin, 3) - aa * 2.0f, aa));
    c = Over(c, Hex(PAL_UI_CRIMSON), Coverage(SdPolygon(p, fin, 3), aa));
    float flame = SdEllipse(p, Vec2{0.82f, 0.62f}, Vec2{0.16f, 0.1f});
    c = Over(c, Hex(0xFFB23F), Coverage(flame, aa));
    float ca = FastCos(-0.7f);
    float sa = FastSin(-0.7f);
    Vec2 q = {(p.x * ca - p.y * sa) * 1.35f, (p.x * sa + p.y * ca) * 1.35f + 0.1f};
    return PaintBanana(c, q, aa * 1.35f);
}

static Color Heart(Color c, Vec2 p, float aa)
{
    // Heart from two circles and a triangle.
    float d = Min(SdCircle(p, Vec2{-0.33f, -0.2f}, 0.4f), SdCircle(p, Vec2{0.33f, -0.2f}, 0.4f));
    Vec2 tri[3] = {{-0.72f, -0.05f}, {0.72f, -0.05f}, {0.0f, 0.8f}};
    d = Min(d, SdPolygon(p, tri, 3));
    c = Over(c, Hex(PAL_INK), Coverage(d - aa * 2.5f, aa));
    c = Over(c, Hex(PAL_UI_CRIMSON), Coverage(d, aa));
    float shine = SdEllipse(p, Vec2{-0.35f, -0.32f}, Vec2{0.13f, 0.09f});
    return Over(c, Color{1.0f, 0.9f, 0.85f, 0.7f}, Coverage(shine, aa));
}

static Color CrateIcon(Color c, Vec2 p, float aa)
{
    float box = SdBox(p, Vec2{0, 0.05f}, Vec2{0.75f, 0.7f}, 0.0f, 0.06f);
    c = Over(c, Hex(PAL_INK), Coverage(box - aa * 2.0f, aa));
    c = Over(c, Hex(0xB08050), Coverage(box, aa));
    float inner = SdBox(p, Vec2{0, 0.05f}, Vec2{0.58f, 0.53f});
    c = Over(c, Hex(0x8A6038), Coverage(Abs(inner) - 0.03f, aa));
    return PaintBanana(c, Vec2{p.x * 2.0f, p.y * 2.0f + 0.2f}, aa * 2.0f);
}

static Color PortraitFrame(Color c, Vec2 p, float aa, const Color& sky)
{
    float r = Sqrt(p.x * p.x + p.y * p.y);
    c = Over(c, Hex(PAL_INK), Coverage(r - 0.99f, aa));
    c = Over(c, Brass(p, 1.0f), Coverage(r - 0.95f, aa));
    return Over(c, Mix(sky, Shade(sky, 0.7f), (p.y + 1.0f) * 0.5f), Coverage(r - 0.84f, aa));
}

static Color GorillaFace(Color c, Vec2 p, float aa)
{
    Color ink = Hex(PAL_INK);
    float head = SdEllipse(p, Vec2{0, -0.05f}, Vec2{0.68f, 0.66f});
    float crest = SdEllipse(p, Vec2{0, -0.45f}, Vec2{0.42f, 0.38f});
    float shape = Min(head, crest);
    c = Over(c, ink, Coverage(shape - 0.06f, aa));
    c = Over(c, Hex(0x2A2A2A), Coverage(shape, aa));
    float cap = Max(SdEllipse(p, Vec2{0, -0.35f}, Vec2{0.66f, 0.52f}), p.y + 0.28f);
    c = Over(c, Hex(0x6B4423), Coverage(cap, aa));
    c = Over(c, Hex(0xB8B8B8), Coverage(SdEllipse(p, Vec2{0, -0.12f}, Vec2{0.55f, 0.12f}), aa));
    float face = SdEllipse(p, Vec2{0, 0.28f}, Vec2{0.42f, 0.34f});
    c = Over(c, Hex(0x4E4644), Coverage(face, aa));
    c = Over(c, ink, Coverage(Min(SdEllipse(p, Vec2{-0.12f, 0.2f}, Vec2{0.09f, 0.06f}),
                                    SdEllipse(p, Vec2{0.12f, 0.2f}, Vec2{0.09f, 0.06f})), aa));
    c = Over(c, ink, Coverage(SdBox(p, Vec2{0, 0.45f}, Vec2{0.22f, 0.025f}), aa));
    // Eyepatch on one side, an amber eye on the other.
    c = Over(c, ink, Coverage(SdCircle(p, Vec2{0.24f, 0.0f}, 0.11f), aa));
    c = Over(c, ink, Coverage(SdSegment(p, Vec2{-0.6f, -0.3f}, Vec2{0.6f, 0.15f}) - 0.025f, aa));
    c = Over(c, Hex(0xE8C060), Coverage(SdCircle(p, Vec2{-0.24f, 0.0f}, 0.07f), aa));
    return Over(c, ink, Coverage(SdCircle(p, Vec2{-0.24f, 0.0f}, 0.03f), aa));
}

static Color StampFrame(Color c, Vec2 p, float aa)
{
    // 128 x 48: p.y spans -0.375..0.375. Double crimson border with an inked, uneven look.
    float outer = Abs(SdBox(p, Vec2{0, 0}, Vec2{0.95f, 0.33f}, 0.0f, 0.06f)) - 0.025f;
    float inner = Abs(SdBox(p, Vec2{0, 0}, Vec2{0.87f, 0.25f}, 0.0f, 0.03f)) - 0.01f;
    float stars = Min(SdCircle(p, Vec2{-0.91f, 0.0f}, 0.025f), SdCircle(p, Vec2{0.91f, 0.0f}, 0.025f));
    float d = Min(Min(outer, inner), stars);
    float grain = Fbm(p.x * 30.0f, p.y * 30.0f, 3, 601u);
    Color ink = Hex(PAL_UI_CRIMSON, Clamp(0.55f + grain * 0.6f, 0.0f, 1.0f));
    return Over(c, ink, Coverage(d, aa));
}

ImageData MakeHudTexture()
{
    Canvas canvas(256, 256, Color{1.0f, 1.0f, 1.0f, 0.0f});
    PaintArea(canvas, HUD_RETICLE, Reticle);
    PaintArea(canvas, HUD_LEAD, Lead);
    PaintArea(canvas, HUD_HIT_MARKER, HitMarker);
    PaintArea(canvas, HUD_ARROW, Arrow);
    PaintArea(canvas, HUD_BRACKET, Bracket);
    PaintArea(canvas, HUD_NEEDLE, Needle);
    PaintArea(canvas, HUD_RADAR_BLIP, Blip);
    PaintArea(canvas, HUD_RADAR_PLAYER, PlayerMarker);
    PaintArea(canvas, HUD_HEAT_ARC, HeatArc);
    PaintArea(canvas, HUD_BAR_FRAME, BarFrame);
    PaintArea(canvas, HUD_BAR_FILL, BarFill);
    PaintArea(canvas, HUD_DIAL, Dial);
    PaintArea(canvas, HUD_RADAR, Radar);
    PaintArea(canvas, HUD_BANANA, [](Color c, Vec2 p, float aa) {
        return PaintBanana(c, Vec2{p.x * 1.15f, p.y * 1.15f + 0.1f}, aa * 1.15f);
    });
    PaintArea(canvas, HUD_ROCKET, RocketIcon);
    PaintArea(canvas, HUD_HEART, Heart);
    PaintArea(canvas, HUD_SKULL, [](Color c, Vec2 p, float aa) {
        return PaintGorillaSkull(c, Vec2{p.x * 1.15f, p.y * 1.15f + 0.05f}, aa * 1.15f, Hex(PAL_BONE));
    });
    PaintArea(canvas, HUD_PORTRAIT_BABOON, [](Color c, Vec2 p, float aa) {
        c = PortraitFrame(c, p, aa, Hex(0x5E93D6));
        if (p.x * p.x + p.y * p.y > 0.84f * 0.84f)
            return c;
        return PaintBaboonFace(c, Vec2{p.x * 1.15f, p.y * 1.15f + 0.05f}, aa * 1.15f);
    });
    PaintArea(canvas, HUD_PORTRAIT_GORILLA, [](Color c, Vec2 p, float aa) {
        c = PortraitFrame(c, p, aa, Hex(0x8A2A2A));
        if (p.x * p.x + p.y * p.y > 0.84f * 0.84f)
            return c;
        return GorillaFace(c, Vec2{p.x * 1.15f, p.y * 1.15f + 0.05f}, aa * 1.15f);
    });
    PaintArea(canvas, HUD_CRATE, CrateIcon);
    PaintArea(canvas, HUD_STAMP_FRAME, StampFrame);
    return canvas.ToImage();
}

// --- Font

ImageData MakeFontTexture()
{
    Canvas canvas(128, 64, Color{1.0f, 1.0f, 1.0f, 0.0f});
    for (int code = 32; code < 128; code++) {
        int index = code - 32;
        int cx = (index % 16) * 8;
        int cy = (index / 16) * 8;
        char text[2] = {static_cast<char>(code >= 'a' && code <= 'z' ? code - 32 : code), 0};
        // Glyph units (4 x 6) onto pixels 1..6 across and 0..7 down; a pixel is set where most of it is stroke.
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                int hits = 0;
                for (int sy = 0; sy < 2; sy++) {
                    for (int sx = 0; sx < 2; sx++) {
                        Vec2 q = {(x + 0.25f + sx * 0.5f - 1.0f) * 0.8f, (y + 0.25f + sy * 0.5f - 0.5f) * 0.92f};
                        if (SdText(q, text, 1.0f) < 0.5f)
                            hits++;
                    }
                }
                canvas.At(cx + x, cy + y).a = hits >= 2 ? 1.0f : 0.0f;
            }
        }
    }
    return canvas.ToImage();
}

// --- Title logo

// Draws one line of logo text: crimson letters with a yellow outline, an ink rim and a drop shadow. The line is
// centred at cx, with its top at top; height in pixels; arch lifts the middle.
static Color LogoLine(Color c, Vec2 px, const char* text, float cx, float top, float height, float arch)
{
    float unit = height / 6.0f;
    float gap = 1.9f;
    float width = TextWidthUnits(text, gap) * unit;
    float rel = (px.x - cx) / (width * 0.5f);
    float lift = arch * (1.0f - rel * rel);
    // Italic slant: shift x by the height above the baseline.
    float slant = 0.18f;
    float baseY = top + height;
    auto at = [&](float ox, float oy) {
        float y = px.y - oy + lift;
        float x = px.x - ox + (baseY - y) * slant;
        Vec2 q = {(x - (cx - width * 0.5f)) / unit, (y - top) / unit};
        return SdText(q, text, gap) * unit;
    };
    float stroke = unit * 0.7f;
    float rim = unit * 0.35f;
    float edge = unit * 0.5f;
    // Nothing of this line reaches pixels outside its box (plus outline, slant and shadow).
    float reach = stroke + rim + edge + unit;
    if (px.y < top - reach - arch || px.y > top + height + reach || Abs(px.x - cx) > width * 0.5f + reach + height)
        return c;
    float d = at(0.0f, 0.0f) - stroke;
    // The drop shadow only shows outside the letter's own outline.
    if (d - rim - edge > -1.0f) {
        float shadow = at(unit * 0.5f, unit * 0.5f) - stroke - rim - edge;
        c = Over(c, Hex(PAL_INK, 0.55f), Coverage(shadow, 1.5f));
    }
    c = Over(c, Hex(PAL_INK), Coverage(d - rim - edge, 1.0f));
    c = Over(c, Hex(PAL_BANANA), Coverage(d - rim, 1.0f));
    c = Over(c, Hex(0x7A0A12), Coverage(d - 1.0f, 1.0f));
    float k = Clamp((px.y + lift - top) / height, 0.0f, 1.0f);
    Color fill = Mix(Hex(0xF0503A), Hex(0xC1121F), SmoothStep(0.0f, 0.55f, k));
    fill = Mix(fill, Hex(0x8A0A14), SmoothStep(0.6f, 1.0f, k));
    c = Over(c, fill, Coverage(d, 1.0f));
    // A cream highlight streak across the upper part of each letter.
    float hl = Abs(k - 0.22f) - 0.035f;
    return Over(c, Hex(PAL_CREAM, 0.7f), Coverage(Max(hl * height, d + 2.5f), 1.0f));
}

ImageData MakeTitleTexture()
{
    constexpr int W = 512;
    constexpr int H = 128;
    Canvas canvas(W, H, Color{1.0f, 0.82f, 0.25f, 0.0f});
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            Vec2 px = {x + 0.5f, y + 0.5f};
            Color c = canvas.At(x, y);
            // Speed stripes behind "RED" in banana yellow.
            for (int i = 0; i < 3 && y < 40; i++) {
                float sy = 12.0f + i * 8.0f;
                float len = 110.0f - i * 25.0f;
                float d1 = SdBox(px, Vec2{256.0f - 52.0f - len * 0.5f, sy}, Vec2{len * 0.5f, 2.4f}, 0.0f, 2.4f);
                float d2 = SdBox(px, Vec2{256.0f + 52.0f + len * 0.5f, sy}, Vec2{len * 0.5f, 2.4f}, 0.0f, 2.4f);
                float d = Min(d1, d2);
                c = Over(c, Hex(PAL_INK), Coverage(d - 1.5f, 1.0f));
                c = Over(c, Hex(PAL_BANANA), Coverage(d, 1.0f));
            }
            c = LogoLine(c, px, "RED", 256.0f, 8.0f, 26.0f, 0.0f);
            c = LogoLine(c, px, "BABOON", 256.0f, 52.0f, 52.0f, 5.0f);
            canvas.At(x, y) = c;
        }
    }
    return canvas.ToImage();
}

} // namespace content
