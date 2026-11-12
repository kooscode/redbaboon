// A renderer that draws nothing, for host tests (render/render.h). Resources get ids, the camera is kept, and
// Project uses the real perspective projection so HUD code paths run as on the Xbox.
#include "render/render.h"

#include <string.h>

namespace render {

constexpr int WIDTH = 640;
constexpr int HEIGHT = 480;

// Ids handed out so far and the camera of the current frame.
static int nextTexture = 0;
static int nextMesh = 0;
static Camera frameCamera;

TextureId CreateTexture(const ImageData& image, bool mipmaps, bool wrap)
{
    (void)image;
    (void)mipmaps;
    (void)wrap;
    return nextTexture++;
}

MeshId CreateMesh(const MeshData& mesh)
{
    (void)mesh;
    return nextMesh++;
}

void BeginFrame(const Camera& camera, const Environment& environment)
{
    (void)environment;
    frameCamera = camera;
}

void DrawSky()
{
    // Nothing to draw on the host.
}

void DrawMesh(MeshId mesh, const Mat4& model, const Material& material)
{
    (void)mesh;
    (void)model;
    (void)material;
}

void DrawSeaGlints(float time, int count, float seaLevel)
{
    (void)time;
    (void)count;
    (void)seaLevel;
}

void DrawBillboards(TextureId atlas, int atlasColumns, int atlasRows, const Billboard* billboards, int count,
                    Blend blend)
{
    (void)atlas;
    (void)atlasColumns;
    (void)atlasRows;
    (void)billboards;
    (void)count;
    (void)blend;
}

void DrawStreaks(TextureId texture, const Streak* streaks, int count, Blend blend)
{
    (void)texture;
    (void)streaks;
    (void)count;
    (void)blend;
}

int ScreenWidth()
{
    return WIDTH;
}

int ScreenHeight()
{
    return HEIGHT;
}

bool Project(const Vec3& world, float& sx, float& sy)
{
    Vec3 rel = Rotate(QuatConjugate(frameCamera.rotation), world - frameCamera.position);
    float depth = -rel.z;
    if (depth <= frameCamera.nearZ)
        return false;
    float f = 1.0f / tanf(frameCamera.fovY * 0.5f);
    float aspect = static_cast<float>(WIDTH) / HEIGHT;
    sx = (rel.x / depth * f / aspect * 0.5f + 0.5f) * WIDTH;
    sy = (0.5f - rel.y / depth * f * 0.5f) * HEIGHT;
    return true;
}

void DrawRect(float x, float y, float w, float h, uint32_t color)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)color;
}

void DrawImage(TextureId texture, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
               uint32_t color, Blend blend)
{
    (void)texture;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)u0;
    (void)v0;
    (void)u1;
    (void)v1;
    (void)color;
    (void)blend;
}

void DrawText(float x, float y, float scale, uint32_t color, const char* text)
{
    (void)x;
    (void)y;
    (void)scale;
    (void)color;
    (void)text;
}

int TextWidth(float scale, const char* text)
{
    return static_cast<int>(strlen(text) * 7 * scale);
}

void DrawTextShadow(float x, float y, float scale, uint32_t color, const char* text, uint32_t shadow)
{
    (void)x;
    (void)y;
    (void)scale;
    (void)color;
    (void)text;
    (void)shadow;
}

FrameStats LastFrameStats()
{
    return FrameStats();
}

void EndFrame()
{
    // Nothing to present on the host.
}

bool Init()
{
    return true;
}

} // namespace render
