// A small CPU canvas with soft (anti-aliased) shapes, for the art the game draws itself at start-up: the
// harbourmaster's portrait (hud.cpp) and the controller and button glyphs (controls.cpp).
#pragma once

#include "core/geometry.h"
#include "core/vmath.h"

#include <math.h>

namespace game {

struct Canvas {
    ImageData image;

    void Blend(int x, int y, uint32_t color, float coverage)
    {
        if (x < 0 || y < 0 || x >= image.width || y >= image.height || coverage <= 0.0f)
            return;
        uint32_t& dst = image.pixels[y * image.width + x];
        float a = ((color >> 24) & 0xFF) / 255.0f * Clamp(coverage, 0.0f, 1.0f);
        float da = ((dst >> 24) & 0xFF) / 255.0f;
        float outA = a + da * (1.0f - a);
        if (outA <= 0.0f)
            return;
        uint32_t out = 0;
        for (int shift = 0; shift < 24; shift += 8) {
            float sc = ((color >> shift) & 0xFF) / 255.0f;
            float d = ((dst >> shift) & 0xFF) / 255.0f;
            float c = (sc * a + d * da * (1.0f - a)) / outA;
            out |= static_cast<uint32_t>(c * 255.0f + 0.5f) << shift;
        }
        dst = out | (static_cast<uint32_t>(outA * 255.0f + 0.5f) << 24);
    }

    // Filled ellipse (centre and radii in pixels).
    void Ellipse(float cx, float cy, float rx, float ry, uint32_t color)
    {
        int x0 = Low(cx - rx);
        int x1 = High(cx + rx);
        int y0 = Low(cy - ry);
        int y1 = High(cy + ry);
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                float dx = (x + 0.5f - cx) / rx;
                float dy = (y + 0.5f - cy) / ry;
                float d = sqrtf(dx * dx + dy * dy);
                Blend(x, y, color, (1.0f - d) * fminf(rx, ry) + 0.5f);
            }
        }
    }

    // Ring between radius r0 and r1 around the image's centre.
    void Ring(float r0, float r1, uint32_t color)
    {
        RingAt(image.width * 0.5f, image.height * 0.5f, r0, r1, color);
    }

    // Ring between radius r0 and r1 around a point.
    void RingAt(float cx, float cy, float r0, float r1, uint32_t color)
    {
        for (int y = Low(cy - r1); y <= High(cy + r1); y++) {
            for (int x = Low(cx - r1); x <= High(cx + r1); x++) {
                float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
                Blend(x, y, color, fminf(d - r0, r1 - d) + 0.5f);
            }
        }
    }

    // A thick line with round ends.
    void Line(float x0, float y0, float x1, float y1, float width, uint32_t color)
    {
        float lx = x1 - x0;
        float ly = y1 - y0;
        float l2 = fmaxf(lx * lx + ly * ly, 1e-4f);
        float r = width * 0.5f + 1.0f;
        for (int y = Low(fminf(y0, y1) - r); y <= High(fmaxf(y0, y1) + r); y++) {
            for (int x = Low(fminf(x0, x1) - r); x <= High(fmaxf(x0, x1) + r); x++) {
                float px = x + 0.5f - x0;
                float py = y + 0.5f - y0;
                float t = Clamp((px * lx + py * ly) / l2, 0.0f, 1.0f);
                float dx = px - lx * t;
                float dy = py - ly * t;
                Blend(x, y, color, width * 0.5f - sqrtf(dx * dx + dy * dy) + 0.5f);
            }
        }
    }

    // Pixel bounds of a shape, clamped to the image.
    int Low(float v) const
    {
        return v < 0.0f ? 0 : static_cast<int>(v);
    }

    int High(float v) const
    {
        int limit = image.width > image.height ? image.width - 1 : image.height - 1;
        return v > limit ? limit : static_cast<int>(v) + 1;
    }
};

} // namespace game
