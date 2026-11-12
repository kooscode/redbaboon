// 2D painting for procedural textures: a float colour canvas, signed distance shapes with antialiased coverage, and
// a chunky stroke font (A-Z, 0-9) for logos, numbers and stamps. Internal to src/content (docs/design.md §7).
#pragma once

#include <stdint.h>

#include <vector>

#include "content/fast_math.h"
#include "core/geometry.h"
#include "core/vmath.h"

namespace content {

// --- Colours

struct Color {
    float r;
    float g;
    float b;
    float a;
};

// Colour from 0xRRGGBB and an alpha.
Color Hex(uint32_t rgb, float a = 1.0f);
Color Mix(const Color& a, const Color& b, float t);
// Multiplies the colour (not alpha) by k.
Color Shade(const Color& c, float k);
// src over dst, src's alpha scaled by coverage.
Color Over(const Color& dst, const Color& src, float coverage);

// --- Canvas

struct Canvas {
    int width;
    int height;
    std::vector<Color> pixels;

    Canvas(int w, int h, const Color& fill);
    Color& At(int x, int y);
    ImageData ToImage() const;
};

// --- Signed distance shapes (negative inside)

float SdCircle(Vec2 p, Vec2 c, float r);
float SdEllipse(Vec2 p, Vec2 c, Vec2 r);
// Box with half extents, rotated by angle (radians), with rounded corners of radius round.
float SdBox(Vec2 p, Vec2 c, Vec2 half, float angle = 0.0f, float round = 0.0f);
// Distance to the segment a-b (subtract a half width for a stroke).
float SdSegment(Vec2 p, Vec2 a, Vec2 b);
// Signed distance to a simple polygon.
float SdPolygon(Vec2 p, const Vec2* points, int count);
// Antialiased coverage of a shape with distance d; aa is the size of one pixel in the shape's units.
float Coverage(float d, float aa);

// --- Stroke font. Glyphs are 4 units wide and 6 high (y down), advance 4 + gap.

// Distance from p (text units, origin at the top-left of the first glyph) to the strokes' centre lines.
float SdText(Vec2 p, const char* text, float gap);
// Width of the text in text units.
float TextWidthUnits(const char* text, float gap);

} // namespace content
