// Emblems painted on several textures: the banana, the gorilla skull (pirates), the baboon face (the hero), the
// banana roundel. Each composites over a colour at a point in the emblem's own unit space (about -1..1, y down);
// aa is the size of one texel in those units. Internal to src/content (docs/design.md §7).
#pragma once

#include "content/paint.h"

namespace content {

// Palette shared by the emblems and liveries (creative brief §1).
constexpr uint32_t PAL_CRIMSON = 0xC1121F;
constexpr uint32_t PAL_CRIMSON_SHADE = 0x7A0A12;
constexpr uint32_t PAL_CREAM = 0xF2E6C9;
constexpr uint32_t PAL_BANANA = 0xFFD23F;
constexpr uint32_t PAL_INK = 0x1A1410;
constexpr uint32_t PAL_BONE = 0xEDE6D3;
constexpr uint32_t PAL_GUNMETAL = 0x3A3F47;
constexpr uint32_t PAL_HAZARD = 0xE8B923;
constexpr uint32_t PAL_PURPLE = 0x5B2A86;
constexpr uint32_t PAL_GOLD = 0xD4A017;
constexpr uint32_t PAL_BRASS = 0xC9A227;
constexpr uint32_t PAL_UI_CREAM = 0xF4E4C1;
constexpr uint32_t PAL_UI_CRIMSON = 0xB3202A;

// A banana with an ink outline, a highlight, a darker inner curve and a brown stem.
Color PaintBanana(Color dst, Vec2 p, float aa);
// A gorilla skull (wide brow, deep eye sockets, fangs) in bone with ink holes and outline.
Color PaintGorillaSkull(Color dst, Vec2 p, float aa, const Color& bone);
// Skull over two crossed bananas: the pirates' insignia.
Color PaintPirateInsignia(Color dst, Vec2 p, float aa, const Color& bone);
// The hero's face: baboon head with a long muzzle, leather cap and brass goggles.
Color PaintBaboonFace(Color dst, Vec2 p, float aa);
// The banana roundel: yellow ring, ink rims, cream disc with a banana.
Color PaintRoundel(Color dst, Vec2 p, float aa);

} // namespace content
