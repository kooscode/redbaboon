// Renderer: draws lit, textured, fogged meshes, the sky, billboards and the 2D overlay on the Xbox GPU (pbkit).
// render_xbox.cpp is the Xbox implementation; tests/host/render_null.cpp is a no-op version for host tests.
// See docs/design.md §5.
#pragma once

#include <stdint.h>

#include "core/geometry.h"
#include "core/vmath.h"

namespace render {

// --- Resources (create once at start-up; they live until exit)

using MeshId = int;
using TextureId = int;

constexpr MeshId NO_MESH = -1;
constexpr TextureId NO_TEXTURE = -1;

// Uploads a texture. mipmaps: builds the mip chain (use for anything seen at a distance).
// wrap: repeat; otherwise clamp to edge.
TextureId CreateTexture(const ImageData& image, bool mipmaps, bool wrap);

// Uploads a mesh into GPU memory; the MeshData can be freed afterwards.
MeshId CreateMesh(const MeshData& mesh);

// --- Frame set-up

enum class Blend {
    OPAQUE,      // no blending, depth write on
    ALPHA,       // src*a + dst*(1-a), depth test on, no depth write
    ADDITIVE,    // src*a + dst, depth test on, no depth write
    ALPHA_TEST,  // opaque, but pixels with alpha < 0.5 are discarded (foliage, cut-outs)
};

struct Camera {
    Vec3 position;
    Quat rotation;            // looks along its local -Z
    float fovY = 60.0f * DEG; // vertical field of view in radians
    float nearZ = 1.0f;
    float farZ = 7000.0f;     // keep it beyond fogEnd so nothing pops at the far plane
};

// Lighting, fog and sky for the frame (defaults: late afternoon, sun 32 degrees high in the south-west).
// Fog fades to the sky's horizon colour in the view direction: skyHorizon away from the sun, skyHorizonSun toward
// it, weighted by max(0, cosine of the angle between the two on the ground plane)^2, plus the sun glow. The sky
// dome uses the same formula, so fogged terrain and sea meet the sky without a seam.
struct Environment {
    Vec3 sunDirection = Vec3(-0.545f, 0.530f, 0.650f); // unit vector pointing TOWARD the sun
    Vec3 sunColor = Vec3(1.0f, 0.92f, 0.78f);          // direct light
    Vec3 ambient = Vec3(0.36f, 0.42f, 0.52f);          // sky fill, on surfaces facing up
    Vec3 groundAmbient = Vec3(0.26f, 0.23f, 0.18f);    // bounce light, on surfaces facing down
    Vec3 fogColor = Vec3(0.79f, 0.87f, 0.92f);         // unused by the Xbox renderer (see the comment above)
    float fogStart = 350.0f;
    float fogEnd = 6500.0f;
    Vec3 skyZenith = Vec3(0.165f, 0.384f, 0.722f);     // 2A62B8
    Vec3 skyMid = Vec3(0.369f, 0.576f, 0.839f);        // 5E93D6, about 20 degrees up
    Vec3 skyHorizon = Vec3(0.788f, 0.867f, 0.922f);    // C9DDEB, horizon away from the sun
    Vec3 skyHorizonSun = Vec3(0.953f, 0.863f, 0.682f); // F3DCAE, horizon below the sun
    Vec3 sunDisc = Vec3(1.0f, 0.973f, 0.902f);         // FFF8E6
    Vec3 sunGlow = Vec3(1.0f, 0.851f, 0.541f);         // FFD98A, added to sky and fog within 25 degrees of the sun
    float sunGlowStrength = 0.45f;
};

// Starts a frame: clears colour and depth, sets the camera and environment. Call once per frame first.
void BeginFrame(const Camera& camera, const Environment& environment);

// Draws the sky dome (zenith-to-horizon gradient with a sun glow) behind everything. Call right after BeginFrame.
void DrawSky();

// --- 3D drawing

struct Material {
    TextureId texture = NO_TEXTURE; // NO_TEXTURE = vertex colour only
    uint32_t tint = 0xFFFFFFFF;     // multiplies vertex colour and texture (0xAARRGGBB)
    float specular = 0.0f;          // 0..1 sun highlight strength (painted metal ~0.5)
    float shininess = 24.0f;        // highlight exponent
    bool unlit = false;             // ignore sun/ambient (sky objects, muzzle flashes, glowing things); also the fast
                                    // path for static geometry with lighting baked into its vertex colours
                                    // (terrain): about half the vertex cost of a lit draw
    bool fog = true;
    float fogDensity = 1.0f;        // > 1 reaches the horizon colour sooner (the sea: ~1.6)
    bool cullBackFaces = true;
    Blend blend = Blend::OPAQUE;
    Vec2 uvOffset = {0.0f, 0.0f};   // added to texture coordinates (scrolling water)
    Vec3 emissive = Vec3();         // added light colour (damage glow, hit flash)
    float rim = 0.0f;               // fresnel rim, (1 - n.v)^3, on silhouettes; lit draws only (0.25-0.35)
    Vec3 rimColor = Vec3(1.0f, 0.949f, 0.816f); // FFF2D0
    float rimDistance = 0.0f;       // > 0: the rim grows with distance, x (1 + distance / rimDistance), so far
                                    // planes stay readable (enemies: ~250 m: x3 at 500 m)
    // Hemisphere ambient of its own (lit draws), instead of the environment's ambient and groundAmbient, so a
    // backlit silver hull stays light. The defaults are the zeppelin's: sky 9DB8D8 from above and a sea-lit
    // 999EA6 from below, so its albedo B4BAC0 never goes darker than 6A7078.
    bool hemisphere = false;
    Vec3 hemisphereSky = Vec3(0.616f, 0.722f, 0.847f);
    Vec3 hemisphereGround = Vec3(0.600f, 0.620f, 0.650f);
    // Distance LOD (unlit draws): the alpha fades in around minDistance and out around maxDistance (0 = none);
    // with Blend::ALPHA_TEST the switch is at exactly those distances. Parts of a mesh entirely outside are not
    // drawn at all. For prop LODs: full mesh maxDistance 150, impostor minDistance 150 and maxDistance 600.
    float minDistance = 0.0f;
    float maxDistance = 0.0f;
    // > 0: the alpha fades to 0 over this many metres below the sea (y = 0); with Blend::ALPHA_TEST the geometry
    // disappears at half that depth (sunken wreck parts: 6 -> gone 3 m down). The sea is translucent, so without
    // this sunken objects stay visible through it.
    float underwaterFade = 0.0f;
    float depthBias = 0.0f;         // unlit draws: metres toward the camera, times (1 + distance / 100 m); for
                                    // decals, shadows, foam (0.1-0.5). The drawn pixels do not move.
    // Second texture on its own uv scale (terrain detail, sea ripples). When set, BOTH textures modulate at 2x:
    // mid grey 0x80 leaves the colour unchanged, so the textures should average mid grey, and their mip chains then
    // fade to neutral with distance. Costs a third combiner stage on those pixels.
    TextureId detail = NO_TEXTURE;
    Vec2 detailScale = {1.0f, 1.0f};  // detail uv = uv * detailScale + detailOffset
    Vec2 detailOffset = {0.0f, 0.0f};
};

// Draws a mesh with a model matrix (local -> world). Any number of calls per frame. Meshes, and parts of big
// meshes (about 1000 triangles each), outside the view or entirely beyond the fog are skipped. Lit draws assume a
// uniform scale in the model matrix (normals are rescaled, not renormalised).
void DrawMesh(MeshId mesh, const Mat4& model, const Material& material);

// Camera-facing quad (smoke, fire, flashes, clouds, sprites). The atlas texture is split into a grid of
// atlasColumns x atlasRows frames; frame counts left to right, top to bottom.
struct Billboard {
    Vec3 position;
    float size = 1.0f;     // world-space width and height
    float rotation = 0.0f; // radians, around the view axis
    uint32_t color = 0xFFFFFFFF;
    int frame = 0;
};

// Blend::ALPHA billboards are sorted back to front within the call. Near the camera a billboard is capped at 35 % of
// the screen height and fades out between 6 m and 1 m. Billboards, streaks and the 2D overlay share
// a per-frame budget of about 14000 quads; what does not fit is dropped.
void DrawBillboards(TextureId atlas, int atlasColumns, int atlasRows, const Billboard* billboards, int count,
                    Blend blend);

// Camera-facing ribbon between two points (tracers, contrails segments, speed lines).
struct Streak {
    Vec3 from;
    Vec3 to;
    float width = 0.5f;
    uint32_t color = 0xFFFFFFFF;
};

// Texture u runs across the width (0..1), v from `from` (0) to `to` (1). NO_TEXTURE = plain colour. A streak is
// never drawn thinner than about 1.6 pixels, so distant tracers stay visible.
void DrawStreaks(TextureId texture, const Streak* streaks, int count, Blend blend);

// Sparkles of sunlight on the sea along the sun's reflection (additive). Call after drawing the sea; time in
// seconds drives the twinkle. At most 128.
void DrawSeaGlints(float time, int count = 64, float seaLevel = 0.0f);

// --- 2D overlay (pixel coordinates, origin top-left, ScreenWidth() x ScreenHeight()). Draw after all 3D.

int ScreenWidth();
int ScreenHeight();

// Projects a world point to the screen. Returns false when it is behind the camera; sx/sy may be off-screen.
bool Project(const Vec3& world, float& sx, float& sy);

void DrawRect(float x, float y, float w, float h, uint32_t color);
void DrawImage(TextureId texture, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
               uint32_t color, Blend blend);

// Built-in art-deco bitmap font (upper case, digits, punctuation; lower case prints as upper case). Each glyph is
// 6x8 pixels times scale with a 7 * scale advance; lines are 10 * scale apart. Use integer scales. '\n' starts a
// new line.
void DrawText(float x, float y, float scale, uint32_t color, const char* text);
int TextWidth(float scale, const char* text);

// DrawText with a drop shadow one font pixel (scale screen pixels) down and right; the shadow takes the text's
// alpha.
constexpr uint32_t INK = 0xFF1A1410;
void DrawTextShadow(float x, float y, float scale, uint32_t color, const char* text, uint32_t shadow = INK);

// Ends the frame; it is shown at the first vertical blank after the GPU has finished it (never tears, never shows
// a half-drawn frame). BeginFrame waits when the next back buffer is still on screen.
void EndFrame();

// Counters of the last finished frame, for a debug overlay.
struct FrameStats {
    int drawCalls = 0;
    int triangles = 0;
    int culled = 0;          // DrawMesh calls skipped as outside the view
    float gpuWaitMs = 0.0f;  // BeginFrame waiting for the GPU (previous frame) and for the display to move on
    float flipWaitMs = 0.0f; // EndFrame waiting for a free slot in the flip queue
};

FrameStats LastFrameStats();

// --- Lifetime

// Sets the video mode and initialises the GPU. Returns false on failure.
bool Init();

} // namespace render
