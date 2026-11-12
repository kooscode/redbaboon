// Procedural content: every mesh and texture of the game is generated at start-up from code (no art files).
// Platform independent, so meshes and textures can be previewed on the host (tools/preview). docs/design.md §7.
//
// Which texture goes with which mesh (and how to blend) is listed in each section. All meshes are counter-clockwise
// from outside and can be drawn with back-face culling (double-sided parts carry both faces).
#pragma once

#include "core/geometry.h"
#include "core/vmath.h"

namespace content {

// --- Planes. Each plane is built nose toward -Z, centre of mass at the origin.
// The player's plane: wingspan 11.2 m, length 9.4 m. Pirates are built 115 % larger (wingspan ~12.9 m) so they read
// at range; aces have twin tail fins. Texture: MakePlaneTexture(livery), OPAQUE, specular ~0.5.

enum class Livery {
    PLAYER_RED,      // the Red Baboon: crimson with cream trim and a banana roundel
    PIRATE_GREEN,    // sky pirate fighters: gunmetal with hazard-yellow stripes and a bone skull nose
    PIRATE_PURPLE,   // pirate aces: royal purple and gold, twin tail fins
};

// Fuselage, wings, tail, cockpit frame and pilot (a baboon in goggles, the pirates a gorilla). No propeller.
MeshData MakePlane(Livery livery);
// Canopy glass (and windscreen), drawn after the plane with Blend::ALPHA and the plane texture (its alpha ~0.35).
MeshData MakeCanopyGlass(Livery livery);
// Point (in plane-local metres) where the propeller hub sits: the player's plane, and per livery.
Vec3 PropellerHub();
Vec3 PropellerHub(Livery livery);
// Propeller blades and spinner (spinning, drawn rotated around Z at PropellerHub), centred on the origin.
// The player's has 3 blades of radius 1.4 m; MakePropeller(livery) gives the pirates' 4-blade 1.6 m propeller.
// Texture: MakePlaneTexture(livery) of the same plane, OPAQUE; the spinner wants specular ~0.8.
MeshData MakePropeller();
MeshData MakePropeller(Livery livery);
// Translucent disc drawn instead of the blades at high rpm (motion blur): MakePropellerTexture, Blend::ALPHA,
// no culling needed (both faces are in the mesh). Radius matches MakePropeller(livery).
MeshData MakePropellerDisc();
MeshData MakePropellerDisc(Livery livery);
// Gun muzzle points (plane-local), two wing guns.
Vec3 GunMuzzle(int index);
Vec3 GunMuzzle(Livery livery, int index);

// The pilot's red scarf (player plane only): a ribbon of ScarfSegmentCount() segments trailing back (+Z) from
// ScarfAnchor() (plane-local). MakeScarf() is the whole ribbon at rest, already placed in plane-local space.
// For a flutter, draw MakeScarfSegment(i) instead: segment i has its pivot at its local origin (its front edge) and
// extends ScarfSegmentLength() along +Z; chain them from the anchor, each rotated a little (sine wave) relative to
// the previous one. Texture: MakePlaneTexture(PLAYER_RED), OPAQUE (both faces are in the mesh).
int ScarfSegmentCount();
float ScarfSegmentLength();
Vec3 ScarfAnchor();
MeshData MakeScarf();
MeshData MakeScarfSegment(int index);

// --- Zeppelin boss "the Silverback": 180 m long, hull radius 21 m, nose toward -Z, centre at the origin.
// Every zeppelin mesh uses MakeZeppelinTexture, OPAQUE.

// The whole airship, intact: hull, fins, gondola, engine struts (no engine pods, no turrets).
MeshData MakeZeppelin();
// The same without the hull skin: fins, gondola and struts. Draw it with the hull sections that are still there.
MeshData MakeZeppelinBody();
// The hull skin cut into sections from nose to tail, so burnt sections can be hidden to show the frame.
int ZeppelinHullSectionCount();
MeshData MakeZeppelinHullSection(int index);
Vec3 ZeppelinHullSectionCenter(int index);
// The girder skeleton inside the hull (rings and stringers), for the burning-apart sequence.
MeshData MakeZeppelinFrame();
// Turret on a flank sponson (yaws around +Y), twin barrels pointing -Z, pivot at the origin; ~5 m across.
MeshData MakeTurret();
// Local positions of the turrets and the four engine pods (the targets that bring it down).
int ZeppelinTurretCount();
Vec3 ZeppelinTurret(int index);
int ZeppelinEngineCount();
Vec3 ZeppelinEngine(int index);
// An engine pod (nacelle with a pusher propeller at +Z), centred on the origin, 14 m long, radius 3 m.
MeshData MakeZeppelinEngine();

// --- Weapons and pickups. Texture: MakePropsTexture (their UVs point at its white and logo areas), OPAQUE.

MeshData MakeBanana();       // a banana, ~0.6 m (the bullet when seen close; mostly drawn as a streak)
MeshData MakeBananaRocket(); // a banana with a fin and nozzle, ~1.5 m
MeshData MakeCrate();        // a supply crate with a parachute-less banana logo, ~2 m
MeshData MakeRing();         // a stunt ring, radius 15 m, tube radius 1 m, red/yellow/cream bands; axis along Z

// --- Terrain: the island of the level. Height field over [-SIZE/2, SIZE/2] in X and Z (north is -Z, east +X).
// Tiles: UVs are world position / 400 m (on steep ground: along the wall and the height, so cliffs are not
// smeared); textures: MakeTerrainMacroTexture + MakeTerrainDetailTexture (detailScale 25), see Textures. OPAQUE.

struct Terrain {
    float size = 0.0f;        // world extent in metres
    int resolution = 0;       // samples per side
    std::vector<float> heights;

    // Height at a world position (bilinear). Below 0 is under the sea.
    float HeightAt(float x, float z) const;
    // Unit surface normal at a world position.
    Vec3 NormalAt(float x, float z) const;
};

const Terrain& BuildTerrain();
// The terrain as several meshes (tiles), so the renderer can draw what the camera sees.
int TerrainTileCount();
MeshData MakeTerrainTile(int index);
// World-space bounding sphere of each tile (for culling).
Vec3 TerrainTileCenter(int index);
float TerrainTileRadius(int index);

// Sea at y = 0 covering the map and far beyond (fine grid near the island, coarse far away). UVs tile the water
// texture every 32 m; vertex colours (deep, mid, lagoon) and alpha come from the (smoothed) depth below. The mesh
// ends with the shore foam: a strip 11 m wide along the smoothed coastline, 0.12 m above the water, white fading out
// to sea (drawn after the water, same material). MakeSeaTexture, Blend::ALPHA.
MeshData MakeSea();

// Stunt rings along the canyon (game: +250 points each): centre and rotation (the ring's axis, local Z, runs along
// the canyon, west to east).
int RingCount();
Vec3 RingPosition(int index);
Quat RingRotation(int index);
// Landmarks: the harbour bay (sea level, centre of the bay), the pirate rock tower's top deck, the volcano's crater
// (centre of the lava, for the smoke plume) and the lighthouse lamp (for a glow sprite; the lighthouse stands at
// the end of a stone mole in the bay's mouth).
Vec3 HarbourPosition();
Vec3 PirateBasePosition();
Vec3 VolcanoCraterPosition();
Vec3 LighthouseLampPosition();
// The middle of the harbour village (ground level, 60 m in from the waterfront). For the intro: start behind it,
// looking out over the roofs, the wharf, the steamer and the pier toward the mole and the lighthouse.
Vec3 HarbourVillagePosition();
// The boss's arrival shot: a camera on the island side of the tower looking up at the keep, and a path for the
// zeppelin (4 points, nose along the path, at the mast's height) that starts hidden behind the tower, slides out
// past its side and heads on toward the volcano; about 1.2 km, so cover the first two points in the 2 s cut.
void PirateTowerShot(Vec3& eye, Vec3& target);
int ZeppelinEmergePathCount();
Vec3 ZeppelinEmergePathPoint(int index);
// The middle of the wharf's deck. A good intro opening: eye = wharf + (-110, 30, -10) looking at wharf + (80, -5, 35)
// (east along the waterfront: the village fills the lower left third, the wharf, cranes and steamer the middle).
Vec3 HarbourWharfPosition();
// The natural rock arch over the canyon (between rings 4 and 5): the centre of the opening under it. The opening
// spans the canyon (about 200 m wide) from the river (y -8) up to the arch's underside at about y 100.
Vec3 CanyonArchPosition();

// Level props, positioned in world space already: palm trees, huts, rocks, a lighthouse, docks, boats, the pirate
// base. MakePropsTexture, Blend::ALPHA_TEST (palm fronds are cut-outs; everything else is opaque), back-face
// culling on. Split into groups on a grid over the map, for culling.
int PropGroupCount();
MeshData MakePropGroup(int index);
Vec3 PropGroupCenter(int index);
float PropGroupRadius(int index);

// Palms (review 4 §2), in clusters of 4-12. Up close (within ~150 m) draw a cluster's mesh: MakePalmCluster, the
// props texture, ALPHA_TEST, baked light like the prop groups. Farther out draw each palm as one camera-facing
// billboard: MakePalmImpostorTexture (1 x 1 atlas, ALPHA_TEST), centre = (base + top) / 2 + up * 0.15 * height,
// size = 1.3 * height. Beyond ~600 m draw none: the terrain is darker under each cluster.
struct PalmInstance {
    Vec3 base;    // foot of the trunk
    Vec3 top;     // top of the (leaning) trunk, the crown's centre
    float height; // trunk height, 10-15 m
};

int PalmCount();
PalmInstance Palm(int index);
int PalmClusterCount();
MeshData MakePalmCluster(int index);
Vec3 PalmClusterCenter(int index);
float PalmClusterRadius(int index);
// The palms of cluster i are Palm(PalmClusterFirst(i)) .. Palm(PalmClusterFirst(i) + PalmClusterSize(i) - 1).
int PalmClusterFirst(int index);
int PalmClusterSize(int index);
ImageData MakePalmImpostorTexture(); // a whole palm seen from the side: thin trunk, full star crown; 128x128

// Sky dome hemisphere (unit radius, the renderer may supply its own), vertex colours from zenith to horizon.
MeshData MakeSkyDome();

// --- Textures

// Terrain on the 2x detail path (Material.detail; both textures neutral at mid grey 0x80): terrain Material.texture =
// MakeTerrainMacroTexture() (one tile per 400 m: the mesh UVs as they are), Material.detail =
// MakeTerrainDetailTexture() with detailScale {25, 25} (one tile per 16 m). Both mipmapped, wrap.
ImageData MakeTerrainMacroTexture();  // broad patches +-10 %, slight hue shifts, 256x256
ImageData MakeTerrainDetailTexture(); // clumps, tufts, grit, pebbles +-22 %, 256x256
// Sea on the 2x detail path: Material.texture = MakeSeaTexture() (the ripples on mid grey, alpha as the water; the
// sea mesh UVs tile it every 32 m), Material.detail = MakeSeaDetailTexture() with detailScale {0.2, 0.2} (160 m
// swells), scroll detailOffset slower than uvOffset and in another direction.
ImageData MakeSeaTexture();
ImageData MakeSeaDetailTexture(); // long swells, wind streaks, crests, mid grey +-12 %, 256x256
ImageData MakePlaneTexture(Livery livery); // atlas for MakePlane UVs, 256x256
ImageData MakePropellerTexture(); // the spinning disc: a soft translucent ring with two blade ghosts, 128x128
ImageData MakeZeppelinTexture();  // hull panels, band, emblem, girders and swatches, 256x256
ImageData MakeParticleAtlas();    // 4x4 frames: 0-3 smoke puffs, 4-7 fire, 8 spark, 9 flash, 10 banana,
                                  // 11 glow dot, 12-13 banana peels, 14 metal shard, 15 wood splinter; 256x256.
                                  // Smoke is white-grey (tint it); fire and flash for Blend::ADDITIVE.
ImageData MakeCloudTexture();     // soft cumulus puffs 2x2 frames, lit top, blue-grey base, Blend::ALPHA, 256x256
ImageData MakeHudTexture();       // HUD atlas (see the HUD_* rectangles below), 256x256, Blend::ALPHA
ImageData MakeTitleTexture();     // "RED BABOON" title logo, 512x128, Blend::ALPHA
ImageData MakePropsTexture();     // atlas for the prop groups' (and the pickups') UVs, 256x256
ImageData MakeShadowTexture();    // soft dark blob shadow (ellipse, alpha), 64x64, Blend::ALPHA
// Deco bitmap font: white glyphs (A-Z, 0-9, punctuation; lower case drawn as upper case) on alpha, 8x8 cells for ASCII
// 32..127 in a 16 x 6 grid (cell of code c at x = (c - 32) % 16 * 8, y = (c - 32) / 16 * 8), 128x64.
ImageData MakeFontTexture();

// --- HUD atlas layout: pixel rectangles in MakeHudTexture (white and cream art tints well with DrawImage colour).

struct AtlasRect {
    int x;
    int y;
    int w;
    int h;
};

constexpr AtlasRect HUD_RETICLE = {0, 0, 64, 64};             // gun reticle: ring, ticks, centre dot
constexpr AtlasRect HUD_LEAD = {64, 0, 32, 32};               // lead pipper
constexpr AtlasRect HUD_HIT_MARKER = {96, 0, 32, 32};         // hit marker: four diagonal ticks
constexpr AtlasRect HUD_ARROW = {64, 32, 32, 32};             // off-screen target arrow, pointing up
constexpr AtlasRect HUD_BRACKET = {96, 32, 32, 32};           // target bracket corner (top-left; mirror the others)
constexpr AtlasRect HUD_NEEDLE = {0, 64, 16, 64};             // dial needle pointing up, pivot at pixel (8, 56)
constexpr AtlasRect HUD_RADAR_BLIP = {16, 64, 16, 16};        // radar blip: dot with a glow
constexpr AtlasRect HUD_RADAR_PLAYER = {16, 80, 16, 16};      // radar player marker: chevron pointing up
constexpr AtlasRect HUD_HEAT_ARC = {32, 64, 96, 32};          // gun heat arc (lower half ring, centre at top middle)
constexpr AtlasRect HUD_BAR_FRAME = {32, 96, 96, 16};         // brass bar frame (fill inset 3 px)
constexpr AtlasRect HUD_BAR_FILL = {32, 112, 96, 8};          // bar fill: white with a soft highlight
constexpr AtlasRect HUD_DIAL = {128, 0, 128, 128};            // brass dial gauge: face, ticks, red zone at the low end
constexpr AtlasRect HUD_RADAR = {0, 128, 128, 128};           // radar: brass ring around a dark glass face
constexpr AtlasRect HUD_BANANA = {128, 128, 32, 32};          // icons: banana (also the menu cursor)
constexpr AtlasRect HUD_ROCKET = {160, 128, 32, 32};          //        banana rocket
constexpr AtlasRect HUD_HEART = {192, 128, 32, 32};           //        heart (lives)
constexpr AtlasRect HUD_SKULL = {224, 128, 32, 32};           //        gorilla skull (kills)
constexpr AtlasRect HUD_PORTRAIT_BABOON = {128, 160, 48, 48}; // radio portraits: Red Baboon
constexpr AtlasRect HUD_PORTRAIT_GORILLA = {176, 160, 48, 48}; //                 a pirate gorilla
constexpr AtlasRect HUD_CRATE = {224, 160, 32, 32};           // supply crate icon
constexpr AtlasRect HUD_STAMP_FRAME = {128, 208, 128, 48};    // rank stamp frame (crimson ink); print the rank inside

} // namespace content
