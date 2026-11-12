// Emblems painted on several textures: banana, gorilla skull, pirate insignia, baboon face, roundel
// (docs/design.md §7).
#include "content/emblems.h"

#include <math.h>

namespace content {

// --- Banana

// Distance to a banana: a tapered arc from (-1, 0) to (1, 0) bulging downward, thickness ~0.36.
static float SdBanana(Vec2 p)
{
    // Arc of radius R around a centre above the banana; angles span the banana's length.
    constexpr float R = 1.6f;
    constexpr float CY = -1.25f;
    constexpr float HALF_ANGLE = 0.72f;
    float dx = p.x;
    float dy = p.y - CY;
    float ang = FastAtan2(dx, dy);
    float k = Clamp(ang / HALF_ANGLE, -1.0f, 1.0f);
    float a = k * HALF_ANGLE;
    Vec2 onArc = {R * FastSin(a), CY + R * FastCos(a)};
    float d = Sqrt((p.x - onArc.x) * (p.x - onArc.x) + (p.y - onArc.y) * (p.y - onArc.y));
    float thick = 0.2f * Sqrt(Max(1.0f - k * k * k * k, 0.0f)) + 0.02f;
    return d - thick;
}

Color PaintBanana(Color dst, Vec2 p, float aa)
{
    Color ink = Hex(PAL_INK);
    Color yellow = Hex(PAL_BANANA);
    float d = SdBanana(p);
    float stem = SdBox(p, Vec2{-1.15f, -0.14f}, Vec2{0.14f, 0.055f}, 0.72f, 0.03f);
    dst = Over(dst, ink, Coverage(Min(d, stem) - 0.07f, aa));
    dst = Over(dst, Hex(0x6A4A22), Coverage(stem, aa));
    // Shading: darker toward the inner (upper) curve, a highlight band along the outer one.
    float r = Sqrt(p.x * p.x + (p.y + 1.25f) * (p.y + 1.25f));
    float across = Clamp((r - 1.6f) / 0.2f, -1.0f, 1.0f);
    Color body = Mix(Hex(0xE0A21E), yellow, Clamp(across * 0.5f + 0.7f, 0.0f, 1.0f));
    dst = Over(dst, body, Coverage(d, aa));
    float hl = Abs(r - 1.68f) - 0.035f;
    dst = Over(dst, Hex(0xFFF2B0, 0.85f), Coverage(Max(hl, d + 0.05f), aa) * Clamp(1.0f - Abs(p.x), 0.0f, 1.0f));
    float tip = SdCircle(p, Vec2{1.07f, -0.05f}, 0.07f);
    dst = Over(dst, Hex(0x3A2A18), Coverage(Max(tip, d), aa));
    return dst;
}

// --- Gorilla skull

Color PaintGorillaSkull(Color dst, Vec2 p, float aa, const Color& bone)
{
    Color ink = Hex(PAL_INK);
    float cranium = SdEllipse(p, Vec2{0, -0.2f}, Vec2{0.78f, 0.66f});
    float brow = SdEllipse(p, Vec2{0, -0.02f}, Vec2{0.92f, 0.26f});
    float cheeks = SdEllipse(p, Vec2{0, 0.22f}, Vec2{0.7f, 0.3f});
    float jaw = SdBox(p, Vec2{0, 0.52f}, Vec2{0.5f, 0.24f}, 0.0f, 0.16f);
    float skull = Min(Min(cranium, brow), Min(cheeks, jaw));
    dst = Over(dst, ink, Coverage(skull - 0.09f, aa));
    dst = Over(dst, bone, Coverage(skull, aa));
    // Shading at the top of the cranium, brow highlight.
    dst = Over(dst, Hex(0xFFFFFF, 0.25f), Coverage(SdEllipse(p, Vec2{-0.2f, -0.5f}, Vec2{0.3f, 0.16f}), aa * 4.0f));
    float eyeL = SdEllipse(p, Vec2{-0.34f, 0.1f}, Vec2{0.22f, 0.15f});
    float eyeR = SdEllipse(p, Vec2{0.34f, 0.1f}, Vec2{0.22f, 0.15f});
    float nose = Min(SdCircle(p, Vec2{-0.08f, 0.34f}, 0.065f), SdCircle(p, Vec2{0.08f, 0.34f}, 0.065f));
    dst = Over(dst, ink, Coverage(Min(Min(eyeL, eyeR), nose), aa));
    // Teeth: an ink line with gaps, and two fangs.
    float mouth = SdBox(p, Vec2{0, 0.56f}, Vec2{0.4f, 0.025f});
    float gaps = 1e9f;
    for (int i = -3; i <= 3; i++)
        gaps = Min(gaps, SdBox(p, Vec2{i * 0.11f, 0.56f}, Vec2{0.012f, 0.13f}));
    dst = Over(dst, ink, Coverage(Min(mouth, Max(gaps, jaw + 0.05f)), aa));
    Vec2 fangL[3] = {{-0.3f, 0.5f}, {-0.18f, 0.5f}, {-0.24f, 0.74f}};
    Vec2 fangR[3] = {{0.18f, 0.5f}, {0.3f, 0.5f}, {0.24f, 0.74f}};
    float fangs = Min(SdPolygon(p, fangL, 3), SdPolygon(p, fangR, 3));
    dst = Over(dst, ink, Coverage(fangs - 0.04f, aa));
    dst = Over(dst, bone, Coverage(fangs, aa));
    return dst;
}

Color PaintPirateInsignia(Color dst, Vec2 p, float aa, const Color& bone)
{
    // Two bananas crossed behind the skull like crossbones.
    constexpr float SCALE = 1.35f;
    for (int i = 0; i < 2; i++) {
        float ang = i == 0 ? 0.85f : -0.85f;
        float c = FastCos(ang);
        float s = FastSin(ang);
        Vec2 q = {(p.x * c + p.y * s) / SCALE, (-p.x * s + p.y * c) / SCALE + 0.12f};
        dst = PaintBanana(dst, q, aa / SCALE);
    }
    Vec2 sp = {p.x * 1.4f, p.y * 1.4f + 0.05f};
    return PaintGorillaSkull(dst, sp, aa * 1.4f, bone);
}

// --- Baboon face

Color PaintBaboonFace(Color dst, Vec2 p, float aa)
{
    Color ink = Hex(PAL_INK);
    Color fur = Hex(0x9C7A48);
    Color cheek = Hex(0xC9AC72);
    Color muzzle = Hex(0xD9927A);
    Color leather = Hex(0x6B4423);
    Color brass = Hex(PAL_BRASS);
    float head = SdEllipse(p, Vec2{0, -0.1f}, Vec2{0.62f, 0.62f});
    float cheeks = Min(SdEllipse(p, Vec2{-0.48f, 0.15f}, Vec2{0.34f, 0.36f}),
                         SdEllipse(p, Vec2{0.48f, 0.15f}, Vec2{0.34f, 0.36f}));
    float snout = SdEllipse(p, Vec2{0, 0.38f}, Vec2{0.3f, 0.5f});
    float all = Min(Min(head, cheeks), snout);
    dst = Over(dst, ink, Coverage(all - 0.08f, aa));
    dst = Over(dst, fur, Coverage(head, aa));
    dst = Over(dst, cheek, Coverage(cheeks, aa));
    dst = Over(dst, ink, Coverage(snout - 0.04f, aa));
    dst = Over(dst, muzzle, Coverage(snout, aa));
    dst = Over(dst, Hex(0xF0B8A0, 0.6f), Coverage(SdEllipse(p, Vec2{-0.08f, 0.3f}, Vec2{0.07f, 0.28f}), aa));
    float nostrils = Min(SdEllipse(p, Vec2{-0.1f, 0.74f}, Vec2{0.07f, 0.05f}),
                           SdEllipse(p, Vec2{0.1f, 0.74f}, Vec2{0.07f, 0.05f}));
    dst = Over(dst, Hex(0x4A2A26), Coverage(nostrils, aa));
    // Leather cap over the top of the head.
    float cap = Max(SdEllipse(p, Vec2{0, -0.2f}, Vec2{0.66f, 0.58f}), p.y + 0.22f);
    dst = Over(dst, ink, Coverage(cap - 0.06f, aa));
    dst = Over(dst, leather, Coverage(cap, aa));
    dst = Over(dst, Hex(0x8A5A30), Coverage(SdEllipse(p, Vec2{-0.2f, -0.55f}, Vec2{0.22f, 0.08f}), aa));
    // Goggles across the eyes: strap, brass rims, lenses with glints.
    dst = Over(dst, ink, Coverage(SdBox(p, Vec2{0, -0.12f}, Vec2{0.66f, 0.05f}), aa));
    for (int side = -1; side <= 1; side += 2) {
        Vec2 c = {side * 0.25f, -0.1f};
        dst = Over(dst, ink, Coverage(SdCircle(p, c, 0.22f), aa));
        dst = Over(dst, brass, Coverage(SdCircle(p, c, 0.18f), aa));
        dst = Over(dst, Hex(0x1E3A4A), Coverage(SdCircle(p, c, 0.12f), aa));
        dst = Over(dst, Hex(0xF5F2EA), Coverage(SdCircle(p, Vec2{c.x - 0.04f, c.y - 0.05f}, 0.04f), aa));
    }
    return dst;
}

// --- Roundel

Color PaintRoundel(Color dst, Vec2 p, float aa)
{
    float r = Sqrt(p.x * p.x + p.y * p.y);
    dst = Over(dst, Hex(PAL_INK), Coverage(r - 1.0f, aa));
    dst = Over(dst, Hex(PAL_BANANA), Coverage(r - 0.93f, aa));
    dst = Over(dst, Hex(PAL_INK), Coverage(r - 0.7f, aa));
    dst = Over(dst, Hex(PAL_CREAM), Coverage(r - 0.64f, aa));
    Vec2 q = {p.x * 1.6f, p.y * 1.6f + 0.12f};
    return PaintBanana(dst, q, aa * 1.6f);
}

} // namespace content
