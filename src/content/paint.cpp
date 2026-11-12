// 2D painting: canvas, signed distance shapes and the stroke font (docs/design.md §7).
#include "content/paint.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace content {

// --- Colours

Color Hex(uint32_t rgb, float a)
{
    constexpr float K = 1.0f / 255.0f;
    return Color{((rgb >> 16) & 0xFF) * K, ((rgb >> 8) & 0xFF) * K, (rgb & 0xFF) * K, a};
}

Color Mix(const Color& a, const Color& b, float t)
{
    return Color{Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t), Lerp(a.a, b.a, t)};
}

Color Shade(const Color& c, float k)
{
    return Color{c.r * k, c.g * k, c.b * k, c.a};
}

Color Over(const Color& dst, const Color& src, float coverage)
{
    float sa = Clamp(src.a * coverage, 0.0f, 1.0f);
    if (sa <= 0.0f)
        return dst;
    float oa = sa + dst.a * (1.0f - sa);
    if (oa <= 1e-6f)
        return Color{src.r, src.g, src.b, 0.0f};
    float k = dst.a * (1.0f - sa);
    return Color{(src.r * sa + dst.r * k) / oa, (src.g * sa + dst.g * k) / oa, (src.b * sa + dst.b * k) / oa, oa};
}

// --- Canvas

Canvas::Canvas(int w, int h, const Color& fill) : width(w), height(h), pixels(static_cast<size_t>(w) * h, fill) {}

Color& Canvas::At(int x, int y)
{
    return pixels[static_cast<size_t>(y) * width + x];
}

ImageData Canvas::ToImage() const
{
    ImageData img;
    img.width = width;
    img.height = height;
    img.pixels.resize(pixels.size());
    for (size_t i = 0; i < pixels.size(); i++) {
        const Color& c = pixels[i];
        img.pixels[i] = PackARGB(c.r, c.g, c.b, c.a);
    }
    return img;
}

// --- Signed distance shapes

float SdCircle(Vec2 p, Vec2 c, float r)
{
    float dx = p.x - c.x;
    float dy = p.y - c.y;
    return Sqrt(dx * dx + dy * dy) - r;
}

float SdEllipse(Vec2 p, Vec2 c, Vec2 r)
{
    float dx = (p.x - c.x) / r.x;
    float dy = (p.y - c.y) / r.y;
    float k = Sqrt(dx * dx + dy * dy);
    return (k - 1.0f) * Min(r.x, r.y);
}

float SdBox(Vec2 p, Vec2 c, Vec2 half, float angle, float round)
{
    float dx = p.x - c.x;
    float dy = p.y - c.y;
    if (angle != 0.0f) {
        float cs = FastCos(angle);
        float sn = FastSin(angle);
        float rx = dx * cs + dy * sn;
        dy = -dx * sn + dy * cs;
        dx = rx;
    }
    float lx = Abs(dx) - half.x + round;
    float ly = Abs(dy) - half.y + round;
    float ox = Max(lx, 0.0f);
    float oy = Max(ly, 0.0f);
    return Sqrt(ox * ox + oy * oy) + Min(Max(lx, ly), 0.0f) - round;
}

float SdSegment(Vec2 p, Vec2 a, Vec2 b)
{
    float px = p.x - a.x;
    float py = p.y - a.y;
    float bx = b.x - a.x;
    float by = b.y - a.y;
    float len2 = bx * bx + by * by;
    float h = len2 > 0.0f ? Clamp((px * bx + py * by) / len2, 0.0f, 1.0f) : 0.0f;
    float dx = px - bx * h;
    float dy = py - by * h;
    return Sqrt(dx * dx + dy * dy);
}

float SdPolygon(Vec2 p, const Vec2* points, int count)
{
    float d = 1e30f;
    bool inside = false;
    for (int i = 0, j = count - 1; i < count; j = i, i++) {
        Vec2 a = points[j];
        Vec2 b = points[i];
        d = Min(d, SdSegment(p, a, b));
        if ((b.y > p.y) != (a.y > p.y) && p.x < (a.x - b.x) * (p.y - b.y) / (a.y - b.y) + b.x)
            inside = !inside;
    }
    return inside ? -d : d;
}

float Coverage(float d, float aa)
{
    return Clamp(0.5f - d / aa, 0.0f, 1.0f);
}

// --- Stroke font

// Polylines of each glyph on a 4 x 6 grid, "x,y x,y | x,y ..." ('|' starts a new polyline).
struct Glyph {
    char ch;
    const char* strokes;
};

static const Glyph GLYPHS[] = {
    {'A', "0,6 0,1.5 1.5,0 2.5,0 4,1.5 4,6 | 0,3.6 4,3.6"},
    {'B', "0,0 3,0 4,1 4,2 3,3 0,3 | 3,3 4,4 4,5 3,6 0,6 0,0"},
    {'C', "4,1 3,0 1,0 0,1 0,5 1,6 3,6 4,5"},
    {'D', "0,0 3,0 4,1 4,5 3,6 0,6 0,0"},
    {'E', "4,0 0,0 0,6 4,6 | 0,3 3,3"},
    {'F', "4,0 0,0 0,6 | 0,3 3,3"},
    {'G', "4,1 3,0 1,0 0,1 0,5 1,6 3,6 4,5 4,3.2 2.2,3.2"},
    {'H', "0,0 0,6 | 4,0 4,6 | 0,3 4,3"},
    {'I', "2,0 2,6 | 0.8,0 3.2,0 | 0.8,6 3.2,6"},
    {'J', "4,0 4,5 3,6 1,6 0,5"},
    {'K', "0,0 0,6 | 4,0 0,3.6 | 1.4,2.6 4,6"},
    {'L', "0,0 0,6 4,6"},
    {'M', "0,6 0,0 2,3 4,0 4,6"},
    {'N', "0,6 0,0 4,6 4,0"},
    {'O', "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0"},
    {'P', "0,6 0,0 3,0 4,1 4,2 3,3 0,3"},
    {'Q', "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0 | 2.4,4.4 4,6"},
    {'R', "0,6 0,0 3,0 4,1 4,2 3,3 0,3 | 2,3 4,6"},
    {'S', "4,1 3,0 1,0 0,1 0,2 1,3 3,3 4,4 4,5 3,6 1,6 0,5"},
    {'T', "0,0 4,0 | 2,0 2,6"},
    {'U', "0,0 0,5 1,6 3,6 4,5 4,0"},
    {'V', "0,0 2,6 4,0"},
    {'W', "0,0 1,6 2,3 3,6 4,0"},
    {'X', "0,0 4,6 | 4,0 0,6"},
    {'Y', "0,0 2,3 4,0 | 2,3 2,6"},
    {'Z', "0,0 4,0 0,6 4,6"},
    {'0', "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0 | 3.2,1.4 0.8,4.6"},
    {'1', "0.8,1.2 2,0 2,6 | 0.8,6 3.2,6"},
    {'2', "0,1 1,0 3,0 4,1 4,2 0,6 4,6"},
    {'3', "0,1 1,0 3,0 4,1 4,2 3,3 1.6,3 | 3,3 4,4 4,5 3,6 1,6 0,5"},
    {'4', "3,6 3,0 0,4 4,4"},
    {'5', "4,0 0,0 0,3 3,3 4,4 4,5 3,6 0,6"},
    {'6', "4,1 3,0 1,0 0,1 0,5 1,6 3,6 4,5 4,4 3,3 0,3"},
    {'7', "0,0 4,0 1.6,6"},
    {'8', "1,3 0,2 0,1 1,0 3,0 4,1 4,2 3,3 1,3 0,4 0,5 1,6 3,6 4,5 4,4 3,3"},
    {'9', "4,3 1,3 0,2 0,1 1,0 3,0 4,1 4,5 3,6 1,6 0,5"},
    {'-', "1,3 3,3"},
    {'!', "2,0 2,4 | 2,5.8 2,5.9"},
    {'.', "2,5.8 2,5.9"},
    {':', "2,1.6 2,1.7 | 2,4.6 2,4.7"},
    {',', "2,5.6 1.4,6.6"},
    {'/', "3.5,0 0.5,6"},
    {'+', "2,1.5 2,4.5 | 0.5,3 3.5,3"},
    {'=', "0.5,2 3.5,2 | 0.5,4 3.5,4"},
    {'\'', "2,0 2,1.5"},
    {'"', "1.2,0 1.2,1.5 | 2.8,0 2.8,1.5"},
    {'(', "2.6,0 1.4,1.5 1.4,4.5 2.6,6"},
    {')', "1.4,0 2.6,1.5 2.6,4.5 1.4,6"},
    {'<', "3.5,0.5 0.5,3 3.5,5.5"},
    {'>', "0.5,0.5 3.5,3 0.5,5.5"},
    {'?', "0,1 1,0 3,0 4,1 4,2 2,3.5 2,4.3 | 2,5.8 2,5.9"},
    {'%', "0.3,0.3 1,0.3 1,1 0.3,1 0.3,0.3 | 3.7,0 0.3,6 | 3,5 3.7,5 3.7,5.7 3,5.7 3,5"},
    {'_', "0,6 4,6"},
};

// Parsed stroke segments of every glyph, built on first use.
struct GlyphSegments {
    int count;
    Vec2 a[16];
    Vec2 b[16];
};

static GlyphSegments g_glyphs[128];
static bool g_glyphsParsed = false;

// Reads an unsigned decimal number ("4", "1.6") and returns the text after it. nxdk's strtof asserts at run time,
// so the glyph strokes are parsed here.
static const char* ParseNumber(const char* s, float& value)
{
    value = 0.0f;
    while (*s >= '0' && *s <= '9') {
        value = value * 10.0f + static_cast<float>(*s - '0');
        s++;
    }
    if (*s != '.')
        return s;
    s++;
    float scale = 0.1f;
    while (*s >= '0' && *s <= '9') {
        value += static_cast<float>(*s - '0') * scale;
        scale *= 0.1f;
        s++;
    }
    return s;
}

static void ParseGlyphs()
{
    for (const Glyph& g : GLYPHS) {
        GlyphSegments& out = g_glyphs[static_cast<int>(g.ch)];
        const char* s = g.strokes;
        bool havePrev = false;
        Vec2 prev = {0, 0};
        while (*s) {
            if (*s == ' ') {
                s++;
                continue;
            }
            if (*s == '|') {
                havePrev = false;
                s++;
                continue;
            }
            float x = 0.0f;
            float y = 0.0f;
            s = ParseNumber(s, x);
            s = ParseNumber(s + 1, y);
            Vec2 p = {x, y};
            if (havePrev && out.count < 16) {
                out.a[out.count] = prev;
                out.b[out.count] = p;
                out.count++;
            }
            prev = p;
            havePrev = true;
        }
    }
    g_glyphsParsed = true;
}

float SdText(Vec2 p, const char* text, float gap)
{
    if (!g_glyphsParsed)
        ParseGlyphs();
    float advance = 4.0f + gap;
    int count = static_cast<int>(strlen(text));
    int center = static_cast<int>(Floor((p.x + gap * 0.5f) / advance));
    float d = 1e30f;
    // The glyph under p first, then its neighbours only where their box is closer than the best so far.
    static const int ORDER[3] = {0, -1, 1};
    for (int k = 0; k < 3; k++) {
        int i = center + ORDER[k];
        if (i < 0 || i >= count)
            continue;
        Vec2 q = {p.x - i * advance, p.y};
        float bx = Max(Max(-q.x, q.x - 4.0f), 0.0f);
        float by = Max(Max(-q.y, q.y - 6.0f), 0.0f);
        if (bx * bx + by * by >= d * d)
            continue;
        int ch = static_cast<unsigned char>(text[i]) & 0x7F;
        const GlyphSegments& g = g_glyphs[ch];
        for (int s = 0; s < g.count; s++)
            d = Min(d, SdSegment(q, g.a[s], g.b[s]));
    }
    return d;
}

float TextWidthUnits(const char* text, float gap)
{
    int count = static_cast<int>(strlen(text));
    if (count == 0)
        return 0.0f;
    return count * 4.0f + (count - 1) * gap;
}

} // namespace content
