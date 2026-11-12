// Host-side software rasteriser for previewing the procedural content: z-buffer, per-vertex sun + hemisphere
// lighting like the Xbox renderer (docs/design.md §5), bilinear textures, alpha test and blending, linear fog.
#pragma once

#include <stdint.h>

#include <vector>

#include "core/geometry.h"
#include "core/vmath.h"

namespace preview {

enum class Blend {
    OPAQUE,
    ALPHA,
    ADDITIVE,
    ALPHA_TEST,
};

struct DrawOptions {
    const ImageData* texture = nullptr;
    // Second texture (the renderer's 2x detail path): colour * 2 * texture * 2 * detail, detail uv = uv * scale.
    const ImageData* detail = nullptr;
    float detailScale = 1.0f;
    Blend blend = Blend::OPAQUE;
    bool cull = true;
    bool unlit = false;
    bool fog = true;
    float specular = 0.0f;
    float shininess = 24.0f;
    uint32_t tint = 0xFFFFFFFF;
};

struct Light {
    Vec3 sunDir = Normalize(Vec3(-0.545f, 0.53f, 0.65f)); // toward the sun (azimuth 220, 32 degrees high)
    Vec3 sunColor = Vec3(1.0f, 0.93f, 0.8f);
    Vec3 skyAmbient = Vec3(0.42f, 0.5f, 0.62f);
    Vec3 groundAmbient = Vec3(0.3f, 0.27f, 0.22f);
    Vec3 fogColor = Vec3(0.79f, 0.87f, 0.92f);
    float fogStart = 600.0f;
    float fogEnd = 14000.0f;
};

class Raster {
public:
    Raster(int width, int height);

    // Camera: position, rotation (looks along local -Z), vertical field of view in radians.
    void SetCamera(const Vec3& eye, const Quat& rotation, float fovY, float nearZ, float farZ);
    void SetLight(const Light& l);
    // Sky gradient (zenith to horizon) as background, and clears depth.
    void ClearSky(const Vec3& zenith, const Vec3& horizon);
    void Clear(const Vec3& color);
    void Draw(const MeshData& mesh, const Mat4& model, const DrawOptions& options);
    // Halves the resolution with a 2x2 box filter (supersampling) into RGB bytes.
    std::vector<uint8_t> Resolve(int& outWidth, int& outHeight) const;

private:
    struct ClipVertex {
        Vec4 clip;
        float u;
        float v;
        Vec3 color;
        float alpha;
        float fog;
    };

    void DrawTriangle(const ClipVertex& a, const ClipVertex& b, const ClipVertex& c, const DrawOptions& o);
    void Raster3(const ClipVertex& a, const ClipVertex& b, const ClipVertex& c, const DrawOptions& o);

    int bufferWidth;
    int bufferHeight;
    std::vector<Vec3> colors;
    std::vector<float> depths;
    Mat4 view;
    Mat4 proj;
    Vec3 eyePos;
    Light light;
};

// Writes RGB or RGBA bytes as a PNG file (stored, uncompressed deflate). Returns false on failure.
bool WritePng(const char* path, int width, int height, const uint8_t* pixels, int channels);

} // namespace preview
