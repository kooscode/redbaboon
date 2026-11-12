// Layout of the props texture atlas (MakePropsTexture), shared by the props, the pickups and the painter.
// Internal to src/content (docs/design.md §7).
#pragma once

#include "content/content.h"
#include "content/mesh_builder.h"

namespace content {

constexpr int PROPS_TEX = 256;

constexpr AtlasRect PX_CROWN_SIDE = {0, 0, 128, 64};   // palm crown seen from the side (cut-out), top at v0
constexpr AtlasRect PX_CROWN_TOP = {0, 64, 64, 64};    // palm crown seen from above: a star of fronds (cut-out)
constexpr AtlasRect PX_STRATA = {64, 64, 64, 64};      // canyon wall strata, tiling in this rectangle
constexpr AtlasRect PX_TRUNK = {128, 0, 32, 128};      // palm bark rings: u around, v from the top down to the base
constexpr AtlasRect PX_ROOF = {160, 0, 96, 64};        // terracotta roof tiles (rows across v)
constexpr AtlasRect PX_WALL = {160, 64, 96, 64};       // whitewashed wall with a door and windows, v down
constexpr AtlasRect PX_BANANA_LEAF = {0, 128, 64, 128}; // banana plant leaf card (alpha), base at the bottom
constexpr AtlasRect PX_CRATE = {64, 128, 64, 64};      // supply crate face with the banana logo
constexpr AtlasRect PX_PLANKS = {128, 128, 64, 64};    // weathered planks (decks, boats, platforms)
constexpr AtlasRect PX_ROCK = {192, 128, 64, 64};      // rock
constexpr AtlasRect PX_FLAG = {64, 192, 64, 64};       // the pirates' flag
constexpr AtlasRect PX_BANANA_SKIN = {128, 192, 64, 64}; // banana skin: u around, v from stem to tip
constexpr AtlasRect PX_TOWN_FLAG = {192, 224, 64, 32}; // the harbour's flag
constexpr int PX_SWATCH_X = 192;
constexpr int PX_SWATCH_Y = 192;

enum PropSwatch {
    PS_WHITE,
    PS_INK,
    PS_DARK_WOOD,
    PS_METAL,
    PS_RED,
    PS_YELLOW,
    PS_LAMP,
    PS_ROPE,
    PS_COUNT,
};

inline UvRect PropRect(const AtlasRect& r)
{
    return AtlasUv(r.x, r.y, r.w, r.h, PROPS_TEX);
}

inline UvRect PropSwatchUv(int s)
{
    return AtlasSwatch(PX_SWATCH_X + (s % 4) * 16 + 4, PX_SWATCH_Y + (s / 4) * 16 + 4, 8, 8, PROPS_TEX);
}

} // namespace content
