// Mesh and image data as the content generator builds them and the renderer uploads them (docs/design.md §4).
// Platform independent.
#pragma once

#include <stdint.h>

#include <vector>

#include "core/vmath.h"

// One vertex. The colour multiplies the texture and the lighting (0xAARRGGBB); white leaves them unchanged.
struct Vertex {
    float px;
    float py;
    float pz;
    float nx;
    float ny;
    float nz;
    float u;
    float v;
    uint32_t color;
};

static_assert(sizeof(Vertex) == 36, "the renderer's vertex layout is 9 dwords");

// Indexed triangle list (three indices per triangle, counter-clockwise when seen from outside).
// At most 65535 vertices per mesh.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
};

// 32-bit image, 0xAARRGGBB, rows top to bottom. Width and height are powers of two (16..1024).
struct ImageData {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
};
