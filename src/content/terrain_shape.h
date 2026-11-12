// Shape of the island that the props need besides the height field: the canyon path, the harbour bay, the
// landmarks and the islets. Internal to src/content (docs/design.md §2, §7); terrain.cpp defines them.
#pragma once

namespace content {

constexpr float VOLCANO_X = -300.0f;
constexpr float VOLCANO_Z = -650.0f;
constexpr float BAY_X = 700.0f;
constexpr float BAY_Z = 2080.0f;
constexpr float BAY_RADIUS_X = 560.0f;
constexpr float BAY_RADIUS_Z = 460.0f;
constexpr float PIRATE_X = 2900.0f;
constexpr float PIRATE_Z = -2900.0f;
// Height of the pirates' rock tower deck above the island top.
constexpr float PIRATE_TOWER = 140.0f;

// Canyon centre line (z of the canyon at x) and half width.
float CanyonCenterZ(float x);
float CanyonHalfWidth(float x);

// Islets around the island: centre and radius.
struct Islet {
    float x;
    float z;
    float radius;
    float height;
};

int IsletCount();
const Islet& IsletAt(int index);

// How much palm canopy covers the ground at (x, z), 0..1 (props.cpp places the palms; the terrain darkens the
// ground under them so clusters still read where the palms themselves are not drawn).
float PalmGroundShade(float x, float z);

} // namespace content
