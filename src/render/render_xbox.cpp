// Renderer for the Xbox GPU (NV2A) through pbkit: lit, textured, fogged meshes, the sky dome, billboards, streaks
// and the 2D overlay (docs/design.md §5, docs/creative/brief.md §1).
//
// How it drives the GPU:
//   - Three vertex programs stay loaded: lit 3D (shaders/mesh.vs.cg), unlit 3D (shaders/unlit.vs.cg: about half
//     the instructions, for terrain with baked lighting, the sky, billboards, streaks and decals) and 2D
//     (shaders/screen.vs.cg); a draw only selects the start slot. One register-combiner setup
//     (shaders/combine.ps.cg) for all.
//   - Frame constants (camera, sun, fog, sky) are uploaded once per frame, a draw uploads 32 dwords (model matrix
//     and material).
//   - Meshes live in contiguous write-combined memory. Their triangles are reordered for the post-transform vertex
//     cache, and their draw commands (with the 16-bit indices packed in) are built once and copied into the push
//     buffer with a string move per draw (NV2A has no index buffers).
//   - Billboards, streaks, the sky and 2D quads are built on the CPU each frame into one of two dynamic vertex
//     buffers, alternating per frame so the CPU never writes what the GPU still reads.
//   - Textures are swizzled A8R8G8B8 with box-filtered mip chains, sampled trilinear. (2x anisotropic filtering
//     cost about 1.2 ms of GPU time per frame on the console in the canyon, so it is off.)
//   - Depth is z/w in a 24-bit fixed-point buffer (pbkit's W-buffering is switched off).
//   - pbkit triple-buffers: EndFrame queues the flip for the next vertical blank, so frames never tear and the
//     frame rate locks to 60 Hz while a frame fits in 16.7 ms.
#include "render/render.h"

#include "render/font_deco.h"

#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <stddef.h>
#include <string.h>
#include <xboxkrnl/xboxkrnl.h>

#include <algorithm>
#include <vector>

namespace render {

namespace {

// --- Constants

constexpr int SCREEN_WIDTH = 640;
constexpr int SCREEN_HEIGHT = 480;

// Depth buffer is D24S8: screen-space z runs from 0 to 2^24 - 1.
constexpr float Z_SCALE = 16777215.0f;

// Push buffer: a frame should fit (60k triangles of indices are ~360 KB); past PUSH_LIMIT_DWORDS the push buffer
// restarts at its head once the GPU has caught up (a short stall, not an error).
constexpr uint32_t PUSH_BUFFER_BYTES = 2 * 1024 * 1024;
constexpr int PUSH_LIMIT_DWORDS = PUSH_BUFFER_BYTES / 4 - 64 * 1024;

// One method header carries at most 2047 data dwords; this header bit makes them all go to the same method.
constexpr int MAX_METHOD_DWORDS = 2047;
constexpr uint32_t NON_INCREMENTING = 0x40000000;

// Transform constants and program words: 32 dwords per method header (the methods' register window).
constexpr int MAX_TRANSFORM_DWORDS = 32;

// Indices per begin/end block (multiple of 6: whole triangles and an even count).
constexpr int INDICES_PER_BATCH = 30000;

// Dynamic vertices per frame (two of these alternate).
constexpr uint32_t DYNAMIC_BYTES = 2 * 1024 * 1024;
constexpr int DYNAMIC_VERTICES = DYNAMIC_BYTES / sizeof(Vertex);

// The video standard field of the encoder settings for PAL (nxdk's hal keeps its own copy private).
constexpr DWORD VIDEO_STANDARD_PAL = 0x00000300;

// GPU memory for meshes and textures is carved from chunks of this size; bigger requests get their own block.
constexpr uint32_t ARENA_CHUNK_BYTES = 1024 * 1024;
constexpr uint32_t GPU_ALIGNMENT = 256;

// The vertex program's c[0] is transform constant slot 96 (slots 0..95 are the "negative" registers).
constexpr int CONSTANT_BASE = 96;

// Constant registers used by the shaders (see shaders/mesh.vs.cg).
constexpr int REG_VIEW_PROJ = 0;
constexpr int REG_FRAME_A_COUNT = 12; // c0..c11
constexpr int REG_DRAW = 12;          // c12..c25
constexpr int REG_DRAW_COUNT = 14;
constexpr int REG_FRAME_B = 26;       // c26..c29
constexpr int REG_FRAME_B_COUNT = 4;

// Texture stage registers repeat every 64 bytes.
constexpr uint32_t TEXTURE_STAGE_STRIDE = 64;

// Vertex attribute slots (NV2A fixed meaning: 0 position, 2 normal, 3 diffuse, 9 texcoord 0).
constexpr int ATTR_POSITION = 0;
constexpr int ATTR_NORMAL = 2;
constexpr int ATTR_COLOR = 3;
constexpr int ATTR_TEXCOORD = 9;
constexpr int ATTR_COUNT = 16;

// Texture register values (see nv_regs.h). The format always sets BORDER_SOURCE (border from a colour): without
// it the GPU expects every mip level to carry a 4-texel border in memory.
constexpr uint32_t TEX_CONTEXT_DMA_B = 2;
constexpr uint32_t TEX_DIMENSIONS_2D = 2;
constexpr uint32_t TEX_ADDRESS_WRAP = 1;
constexpr uint32_t TEX_ADDRESS_CLAMP = 3;
constexpr uint32_t TEX_FILTER_NEAREST = 1;
constexpr uint32_t TEX_FILTER_LINEAR = 2;
constexpr uint32_t TEX_FILTER_TRILINEAR = 6;
constexpr uint32_t TEX_MAX_LOD_CLAMP_ALL = 0xFFF;

// The detail texture (stage 1) is sampled half a mip level sharper: -0.5 in the filter's signed 5.8 LOD bias.
constexpr uint32_t DETAIL_LOD_BIAS = 0x2000 - 128;

// Font texture: 16 x 8 cells of 8x8 pixels for ASCII 0..127; cell 127 is solid (rectangles).
constexpr int FONT_TEXTURE_WIDTH = 128;
constexpr int FONT_TEXTURE_HEIGHT = 64;
constexpr int FONT_CELL = 8;
constexpr int FONT_SOLID_CELL = 127;

// Sky dome: rings by elevation (degrees; dense near the horizon where the colour changes fastest).
constexpr float SKY_RING_ELEVATIONS[] = {-90.0f, -20.0f, -5.0f, 0.0f, 1.5f, 3.5f, 6.0f, 10.0f, 15.0f, 21.0f,
                                         29.0f, 39.0f, 52.0f, 68.0f, 90.0f};
constexpr int SKY_RINGS = sizeof(SKY_RING_ELEVATIONS) / sizeof(SKY_RING_ELEVATIONS[0]);
constexpr int SKY_SEGMENTS = 64;
constexpr int SKY_VERTICES = SKY_RINGS * (SKY_SEGMENTS + 1);
constexpr float SKY_MID_ELEVATION = 20.0f * DEG;

// The sun glow reaches 25 degrees from the sun's centre.
constexpr float COS_GLOW_RADIUS = 0.906308f;

// The sun sprite: disc and corona, as an angular half-size.
constexpr float SUN_SPRITE_HALF_ANGLE = 9.0f * DEG;
constexpr int SUN_TEXTURE_SIZE = 128;

// Billboards: at most this fraction of the screen height, and faded out from FAR to NEAR metres in front of the
// camera.
constexpr float MAX_BILLBOARD_SCREEN = 0.35f;
constexpr float BILLBOARD_FADE_NEAR = 1.0f;
constexpr float BILLBOARD_FADE_FAR = 6.0f;

// Streaks are drawn at least this many pixels wide.
constexpr float MIN_STREAK_PIXELS = 1.6f;

// Sea glints: how long one sparkle lasts (s), its size on screen (pixels) and the most per call.
constexpr float GLINT_LIFE = 0.3f;
constexpr float GLINT_PIXELS = 7.0f;
constexpr int MAX_GLINTS = 128;
constexpr int GLINT_TEXTURE_SIZE = 32;

// BeginFrame waits at most this many vertical blanks, and at most this long, for the display to leave the next
// back buffer; EndFrame waits at most MAX_FLIP_QUEUE_WAIT_MS for a slot in pbkit's flip queue.
constexpr DWORD MAX_DISPLAY_WAIT_BLANKS = 3;
constexpr float MAX_DISPLAY_WAIT_MS = 60.0f;
constexpr float MAX_FLIP_QUEUE_WAIT_MS = 100.0f;

// Frames longer than this are logged, at most MAX_SLOW_FRAME_LOGS times, once SLOW_FRAME_QUIET_FRAMES frames have
// passed without a texture or mesh upload (so loading screens do not use up the log).
constexpr int SLOW_FRAME_QUIET_FRAMES = 120;
constexpr float SLOW_FRAME_MS = 25.0f;
constexpr int MAX_SLOW_FRAME_LOGS = 20;

// Width of the distance fade at minDistance/maxDistance, as a fraction of that distance.
constexpr float FADE_BAND = 0.1f;

// Depth bias grows by this fraction per metre of distance (depth precision falls with distance).
constexpr float DEPTH_BIAS_GROWTH = 0.01f;

// Vertex programs (generated by cgc + vp20compiler at build time).
const uint32_t MESH_PROGRAM[] = {
#include "shaders/mesh.inl"
};

const uint32_t UNLIT_PROGRAM[] = {
#include "shaders/unlit.inl"
};

const uint32_t SCREEN_PROGRAM[] = {
#include "shaders/screen.inl"
};

// All three programs stay loaded, one after the other; a draw only selects the start slot.
constexpr int MESH_PROGRAM_SLOT = 0;
constexpr int MESH_PROGRAM_LENGTH = sizeof(MESH_PROGRAM) / 16;
constexpr int UNLIT_PROGRAM_SLOT = MESH_PROGRAM_SLOT + MESH_PROGRAM_LENGTH;
constexpr int UNLIT_PROGRAM_LENGTH = sizeof(UNLIT_PROGRAM) / 16;
constexpr int SCREEN_PROGRAM_SLOT = UNLIT_PROGRAM_SLOT + UNLIT_PROGRAM_LENGTH;
constexpr int SCREEN_PROGRAM_LENGTH = sizeof(SCREEN_PROGRAM) / 16;

static_assert(SCREEN_PROGRAM_SLOT + SCREEN_PROGRAM_LENGTH <= 136, "NV2A holds 136 vertex program instructions");

// Meshes with more triangles than this are cut into chunks of at most this many, each culled on its own.
constexpr int CHUNK_TRIANGLES = 1024;

// Post-transform vertex cache size the triangle order is optimised for.
constexpr int VERTEX_CACHE_SIZE = 16;

// --- Types

struct GpuTexture {
    uint32_t offset;   // GPU address
    uint32_t format;   // NV097_SET_TEXTURE_FORMAT value
    uint32_t address;  // wrap or clamp
    uint32_t control0; // enable, LOD clamp, anisotropy
    uint32_t filter;   // min/mag filter
};

// A triangle list's draw commands (begin, packed indices, end), built once; a draw copies them into the push
// buffer with a string move. (Push-buffer CALL would avoid the copy, but it hangs the real NV2A.)
struct IndexStream {
    std::vector<uint32_t> words;
    std::vector<int> batchEnds; // end of each begin/end block in words: the push buffer may restart between them
    int triangles = 0;
};

// Part of a mesh with its own bounding sphere, so big meshes (the sea) are culled piece by piece.
struct MeshChunk {
    IndexStream stream;
    Vec3 center; // bounding sphere in local space
    float radius;
};

struct GpuMesh {
    uint32_t vertexOffset; // GPU address of the vertices
    std::vector<MeshChunk> chunks;
    Vec3 center;           // bounding sphere of the whole mesh in local space
    float radius;
};

// Depth handling of a draw: the 3D blend modes test depth; the sky and 2D do not.
enum class DepthMode {
    TEST,
    OFF,
};

enum class Program {
    NONE,
    MESH,   // lit (mesh.vs.cg)
    UNLIT,  // unlit.vs.cg
    SCREEN, // 2D (screen.vs.cg)
};

// GPU state as last pushed; draws push only what changes.
struct StateCache {
    Program program = Program::NONE;
    int blend = -1;           // Blend * 2 + DepthMode, -1 = unknown
    int cull = -1;
    TextureId texture = -2;   // stage 0
    TextureId detail = -2;    // stage 1; NO_TEXTURE = stage off and the plain combiner
    uint32_t vertexBase = 0xFFFFFFFFu;
};

// The current 2D batch: quads in the dynamic buffer, all with one texture and blend.
struct Batch2D {
    TextureId texture = NO_TEXTURE;
    Blend blend = Blend::ALPHA;
    int first = 0; // first dynamic vertex
    int count = 0; // vertices
};

// --- State

// Resources.
std::vector<GpuTexture> textures;
std::vector<GpuMesh> meshes;
TextureId whiteTexture = NO_TEXTURE;
TextureId fontTexture = NO_TEXTURE;
TextureId sunTexture = NO_TEXTURE;
TextureId glintTexture = NO_TEXTURE;

// GPU memory arena.
uint8_t* arenaNext = nullptr;
uint32_t arenaLeft = 0;

// Push buffer position at the last pb_reset, to know when to reset again.
uint32_t* pushHead = nullptr;

// Dynamic vertex buffers and the current one's fill level.
Vertex* dynamicBuffers[2] = {nullptr, nullptr};
int dynamicIndex = 0;
int dynamicUsed = 0;

StateCache state;
Batch2D batch2D;

// Sky dome: unit directions, their elevations (radians) and the index stream.
Vec3 skyDirections[SKY_VERTICES];
float skyElevations[SKY_VERTICES];
IndexStream skyStream;

// The frame's camera and environment.
Camera camera;
Environment environment;
Mat4 viewProjScreen; // world -> screen pixels and depth (homogeneous)
Mat4 viewProjClip;   // world -> clip space, for culling and Project
Vec4 frustumPlanes[6];
Vec3 cameraRight;
Vec3 cameraUp;
Vec3 cameraForward;
Vec3 sunFlat; // sun direction on the ground plane, unit length

FrameStats frameStats;

// Slow-frame log: the times frames ended and started, and the billboards of this frame.
uint64_t lastFrameEnd = 0;
uint64_t frameStartTime = 0;
int slowFrameLogs = 0;
int framesSinceUpload = 0; // frames since the last CreateTexture/CreateMesh: loading frames are not logged
int billboardCount = 0;
float billboardPixels = 0.0f; // screen pixels covered, counting overdraw

// Frames that started while the display still showed their buffer (BeginFrame's wait timed out).
int displayTimeouts = 0;

// Time spent in CreateTexture and CreateMesh, and the resource count when it was last logged.
float loadTextureMs = 0.0f;
float loadMeshMs = 0.0f;
int loggedResources = 0;
FrameStats lastStats;

// Order of alpha-blended billboards (back to front).
std::vector<int> billboardOrder;
std::vector<Billboard> glints;
std::vector<float> billboardDepth;

// --- GPU memory

uint32_t GpuAddress(const void* p)
{
    return reinterpret_cast<uint32_t>(p) & 0x03FFFFFF;
}

// Contiguous, write-combined memory the GPU can read. Lives until exit.
void* GpuAlloc(uint32_t size)
{
    uint32_t rounded = (size + GPU_ALIGNMENT - 1) & ~(GPU_ALIGNMENT - 1);
    if (rounded > ARENA_CHUNK_BYTES / 4)
        return MmAllocateContiguousMemoryEx(rounded, 0, MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (rounded > arenaLeft) {
        arenaNext = static_cast<uint8_t*>(
            MmAllocateContiguousMemoryEx(ARENA_CHUNK_BYTES, 0, MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE));
        if (arenaNext == nullptr) {
            arenaLeft = 0;
            return nullptr;
        }
        arenaLeft = ARENA_CHUNK_BYTES;
    }
    void* p = arenaNext;
    arenaNext += rounded;
    arenaLeft -= rounded;
    return p;
}

float MillisecondsSince(uint64_t start)
{
    static const uint64_t frequency = KeQueryPerformanceFrequency();
    return static_cast<float>(KeQueryPerformanceCounter() - start) * 1000.0f / static_cast<float>(frequency);
}

// Adds the lifetime of the object to a millisecond counter.
class LoadTimer {
public:
    explicit LoadTimer(float& counter) : total(counter), start(KeQueryPerformanceCounter()) {}

    ~LoadTimer()
    {
        total += MillisecondsSince(start);
    }

private:
    float& total;
    uint64_t start;
};

// Makes the CPU's write-combined stores visible before the GPU reads them.
void FlushWrites()
{
    __asm__ __volatile__("sfence" ::: "memory");
}

// --- Push buffer

// Starts a block of at most maxDwords; restarts the push buffer first when the frame would overflow it.
uint32_t* BeginPush(int maxDwords)
{
    uint32_t* p = pb_begin();
    if (p - pushHead + maxDwords > PUSH_LIMIT_DWORDS) {
        pb_reset();
        p = pb_begin();
        pushHead = p;
    }
    return p;
}

uint32_t* PushConstants(uint32_t* p, int reg, const float* data, int vec4Count)
{
    p = pb_push1(p, NV097_SET_TRANSFORM_CONSTANT_LOAD, CONSTANT_BASE + reg);
    int dwords = vec4Count * 4;
    while (dwords > 0) {
        int n = std::min(dwords, MAX_TRANSFORM_DWORDS);
        pb_push(p, NV097_SET_TRANSFORM_CONSTANT, n);
        float* out = reinterpret_cast<float*>(p + 1);
        for (int i = 0; i < n; i++)
            out[i] = data[i];
        p += 1 + n;
        data += n;
        dwords -= n;
    }
    return p;
}

uint32_t* PushProgram(uint32_t* p, int slot, const uint32_t* program, int instructions)
{
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, slot);
    int dwords = instructions * 4;
    while (dwords > 0) {
        int n = std::min(dwords, MAX_TRANSFORM_DWORDS);
        pb_push(p, NV097_SET_TRANSFORM_PROGRAM, n);
        memcpy(p + 1, program, n * 4);
        p += 1 + n;
        program += n;
        dwords -= n;
    }
    return p;
}

// --- State changes (each pushes only when the state differs)

uint32_t* SetProgram(uint32_t* p, Program program)
{
    if (state.program == program)
        return p;
    state.program = program;
    int slot = SCREEN_PROGRAM_SLOT;
    if (program == Program::MESH)
        slot = MESH_PROGRAM_SLOT;
    else if (program == Program::UNLIT)
        slot = UNLIT_PROGRAM_SLOT;
    return pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, slot);
}

uint32_t* SetBlend(uint32_t* p, Blend blend, DepthMode depth)
{
    int key = static_cast<int>(blend) * 2 + static_cast<int>(depth);
    if (state.blend == key)
        return p;
    state.blend = key;
    bool blending = blend == Blend::ALPHA || blend == Blend::ADDITIVE;
    bool depthWrite = depth == DepthMode::TEST && !blending;
    uint32_t destination = blend == Blend::ADDITIVE ? NV097_SET_BLEND_FUNC_DFACTOR_V_ONE
                                                    : NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA;
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, blending ? 1 : 0);
    p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA);
    p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, destination);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, depth == DepthMode::TEST ? 1 : 0);
    p = pb_push1(p, NV097_SET_DEPTH_MASK, depthWrite ? 1 : 0);

    // Alpha test: cut-outs at 0.5; blended pixels with zero alpha are skipped to save fill rate.
    bool alphaTest = blend != Blend::OPAQUE;
    uint32_t reference = blend == Blend::ALPHA_TEST ? 0x80 : 0x00;
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, alphaTest ? 1 : 0);
    p = pb_push1(p, NV097_SET_ALPHA_FUNC, NV097_SET_ALPHA_FUNC_V_GREATER);
    p = pb_push1(p, NV097_SET_ALPHA_REF, reference);
    return p;
}

uint32_t* SetCull(uint32_t* p, bool cull)
{
    int value = cull ? 1 : 0;
    if (state.cull == value)
        return p;
    state.cull = value;
    return pb_push1(p, NV097_SET_CULL_FACE_ENABLE, value);
}

uint32_t* BindTexture(uint32_t* p, uint32_t stage, const GpuTexture& t)
{
    uint32_t offset = stage * TEXTURE_STAGE_STRIDE;
    p = pb_push2(p, NV097_SET_TEXTURE_OFFSET + offset, t.offset, t.format);
    p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS + offset, t.address);
    p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0 + offset, t.control0);
    uint32_t filter = t.filter;
    if (stage == 1)
        filter = (filter & ~NV097_SET_TEXTURE_FILTER_MIPMAP_LOD_BIAS) | DETAIL_LOD_BIAS;
    p = pb_push1(p, NV097_SET_TEXTURE_FILTER + offset, filter);
    return p;
}

uint32_t* SetTexture(uint32_t* p, TextureId id)
{
    if (id < 0 || id >= static_cast<int>(textures.size()))
        id = whiteTexture;
    if (state.texture == id)
        return p;
    state.texture = id;
    return BindTexture(p, 0, textures[id]);
}

// Register combiners generated from shaders/combine.ps.cg and detail.ps.cg (the includes push through p).
uint32_t* PushPlainCombiner(uint32_t* p)
{
#include "shaders/combine.inl"
    return p;
}

uint32_t* PushDetailCombiner(uint32_t* p)
{
#include "shaders/detail.inl"
    return p;
}

// Binds the detail texture on stage 1 and picks the matching combiner setup.
uint32_t* SetDetail(uint32_t* p, TextureId id)
{
    if (id < 0 || id >= static_cast<int>(textures.size()))
        id = NO_TEXTURE;
    if (state.detail == id)
        return p;
    bool wasOn = state.detail >= 0;
    state.detail = id;
    if (id == NO_TEXTURE) {
        p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0 + TEXTURE_STAGE_STRIDE, 0);
        return PushPlainCombiner(p);
    }
    p = BindTexture(p, 1, textures[id]);
    if (!wasOn)
        p = PushDetailCombiner(p);
    return p;
}

// Points the vertex attributes at an array of Vertex.
uint32_t* SetVertexBase(uint32_t* p, uint32_t base)
{
    if (state.vertexBase == base)
        return p;
    state.vertexBase = base;
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + ATTR_POSITION * 4, base + offsetof(Vertex, px));
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + ATTR_NORMAL * 4, base + offsetof(Vertex, nx));
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + ATTR_COLOR * 4, base + offsetof(Vertex, color));
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + ATTR_TEXCOORD * 4, base + offsetof(Vertex, u));
    return p;
}

// --- Draw constants

void UnpackColor(uint32_t argb, float* out)
{
    constexpr float SCALE = 1.0f / 255.0f;
    out[0] = ((argb >> 16) & 0xFF) * SCALE;
    out[1] = ((argb >> 8) & 0xFF) * SCALE;
    out[2] = (argb & 0xFF) * SCALE;
    out[3] = (argb >> 24) * SCALE;
}

// Pushes c12..c24: model matrix, tint, emissive + rim, specular + fog density, uv offset + fog scale + depth bias,
// rim colour, detail uv scale and offset, hemisphere ambient, distance fade, underwater fade.
uint32_t* PushDrawConstants(uint32_t* p, const Mat4& model, const Material& material, Blend blend)
{
    float c[REG_DRAW_COUNT * 4];
    memcpy(c, model.m, sizeof(model.m));
    UnpackColor(material.tint, c + 16);
    c[16] *= 0.5f; // the vertex programs output half the colour; the combiners double it (overbright headroom)
    c[17] *= 0.5f;
    c[18] *= 0.5f;
    c[20] = material.emissive.x;
    c[21] = material.emissive.y;
    c[22] = material.emissive.z;
    float scaleSq = 0.0f;
    for (int row = 0; row < 3; row++) {
        Vec3 axis(model.m[row][0], model.m[row][1], model.m[row][2]);
        scaleSq = std::max(scaleSq, LengthSq(axis));
    }
    c[23] = scaleSq > 0.0f ? 1.0f / sqrtf(scaleSq) : 1.0f; // undoes the model's scale on normals
    c[24] = material.unlit ? 0.0f : material.specular;
    c[25] = material.shininess;
    // Linear fog per draw: saturate(distance * c[26] + c[27]), steeper with the fog density, none when off.
    float fogRange = std::max(environment.fogEnd - environment.fogStart, 1.0f);
    float density = material.fog ? material.fogDensity : 0.0f;
    c[26] = density / fogRange;
    c[27] = -environment.fogStart * density / fogRange;
    c[28] = material.uvOffset.x;
    c[29] = material.uvOffset.y;
    c[30] = blend == Blend::ADDITIVE ? 0.0f : 1.0f; // additive light fades to nothing, not to the fog colour
    c[31] = material.depthBias;
    float rim = material.unlit ? 0.0f : material.rim;
    c[32] = material.rimColor.x * rim;
    c[33] = material.rimColor.y * rim;
    c[34] = material.rimColor.z * rim;
    // w: the lit program's rim growth per metre, the unlit program's depth-bias growth per metre.
    float rimGrowth = material.rimDistance > 0.0f ? 1.0f / material.rimDistance : 0.0f;
    c[35] = material.unlit ? material.depthBias * DEPTH_BIAS_GROWTH : rimGrowth;
    c[36] = material.detailScale.x;
    c[37] = material.detailScale.y;
    c[38] = material.detailOffset.x;
    c[39] = material.detailOffset.y;
    Vec3 sky = material.hemisphere ? material.hemisphereSky : environment.ambient;
    Vec3 ground = material.hemisphere ? material.hemisphereGround : environment.groundAmbient;
    Vec3 mid = (sky + ground) * 0.5f;
    Vec3 half = (sky - ground) * 0.5f;
    c[40] = mid.x;
    c[41] = mid.y;
    c[42] = mid.z;
    c[43] = 0.0f;
    c[44] = half.x;
    c[45] = half.y;
    c[46] = half.z;
    c[47] = 0.0f;
    // Distance fade: alpha rises through 0.5 at minDistance and falls through 0.5 at maxDistance.
    c[48] = 0.0f;
    c[49] = 1.0f;
    c[50] = 0.0f;
    c[51] = 1.0f;
    if (material.minDistance > 0.0f) {
        float band = material.minDistance * FADE_BAND;
        c[48] = 1.0f / band;
        c[49] = 0.5f - material.minDistance / band;
    }
    if (material.maxDistance > 0.0f) {
        float band = material.maxDistance * FADE_BAND;
        c[50] = -1.0f / band;
        c[51] = 0.5f + material.maxDistance / band;
    }
    // Underwater fade: alpha *= min(y * c[52] + c[53], 1), reaching 0 at underwaterFade metres below the sea.
    c[52] = material.underwaterFade > 0.0f ? 1.0f / material.underwaterFade : 0.0f;
    c[53] = 1.0f;
    c[54] = 0.0f;
    c[55] = 0.0f;
    return PushConstants(p, REG_DRAW, c, REG_DRAW_COUNT);
}

// --- Drawing primitives

// Push-buffer header of a method with count data dwords (3D subchannel).
uint32_t MethodHeader(uint32_t method, int count)
{
    return (static_cast<uint32_t>(count) << 18) | method;
}

// Builds the commands that draw a triangle list from the current vertex base.
IndexStream BuildIndexStream(const std::vector<uint16_t>& indices)
{
    IndexStream stream;
    int count = static_cast<int>(indices.size() / 3 * 3);
    std::vector<uint32_t>& w = stream.words;
    for (int done = 0; done < count;) {
        int batch = std::min(count - done, INDICES_PER_BATCH);
        w.push_back(MethodHeader(NV097_SET_BEGIN_END, 1));
        w.push_back(NV097_SET_BEGIN_END_OP_TRIANGLES);
        const uint16_t* source = indices.data() + done;
        for (int left = batch / 2; left > 0;) {
            int n = std::min(left, MAX_METHOD_DWORDS);
            w.push_back(NON_INCREMENTING | MethodHeader(NV097_ARRAY_ELEMENT16, n));
            for (int i = 0; i < n; i++)
                w.push_back(static_cast<uint32_t>(source[i * 2]) | (static_cast<uint32_t>(source[i * 2 + 1]) << 16));
            source += n * 2;
            left -= n;
        }
        if (batch & 1) {
            w.push_back(MethodHeader(NV097_ARRAY_ELEMENT32, 1));
            w.push_back(*source);
        }
        w.push_back(MethodHeader(NV097_SET_BEGIN_END, 1));
        w.push_back(NV097_SET_BEGIN_END_OP_END);
        stream.batchEnds.push_back(static_cast<int>(w.size()));
        done += batch;
    }
    stream.triangles = count / 3;
    return stream;
}

// Copies dwords with one string move (the C library's memcpy is much slower into write-combined memory).
void CopyDwords(uint32_t* destination, const uint32_t* source, int count)
{
    __asm__ __volatile__("rep movsl" : "+D"(destination), "+S"(source), "+c"(count) : : "memory");
}

// Draws an index stream from the current vertex base.
void DrawStream(const IndexStream& stream)
{
    int start = 0;
    for (int end : stream.batchEnds) {
        uint32_t* p = BeginPush(end - start);
        CopyDwords(p, stream.words.data() + start, end - start);
        pb_end(p + (end - start));
        start = end;
    }
    frameStats.triangles += stream.triangles;
}

// Draws quads from vertex `first` of the current vertex base.
void DrawQuads(int first, int count)
{
    constexpr int VERTICES_PER_RUN = 256;
    int runs = (count + VERTICES_PER_RUN - 1) / VERTICES_PER_RUN;
    uint32_t* p = BeginPush(runs + runs / MAX_METHOD_DWORDS + 8);
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);
    int start = first;
    int left = count;
    while (left > 0) {
        int headerRuns = std::min((left + VERTICES_PER_RUN - 1) / VERTICES_PER_RUN, MAX_METHOD_DWORDS);
        pb_push(p, NON_INCREMENTING | NV097_DRAW_ARRAYS, headerRuns);
        p++;
        for (int i = 0; i < headerRuns; i++) {
            int n = std::min(left, VERTICES_PER_RUN);
            *p++ = (static_cast<uint32_t>(n - 1) << 24) | static_cast<uint32_t>(start);
            start += n;
            left -= n;
        }
    }
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    pb_end(p);
    frameStats.drawCalls++;
    frameStats.triangles += count / 2;
}

// Reserves count vertices in this frame's dynamic buffer; nullptr when the frame's budget is used up.
Vertex* AllocDynamic(int count, int& first)
{
    if (dynamicUsed + count > DYNAMIC_VERTICES)
        return nullptr;
    first = dynamicUsed;
    dynamicUsed += count;
    return dynamicBuffers[dynamicIndex] + first;
}

uint32_t DynamicBase()
{
    return GpuAddress(dynamicBuffers[dynamicIndex]);
}

void SetVertex(Vertex& v, const Vec3& p, float u, float tv, uint32_t color)
{
    v.px = p.x;
    v.py = p.y;
    v.pz = p.z;
    v.nx = 0.0f;
    v.ny = 1.0f;
    v.nz = 0.0f;
    v.u = u;
    v.v = tv;
    v.color = color;
}

// Sets up a draw of dynamic world-space quads (billboards, streaks): unlit, fogged, identity model.
void BeginDynamicQuads(TextureId texture, Blend blend)
{
    Material material;
    material.unlit = true;
    material.blend = blend;
    uint32_t* p = BeginPush(128);
    p = SetProgram(p, Program::UNLIT);
    p = SetBlend(p, blend, DepthMode::TEST);
    p = SetCull(p, false);
    p = SetTexture(p, texture);
    p = SetDetail(p, NO_TEXTURE);
    p = SetVertexBase(p, DynamicBase());
    p = PushDrawConstants(p, MatIdentity(), material, blend);
    pb_end(p);
}

// --- 2D batching

void Flush2D()
{
    if (batch2D.count == 0)
        return;
    FlushWrites();
    uint32_t* p = BeginPush(64);
    p = SetProgram(p, Program::SCREEN);
    p = SetBlend(p, batch2D.blend, DepthMode::OFF);
    p = SetCull(p, false);
    p = SetTexture(p, batch2D.texture);
    p = SetDetail(p, NO_TEXTURE);
    p = SetVertexBase(p, DynamicBase());
    pb_end(p);
    DrawQuads(batch2D.first, batch2D.count);
    batch2D.count = 0;
}

void Quad2D(TextureId texture, Blend blend, float x, float y, float w, float h, float u0, float v0, float u1,
            float v1, uint32_t color)
{
    if (batch2D.count > 0 && (batch2D.texture != texture || batch2D.blend != blend ||
                              batch2D.first + batch2D.count != dynamicUsed))
        Flush2D();
    int first = 0;
    Vertex* v = AllocDynamic(4, first);
    if (v == nullptr)
        return;
    if (batch2D.count == 0) {
        batch2D.texture = texture;
        batch2D.blend = blend;
        batch2D.first = first;
    }
    SetVertex(v[0], Vec3(x, y, 0.0f), u0, v0, color);
    SetVertex(v[1], Vec3(x + w, y, 0.0f), u1, v0, color);
    SetVertex(v[2], Vec3(x + w, y + h, 0.0f), u1, v1, color);
    SetVertex(v[3], Vec3(x, y + h, 0.0f), u0, v1, color);
    batch2D.count += 4;
}

// --- Textures

// Bit masks that interleave x and y bits into a swizzled (Morton order) texel address.
void SwizzleMasks(int width, int height, uint32_t& maskX, uint32_t& maskY)
{
    maskX = 0;
    maskY = 0;
    uint32_t bit = 1;
    for (int i = 1; i < width || i < height; i <<= 1) {
        if (i < width) {
            maskX |= bit;
            bit <<= 1;
        }
        if (i < height) {
            maskY |= bit;
            bit <<= 1;
        }
    }
}

// Spreads the bits of value over the set bits of mask.
uint32_t Deposit(uint32_t value, uint32_t mask)
{
    uint32_t result = 0;
    for (uint32_t bit = 1; mask != 0; bit <<= 1) {
        uint32_t lowest = mask & (~mask + 1);
        if (value & bit)
            result |= lowest;
        mask &= mask - 1;
    }
    return result;
}

void WriteSwizzled(uint32_t* destination, const uint32_t* pixels, int width, int height)
{
    uint32_t maskX = 0;
    uint32_t maskY = 0;
    SwizzleMasks(width, height, maskX, maskY);
    std::vector<uint32_t> columnBits(width);
    for (int x = 0; x < width; x++)
        columnBits[x] = Deposit(x, maskX);
    for (int y = 0; y < height; y++) {
        uint32_t rowBits = Deposit(y, maskY);
        const uint32_t* row = pixels + y * width;
        for (int x = 0; x < width; x++)
            destination[rowBits | columnBits[x]] = row[x];
    }
}

// Halves an image (box filter). Colour is averaged weighted by alpha so cut-out edges do not darken.
void Downsample(const std::vector<uint32_t>& source, int width, int height, std::vector<uint32_t>& out)
{
    int w = std::max(width / 2, 1);
    int h = std::max(height / 2, 1);
    out.assign(w * h, 0);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t sumA = 0;
            uint32_t sumR = 0;
            uint32_t sumG = 0;
            uint32_t sumB = 0;
            for (int i = 0; i < 4; i++) {
                int sx = std::min(x * 2 + (i & 1), width - 1);
                int sy = std::min(y * 2 + (i >> 1), height - 1);
                uint32_t c = source[sy * width + sx];
                uint32_t a = (c >> 24) + 1;
                sumA += c >> 24;
                sumR += ((c >> 16) & 0xFF) * a;
                sumG += ((c >> 8) & 0xFF) * a;
                sumB += (c & 0xFF) * a;
            }
            uint32_t weight = sumA + 4;
            uint32_t r = (sumR + weight / 2) / weight;
            uint32_t g = (sumG + weight / 2) / weight;
            uint32_t b = (sumB + weight / 2) / weight;
            uint32_t a = (sumA + 2) / 4;
            out[y * w + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

int Log2(int v)
{
    int n = 0;
    while ((1 << n) < v)
        n++;
    return n;
}

TextureId UploadTexture(const ImageData& image, bool mipmaps, bool wrap, uint32_t minFilter, uint32_t magFilter)
{
    if (image.width <= 0 || image.height <= 0 ||
        static_cast<int>(image.pixels.size()) < image.width * image.height)
        return NO_TEXTURE;
    int levels = mipmaps ? std::max(Log2(image.width), Log2(image.height)) + 1 : 1;
    uint32_t bytes = 0;
    for (int i = 0; i < levels; i++)
        bytes += std::max(image.width >> i, 1) * std::max(image.height >> i, 1) * 4;
    uint32_t* memory = static_cast<uint32_t*>(GpuAlloc(bytes));
    if (memory == nullptr)
        return NO_TEXTURE;

    std::vector<uint32_t> level = image.pixels;
    std::vector<uint32_t> next;
    uint32_t* destination = memory;
    int w = image.width;
    int h = image.height;
    for (int i = 0; i < levels; i++) {
        WriteSwizzled(destination, level.data(), w, h);
        destination += w * h;
        if (i + 1 < levels) {
            Downsample(level, w, h, next);
            level.swap(next);
            w = std::max(w / 2, 1);
            h = std::max(h / 2, 1);
        }
    }
    FlushWrites();

    GpuTexture t;
    t.offset = GpuAddress(memory);
    t.format = TEX_CONTEXT_DMA_B | NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE | (TEX_DIMENSIONS_2D << 4) |
               (NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8 << 8) | (static_cast<uint32_t>(levels) << 16) |
               (static_cast<uint32_t>(Log2(image.width)) << 20) | (static_cast<uint32_t>(Log2(image.height)) << 24);
    uint32_t mode = wrap ? TEX_ADDRESS_WRAP : TEX_ADDRESS_CLAMP;
    t.address = mode | (mode << 8) | (mode << 16);
    t.control0 = NV097_SET_TEXTURE_CONTROL0_ENABLE | (TEX_MAX_LOD_CLAMP_ALL << 6);
    t.filter = (minFilter << 16) | (magFilter << 24) | (2u << 13); // 2 << 13: the hardware's default kernel
    textures.push_back(t);
    return static_cast<TextureId>(textures.size() - 1);
}

void CreateBuiltInTextures()
{
    ImageData white;
    white.width = 4;
    white.height = 4;
    white.pixels.assign(16, 0xFFFFFFFFu);
    whiteTexture = UploadTexture(white, false, true, TEX_FILTER_NEAREST, TEX_FILTER_NEAREST);

    ImageData font;
    font.width = FONT_TEXTURE_WIDTH;
    font.height = FONT_TEXTURE_HEIGHT;
    font.pixels.assign(FONT_TEXTURE_WIDTH * FONT_TEXTURE_HEIGHT, 0x00FFFFFFu);
    for (const Glyph& glyph : FONT_GLYPHS) {
        int cellX = (glyph.code % 16) * FONT_CELL;
        int cellY = (glyph.code / 16) * FONT_CELL;
        for (int y = 0; y < GLYPH_HEIGHT; y++) {
            for (int x = 0; x < GLYPH_WIDTH; x++) {
                if (glyph.rows[y][x] == '#')
                    font.pixels[(cellY + y) * FONT_TEXTURE_WIDTH + cellX + x] = 0xFFFFFFFFu;
            }
        }
    }
    int solidX = (FONT_SOLID_CELL % 16) * FONT_CELL;
    int solidY = (FONT_SOLID_CELL / 16) * FONT_CELL;
    for (int y = 0; y < FONT_CELL; y++) {
        for (int x = 0; x < FONT_CELL; x++)
            font.pixels[(solidY + y) * FONT_TEXTURE_WIDTH + solidX + x] = 0xFFFFFFFFu;
    }
    fontTexture = UploadTexture(font, false, false, TEX_FILTER_NEAREST, TEX_FILTER_NEAREST);

    // Sun: a bright disc with a soft edge inside a corona that falls off to nothing at the sprite's edge.
    ImageData sun;
    sun.width = SUN_TEXTURE_SIZE;
    sun.height = SUN_TEXTURE_SIZE;
    sun.pixels.resize(SUN_TEXTURE_SIZE * SUN_TEXTURE_SIZE);
    constexpr float DISC_RADIUS = 0.17f;
    constexpr float DISC_EDGE = 0.03f;
    for (int y = 0; y < SUN_TEXTURE_SIZE; y++) {
        for (int x = 0; x < SUN_TEXTURE_SIZE; x++) {
            float dx = (x + 0.5f) / SUN_TEXTURE_SIZE * 2.0f - 1.0f;
            float dy = (y + 0.5f) / SUN_TEXTURE_SIZE * 2.0f - 1.0f;
            float r = sqrtf(dx * dx + dy * dy);
            float disc = Clamp((DISC_RADIUS + DISC_EDGE - r) / DISC_EDGE, 0.0f, 1.0f);
            float corona = Clamp(1.0f - r, 0.0f, 1.0f);
            corona = corona * corona * corona * 0.55f;
            float a = Clamp(disc + corona, 0.0f, 1.0f);
            sun.pixels[y * SUN_TEXTURE_SIZE + x] = PackARGB(1.0f, 1.0f, Lerp(0.75f, 1.0f, disc), a);
        }
    }
    sunTexture = UploadTexture(sun, true, false, TEX_FILTER_TRILINEAR, TEX_FILTER_LINEAR);

    // Glint: a four-pointed star with a soft core.
    ImageData glint;
    glint.width = GLINT_TEXTURE_SIZE;
    glint.height = GLINT_TEXTURE_SIZE;
    glint.pixels.resize(GLINT_TEXTURE_SIZE * GLINT_TEXTURE_SIZE);
    for (int y = 0; y < GLINT_TEXTURE_SIZE; y++) {
        for (int x = 0; x < GLINT_TEXTURE_SIZE; x++) {
            float dx = (x + 0.5f) / GLINT_TEXTURE_SIZE * 2.0f - 1.0f;
            float dy = (y + 0.5f) / GLINT_TEXTURE_SIZE * 2.0f - 1.0f;
            float core = expf(-(dx * dx + dy * dy) * 30.0f);
            float rays = std::max(expf(-fabsf(dx) * 22.0f) * (1.0f - fabsf(dy)),
                                  expf(-fabsf(dy) * 22.0f) * (1.0f - fabsf(dx)));
            float a = Clamp(core + rays * 0.8f, 0.0f, 1.0f);
            glint.pixels[y * GLINT_TEXTURE_SIZE + x] = PackARGB(1.0f, 1.0f, 1.0f, a);
        }
    }
    glintTexture = UploadTexture(glint, true, false, TEX_FILTER_TRILINEAR, TEX_FILTER_LINEAR);
}

// --- Sky

void BuildSkyDome()
{
    for (int ring = 0; ring < SKY_RINGS; ring++) {
        float elevation = SKY_RING_ELEVATIONS[ring] * DEG;
        for (int s = 0; s <= SKY_SEGMENTS; s++) {
            float azimuth = 2.0f * PI * s / SKY_SEGMENTS;
            int i = ring * (SKY_SEGMENTS + 1) + s;
            skyDirections[i] = Vec3(cosf(elevation) * sinf(azimuth), sinf(elevation), cosf(elevation) * cosf(azimuth));
            skyElevations[i] = elevation;
        }
    }
    std::vector<uint16_t> indices;
    for (int ring = 0; ring + 1 < SKY_RINGS; ring++) {
        for (int s = 0; s < SKY_SEGMENTS; s++) {
            uint16_t a = static_cast<uint16_t>(ring * (SKY_SEGMENTS + 1) + s);
            uint16_t b = static_cast<uint16_t>(a + 1);
            uint16_t c = static_cast<uint16_t>(a + SKY_SEGMENTS + 1);
            uint16_t d = static_cast<uint16_t>(c + 1);
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    }
    skyStream = BuildIndexStream(indices);
}

// The sky's colour in a direction; at and below the horizon it is the fog colour of that direction, as the vertex
// shaders compute it (with the unflattened direction, as they do).
Vec3 SkyColor(const Vec3& direction, float elevation)
{
    const Environment& e = environment;
    float sunward = std::max(Dot(direction, sunFlat), 0.0f);
    Vec3 horizon = Lerp(e.skyHorizon, e.skyHorizonSun, sunward * sunward);

    float glow = Clamp((Dot(direction, e.sunDirection) - COS_GLOW_RADIUS) / (1.0f - COS_GLOW_RADIUS), 0.0f, 1.0f);
    Vec3 glowColor = e.sunGlow * (e.sunGlowStrength * glow * glow);

    Vec3 sky = horizon;
    if (elevation > 0.0f && elevation < SKY_MID_ELEVATION) {
        float t = elevation / SKY_MID_ELEVATION;
        sky = Lerp(horizon, e.skyMid, t * (2.0f - t));
    } else if (elevation >= SKY_MID_ELEVATION) {
        float t = (elevation - SKY_MID_ELEVATION) / (PI * 0.5f - SKY_MID_ELEVATION);
        sky = Lerp(e.skyMid, e.skyZenith, t);
    }
    return sky + glowColor;
}

// --- Frame constants

Mat4 ProjectionClip(const Camera& c)
{
    float aspect = static_cast<float>(SCREEN_WIDTH) / SCREEN_HEIGHT;
    float sy = 1.0f / tanf(c.fovY * 0.5f);
    float sx = sy / aspect;
    float a = c.farZ / (c.nearZ - c.farZ);
    Mat4 m = {};
    m.m[0][0] = sx;
    m.m[1][1] = sy;
    m.m[2][2] = a;
    m.m[2][3] = -1.0f;
    m.m[3][2] = a * c.nearZ;
    return m;
}

Mat4 ViewportMatrix()
{
    Mat4 m = {};
    m.m[0][0] = SCREEN_WIDTH * 0.5f;
    m.m[1][1] = -SCREEN_HEIGHT * 0.5f;
    m.m[2][2] = Z_SCALE;
    m.m[3][0] = SCREEN_WIDTH * 0.5f;
    m.m[3][1] = SCREEN_HEIGHT * 0.5f;
    m.m[3][3] = 1.0f;
    return m;
}

// Planes (xyz = normal, w = distance) whose positive side is inside the view.
void ExtractFrustum(const Mat4& m)
{
    auto column = [&](int c) {
        return Vec4{m.m[0][c], m.m[1][c], m.m[2][c], m.m[3][c]};
    };
    auto combine = [](const Vec4& a, const Vec4& b, float s) {
        Vec4 r = {a.x + b.x * s, a.y + b.y * s, a.z + b.z * s, a.w + b.w * s};
        float l = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z);
        return Vec4{r.x / l, r.y / l, r.z / l, r.w / l};
    };
    Vec4 x = column(0);
    Vec4 y = column(1);
    Vec4 z = column(2);
    Vec4 w = column(3);
    frustumPlanes[0] = combine(w, x, 1.0f);
    frustumPlanes[1] = combine(w, x, -1.0f);
    frustumPlanes[2] = combine(w, y, 1.0f);
    frustumPlanes[3] = combine(w, y, -1.0f);
    frustumPlanes[4] = combine(z, z, 0.0f);
    frustumPlanes[5] = combine(w, z, -1.0f);
}

bool SphereVisible(const Vec3& center, float radius)
{
    for (const Vec4& plane : frustumPlanes) {
        if (plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w < -radius)
            return false;
    }
    return true;
}

// Beyond this distance a fogged draw is entirely the horizon colour of its direction, the same colour as the sky
// dome behind it, so it can be skipped. Additive and unfogged draws have no such distance.
float FullFogDistance(const Material& material)
{
    if (!material.fog || material.blend == Blend::ADDITIVE || material.fogDensity <= 0.0f)
        return 1e30f;
    return environment.fogStart + (environment.fogEnd - environment.fogStart) / material.fogDensity;
}

// Distances a draw is visible between: the fog limit and the material's distance fade (where the alpha has
// crossed 0.5 for good).
struct DistanceRange {
    float nearest = 0.0f;
    float farthest = 1e30f;
};

DistanceRange VisibleRange(const Material& material)
{
    DistanceRange range;
    range.farthest = FullFogDistance(material);
    if (material.minDistance > 0.0f)
        range.nearest = material.minDistance * (1.0f - FADE_BAND * 0.5f);
    if (material.maxDistance > 0.0f)
        range.farthest = std::min(range.farthest, material.maxDistance * (1.0f + FADE_BAND * 0.5f));
    return range;
}

// A bounding sphere (local centre, world radius) is in the view and not entirely outside the visible range.
bool PartVisible(const Mat4& model, const Vec3& center, float radius, const DistanceRange& range)
{
    Vec3 world = TransformPoint(model, center);
    float distance = Length(world - camera.position);
    if (distance - radius > range.farthest || distance + radius < range.nearest)
        return false;
    return SphereVisible(world, radius);
}

void PushFrameConstants()
{
    const Environment& e = environment;
    float a[REG_FRAME_A_COUNT * 4] = {};
    memcpy(a, viewProjScreen.m, sizeof(viewProjScreen.m));
    const float values[] = {
        camera.position.x, camera.position.y, camera.position.z, 1.0f,           // c4
        e.sunDirection.x, e.sunDirection.y, e.sunDirection.z, 0.0f,              // c5
        e.sunColor.x, e.sunColor.y, e.sunColor.z, 1.0f,                          // c6
        0.0f, 0.0f, 0.0f, 0.0f,                                                  // c7 (unused)
        0.0f, 0.0f, 0.0f, 0.0f,                                                  // c8 (unused)
        0.0f, 0.0f, 0.0f, 0.0f,                                                  // c9 (unused)
        0.0f, 1.0f, 0.5f, 2.0f,                                                  // c10
        e.skyHorizon.x, e.skyHorizon.y, e.skyHorizon.z, 1.0f,                    // c11
    };
    memcpy(a + 16, values, sizeof(values));

    float cosGlow = COS_GLOW_RADIUS;
    Vec3 glow = e.sunGlow * e.sunGlowStrength;
    Vec3 horizonDelta = e.skyHorizonSun - e.skyHorizon;
    const float b[REG_FRAME_B_COUNT * 4] = {
        horizonDelta.x, horizonDelta.y, horizonDelta.z, 0.0f,                    // c26
        sunFlat.x, sunFlat.y, sunFlat.z, 0.0f,                                   // c27
        1.0f / (1.0f - cosGlow), -cosGlow / (1.0f - cosGlow), 0.0f, 0.0f,        // c28
        glow.x, glow.y, glow.z, 1.0f,                                            // c29
    };
    uint32_t* p = BeginPush(128);
    p = PushConstants(p, REG_VIEW_PROJ, a, REG_FRAME_A_COUNT);
    p = PushConstants(p, REG_FRAME_B, b, REG_FRAME_B_COUNT);
    pb_end(p);
}

// --- Start-up

void SetUpGpu()
{
    uint32_t* p = pb_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
                 (NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM << 0) |
                     (NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV << 2));
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
    pb_end(p);

    p = pb_begin();
    p = PushProgram(p, MESH_PROGRAM_SLOT, MESH_PROGRAM, MESH_PROGRAM_LENGTH);
    pb_end(p);
    p = pb_begin();
    p = PushProgram(p, UNLIT_PROGRAM_SLOT, UNLIT_PROGRAM, UNLIT_PROGRAM_LENGTH);
    pb_end(p);
    p = pb_begin();
    p = PushProgram(p, SCREEN_PROGRAM_SLOT, SCREEN_PROGRAM, SCREEN_PROGRAM_LENGTH);
    pb_end(p);

    p = pb_begin();
    p = PushPlainCombiner(p);
    pb_end(p);

    p = pb_begin();
    p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
    p = pb_push1(p, NV097_SET_SPECULAR_ENABLE, 1);
    p = pb_push1(p, NV097_SET_FOG_ENABLE, 0);
    p = pb_push1(p, NV097_SET_DEPTH_FUNC, NV097_SET_DEPTH_FUNC_V_LEQUAL);
    p = pb_push1(p, NV097_SET_FRONT_FACE, NV097_SET_FRONT_FACE_V_CCW);
    p = pb_push1(p, NV097_SET_CULL_FACE, NV097_SET_CULL_FACE_V_BACK);
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);

    // Every draw uses the 36-byte Vertex layout; unused attributes are off (float, size 0).
    pb_push(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, ATTR_COUNT);
    p++;
    for (int i = 0; i < ATTR_COUNT; i++) {
        uint32_t format = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
        if (i == ATTR_POSITION || i == ATTR_NORMAL)
            format |= (3 << 4) | (sizeof(Vertex) << 8);
        else if (i == ATTR_TEXCOORD)
            format |= (2 << 4) | (sizeof(Vertex) << 8);
        else if (i == ATTR_COLOR)
            format = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D | (4 << 4) | (sizeof(Vertex) << 8);
        *p++ = format;
    }
    pb_end(p);
}

// --- Triangle order

// Score of a vertex for the cache optimiser (Tom Forsyth, "Linear-Speed Vertex Cache Optimisation", 2006):
// vertices in the cache score higher (the last triangle's three a fixed amount), and vertices with few triangles
// left get a boost so they are finished off.
float VertexScore(int cachePosition, int trianglesLeft)
{
    // Both terms come from small tables built once (powf and sqrtf per call made mesh loading slow).
    constexpr int VALENCE_TABLE = 32;
    static float cacheScore[VERTEX_CACHE_SIZE];
    static float valenceScore[VALENCE_TABLE];
    static bool built = false;
    if (!built) {
        for (int i = 0; i < VERTEX_CACHE_SIZE; i++) {
            float t = 1.0f - static_cast<float>(i - 3) / (VERTEX_CACHE_SIZE - 3);
            cacheScore[i] = i < 3 ? 0.75f : powf(t, 1.5f);
        }
        for (int i = 1; i < VALENCE_TABLE; i++)
            valenceScore[i] = 2.0f / sqrtf(static_cast<float>(i));
        built = true;
    }
    if (trianglesLeft == 0)
        return -1.0f;
    float score = cachePosition >= 0 ? cacheScore[cachePosition] : 0.0f;
    if (trianglesLeft < VALENCE_TABLE)
        return score + valenceScore[trianglesLeft];
    return score + 2.0f / sqrtf(static_cast<float>(trianglesLeft));
}

// Reorders a triangle list so the GPU's post-transform cache shades each vertex as few times as possible.
void OptimizeTriangleOrder(std::vector<uint16_t>& indices, int vertexCount)
{
    int triangleCount = static_cast<int>(indices.size() / 3);
    std::vector<int> trianglesLeft(vertexCount, 0);
    for (int i = 0; i < triangleCount * 3; i++)
        trianglesLeft[indices[i]]++;
    std::vector<int> firstTriangle(vertexCount + 1, 0);
    for (int v = 0; v < vertexCount; v++)
        firstTriangle[v + 1] = firstTriangle[v] + trianglesLeft[v];
    std::vector<int> vertexTriangles(triangleCount * 3);
    std::vector<int> fill(firstTriangle.begin(), firstTriangle.end() - 1);
    for (int t = 0; t < triangleCount; t++) {
        for (int c = 0; c < 3; c++)
            vertexTriangles[fill[indices[t * 3 + c]]++] = t;
    }

    std::vector<int> cachePosition(vertexCount, -1);
    std::vector<float> vertexScore(vertexCount);
    for (int v = 0; v < vertexCount; v++)
        vertexScore[v] = VertexScore(-1, trianglesLeft[v]);
    std::vector<float> triangleScore(triangleCount);
    std::vector<bool> emitted(triangleCount, false);
    for (int t = 0; t < triangleCount; t++) {
        triangleScore[t] = vertexScore[indices[t * 3]] + vertexScore[indices[t * 3 + 1]] +
                           vertexScore[indices[t * 3 + 2]];
    }

    std::vector<uint16_t> out;
    out.reserve(indices.size());
    // The simulated cache, most recent first; newCache is built from it for each emitted triangle.
    std::vector<int> cache;
    std::vector<int> newCache;
    cache.reserve(VERTEX_CACHE_SIZE + 3);
    newCache.reserve(VERTEX_CACHE_SIZE + 3);
    int scan = 0;
    int best = -1;
    while (static_cast<int>(out.size()) < triangleCount * 3) {
        if (best < 0) {
            // Nothing in the cache to continue from: take the best triangle not yet emitted.
            while (emitted[scan])
                scan++;
            best = scan;
            for (int t = scan; t < triangleCount; t++) {
                if (!emitted[t] && triangleScore[t] > triangleScore[best])
                    best = t;
            }
        }
        emitted[best] = true;
        newCache.clear();
        for (int c = 0; c < 3; c++) {
            int v = indices[best * 3 + c];
            out.push_back(static_cast<uint16_t>(v));
            newCache.push_back(v);

            // Each vertex's list keeps its remaining triangles first: move this one behind them.
            int* list = &vertexTriangles[firstTriangle[v]];
            int active = trianglesLeft[v];
            for (int i = 0; i < active; i++) {
                if (list[i] == best) {
                    list[i] = list[active - 1];
                    list[active - 1] = best;
                    break;
                }
            }
            trianglesLeft[v]--;
        }
        for (int v : cache) {
            if (std::find(newCache.begin(), newCache.end(), v) == newCache.end())
                newCache.push_back(v);
        }
        for (size_t i = 0; i < newCache.size(); i++) {
            int v = newCache[i];
            if (i < static_cast<size_t>(VERTEX_CACHE_SIZE)) {
                cachePosition[v] = static_cast<int>(i);
            } else {
                cachePosition[v] = -1;
                vertexScore[v] = VertexScore(-1, trianglesLeft[v]);
            }
        }
        if (newCache.size() > static_cast<size_t>(VERTEX_CACHE_SIZE))
            newCache.resize(VERTEX_CACHE_SIZE);
        cache.swap(newCache);

        // Rescore the cached vertices' remaining triangles and continue with the best of them.
        best = -1;
        float bestScore = -1.0f;
        for (int v : cache)
            vertexScore[v] = VertexScore(cachePosition[v], trianglesLeft[v]);
        for (int v : cache) {
            for (int i = 0; i < trianglesLeft[v]; i++) {
                int t = vertexTriangles[firstTriangle[v] + i];
                float score = vertexScore[indices[t * 3]] + vertexScore[indices[t * 3 + 1]] +
                              vertexScore[indices[t * 3 + 2]];
                triangleScore[t] = score;
                if (score > bestScore) {
                    bestScore = score;
                    best = t;
                }
            }
        }
    }
    indices.swap(out);
}

// --- Mesh chunks

// Bounding sphere (box centre, farthest vertex) of the vertices a triangle list uses.
void BoundingSphere(const std::vector<Vertex>& vertices, const std::vector<uint16_t>& indices, Vec3& center,
                    float& radius)
{
    Vec3 lo(1e30f, 1e30f, 1e30f);
    Vec3 hi(-1e30f, -1e30f, -1e30f);
    for (uint16_t i : indices) {
        const Vertex& v = vertices[i];
        lo = Vec3(std::min(lo.x, v.px), std::min(lo.y, v.py), std::min(lo.z, v.pz));
        hi = Vec3(std::max(hi.x, v.px), std::max(hi.y, v.py), std::max(hi.z, v.pz));
    }
    center = (lo + hi) * 0.5f;
    float radiusSq = 0.0f;
    for (uint16_t i : indices) {
        const Vertex& v = vertices[i];
        radiusSq = std::max(radiusSq, LengthSq(Vec3(v.px, v.py, v.pz) - center));
    }
    radius = sqrtf(radiusSq);
}

float Axis(const Vertex& v, int axis)
{
    if (axis == 0)
        return v.px;
    return axis == 1 ? v.py : v.pz;
}

// Splits triangles (by their centres) in halves along the widest axis until each part has at most CHUNK_TRIANGLES:
// chunks stay balanced, and small where the mesh is dense.
void SplitTriangles(const std::vector<Vertex>& vertices, const std::vector<uint16_t>& indices,
                    std::vector<int>& triangles, int first, int count, std::vector<std::vector<uint16_t>>& chunks)
{
    if (count <= CHUNK_TRIANGLES) {
        std::vector<uint16_t> chunk;
        for (int i = first; i < first + count; i++) {
            int t = triangles[i];
            chunk.insert(chunk.end(), indices.begin() + t * 3, indices.begin() + t * 3 + 3);
        }
        chunks.push_back(std::move(chunk));
        return;
    }
    auto centre = [&](int t, int axis) {
        return Axis(vertices[indices[t * 3]], axis) + Axis(vertices[indices[t * 3 + 1]], axis) +
               Axis(vertices[indices[t * 3 + 2]], axis);
    };
    float lo[3] = {1e30f, 1e30f, 1e30f};
    float hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int i = first; i < first + count; i++) {
        for (int a = 0; a < 3; a++) {
            lo[a] = std::min(lo[a], centre(triangles[i], a));
            hi[a] = std::max(hi[a], centre(triangles[i], a));
        }
    }
    int axis = 0;
    for (int a = 1; a < 3; a++) {
        if (hi[a] - lo[a] > hi[axis] - lo[axis])
            axis = a;
    }
    int half = count / 2;
    std::nth_element(triangles.begin() + first, triangles.begin() + first + half, triangles.begin() + first + count,
                     [&](int a, int b) {
                         return centre(a, axis) < centre(b, axis);
                     });
    SplitTriangles(vertices, indices, triangles, first, half, chunks);
    SplitTriangles(vertices, indices, triangles, first + half, count - half, chunks);
}

// The mesh's triangles as chunks of at most CHUNK_TRIANGLES (one chunk for a small mesh).
std::vector<std::vector<uint16_t>> SplitIntoChunks(const std::vector<Vertex>& vertices,
                                                   const std::vector<uint16_t>& indices)
{
    int count = static_cast<int>(indices.size() / 3);
    std::vector<int> triangles(count);
    for (int t = 0; t < count; t++)
        triangles[t] = t;
    std::vector<std::vector<uint16_t>> chunks;
    SplitTriangles(vertices, indices, triangles, 0, count, chunks);
    return chunks;
}

} // namespace

// --- Resources

TextureId CreateTexture(const ImageData& image, bool mipmaps, bool wrap)
{
    LoadTimer timer(loadTextureMs);
    framesSinceUpload = 0;
    uint32_t minFilter = mipmaps ? TEX_FILTER_TRILINEAR : TEX_FILTER_LINEAR;
    return UploadTexture(image, mipmaps, wrap, minFilter, TEX_FILTER_LINEAR);
}

MeshId CreateMesh(const MeshData& mesh)
{
    LoadTimer timer(loadMeshMs);
    framesSinceUpload = 0;
    if (mesh.vertices.empty() || mesh.indices.empty() || mesh.vertices.size() > 65535)
        return NO_MESH;
    uint32_t bytes = mesh.vertices.size() * sizeof(Vertex);
    void* memory = GpuAlloc(bytes);
    if (memory == nullptr)
        return NO_MESH;
    memcpy(memory, mesh.vertices.data(), bytes);
    FlushWrites();

    GpuMesh m;
    m.vertexOffset = GpuAddress(memory);
    std::vector<uint16_t> all(mesh.indices.begin(), mesh.indices.begin() + mesh.indices.size() / 3 * 3);
    BoundingSphere(mesh.vertices, all, m.center, m.radius);
    for (std::vector<uint16_t>& indices : SplitIntoChunks(mesh.vertices, all)) {
        MeshChunk chunk;
        BoundingSphere(mesh.vertices, indices, chunk.center, chunk.radius);
        OptimizeTriangleOrder(indices, static_cast<int>(mesh.vertices.size()));
        chunk.stream = BuildIndexStream(indices);
        m.chunks.push_back(std::move(chunk));
    }
    meshes.push_back(std::move(m));
    return static_cast<MeshId>(meshes.size() - 1);
}

// --- Frame set-up

void BeginFrame(const Camera& cam, const Environment& env)
{
    frameStartTime = KeQueryPerformanceCounter();
    billboardCount = 0;
    billboardPixels = 0.0f;
    // During loading (frames of the loading screen) log the upload time so far whenever more was uploaded.
    int resources = static_cast<int>(textures.size() + meshes.size());
    if (resources != loggedResources) {
        DbgPrint("render: CreateTexture %d textures %d ms, CreateMesh %d meshes %d ms\n",
                 static_cast<int>(textures.size()), static_cast<int>(loadTextureMs), static_cast<int>(meshes.size()),
                 static_cast<int>(loadMeshMs));
        loggedResources = resources;
    }
    camera = cam;
    environment = env;
    environment.sunDirection = Normalize(env.sunDirection);
    sunFlat = Normalize(Vec3(environment.sunDirection.x, 0.0f, environment.sunDirection.z));
    if (LengthSq(sunFlat) < 0.5f)
        sunFlat = Vec3(0.0f, 0.0f, 1.0f);

    // Restart the push buffer at its head (waits until the GPU has read the previous frame).
    uint64_t waitStart = KeQueryPerformanceCounter();
    pb_reset();
    float gpuWaitMs = MillisecondsSince(waitStart);
    pushHead = pb_begin();
    pb_end(pushHead);

    // Never draw into the buffer being scanned out. pbkit flips to finished frames at vertical blanks, one per
    // blank, but when two finished frames wait in its queue the next back buffer is the one on screen (half-drawn
    // frames). Then wait for blanks until the display has moved on; it never returns to this buffer before this
    // frame is finished. The wait is bounded by blanks and by time, without blocking on the blank event: xemu
    // sometimes skips flips for several blanks or stops its blanks altogether, and that must not hang the game.
    uint32_t target = reinterpret_cast<uint32_t>(pb_back_buffer()) & 0x03FFFFFF;
    uint64_t displayWait = KeQueryPerformanceCounter();
    DWORD firstBlank = pb_get_vbl_counter();
    while ((VIDEOREG(PCRTC_START) & 0x03FFFFFF) == target) {
        if (pb_get_vbl_counter() - firstBlank >= MAX_DISPLAY_WAIT_BLANKS ||
            MillisecondsSince(displayWait) > MAX_DISPLAY_WAIT_MS) {
            displayTimeouts++;
            if (displayTimeouts == 1)
                DbgPrint("render: the display did not flip in time; drawing anyway\n");
            break;
        }
        NtYieldExecution();
    }
    gpuWaitMs += MillisecondsSince(displayWait);
    pb_target_back_buffer();
    state = StateCache(); // pbkit's surface set-up may change depth and stencil state behind the cache

    // pbkit's surface set-up turns on W-buffering; depth here is the shader's z/w (0..2^24-1), the same on every
    // NV2A and in xemu. Stencil writes and perspective-correct texturing stay on.
    uint32_t* p = BeginPush(16);
    p = pb_push1(p, NV097_SET_CONTROL0,
                 NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE | NV097_SET_CONTROL0_TEXTURE_PERSPECTIVE_ENABLE);
    pb_end(p);

    dynamicIndex ^= 1;
    dynamicUsed = 0;
    batch2D.count = 0;
    frameStats = FrameStats();
    frameStats.gpuWaitMs = gpuWaitMs;

    // Clear colour (to the horizon, in case nothing covers a pixel), depth and stencil.
    Vec3 clear = environment.skyHorizon;
    p = BeginPush(16);
    pb_push(p, NV20_TCL_PRIMITIVE_3D_CLEAR_VALUE_HORIZ, 2);
    p[1] = static_cast<uint32_t>(SCREEN_WIDTH - 1) << 16;
    p[2] = static_cast<uint32_t>(SCREEN_HEIGHT - 1) << 16;
    p += 3;
    pb_push(p, NV20_TCL_PRIMITIVE_3D_CLEAR_VALUE_DEPTH, 3);
    p[1] = 0xFFFFFF00;
    p[2] = PackARGB(clear.x, clear.y, clear.z);
    p[3] = 0xF3;
    p += 4;
    pb_end(p);

    Mat4 view = MatView(camera.position, camera.rotation);
    viewProjClip = view * ProjectionClip(camera);
    viewProjScreen = viewProjClip * ViewportMatrix();
    ExtractFrustum(viewProjClip);
    cameraRight = QuatRight(camera.rotation);
    cameraUp = QuatUp(camera.rotation);
    cameraForward = QuatForward(camera.rotation);
    PushFrameConstants();
}

void DrawSky()
{
    int first = 0;
    Vertex* v = AllocDynamic(SKY_VERTICES + 4, first);
    if (v == nullptr)
        return;
    float radius = camera.farZ * 0.5f;
    for (int i = 0; i < SKY_VERTICES; i++) {
        Vec3 c = SkyColor(skyDirections[i], skyElevations[i]);
        SetVertex(v[i], skyDirections[i] * radius, 0.5f, 0.5f, PackARGB(c.x, c.y, c.z));
    }

    // The sun sprite, facing the camera, at the same distance.
    const Environment& e = environment;
    Vec3 center = e.sunDirection * radius;
    Vec3 side = Normalize(Cross(e.sunDirection, Vec3(0.0f, 1.0f, 0.0f)));
    if (LengthSq(side) < 0.5f)
        side = Vec3(1.0f, 0.0f, 0.0f);
    Vec3 up = Cross(side, e.sunDirection);
    float half = radius * tanf(SUN_SPRITE_HALF_ANGLE);
    uint32_t sunColor = PackARGB(e.sunDisc.x, e.sunDisc.y, e.sunDisc.z);
    SetVertex(v[SKY_VERTICES + 0], center - side * half + up * half, 0.0f, 0.0f, sunColor);
    SetVertex(v[SKY_VERTICES + 1], center + side * half + up * half, 1.0f, 0.0f, sunColor);
    SetVertex(v[SKY_VERTICES + 2], center + side * half - up * half, 1.0f, 1.0f, sunColor);
    SetVertex(v[SKY_VERTICES + 3], center - side * half - up * half, 0.0f, 1.0f, sunColor);
    FlushWrites();

    Material material;
    material.unlit = true;
    material.fog = false;
    material.cullBackFaces = false;
    Mat4 model = MatTranslate(camera.position);
    uint32_t base = DynamicBase() + first * sizeof(Vertex);
    uint32_t* p = BeginPush(128);
    p = SetProgram(p, Program::UNLIT);
    p = SetBlend(p, Blend::OPAQUE, DepthMode::OFF);
    p = SetCull(p, false);
    p = SetTexture(p, whiteTexture);
    p = SetDetail(p, NO_TEXTURE);
    p = SetVertexBase(p, base);
    p = PushDrawConstants(p, model, material, Blend::OPAQUE);
    pb_end(p);
    DrawStream(skyStream);
    frameStats.drawCalls++;

    p = BeginPush(128);
    p = SetBlend(p, Blend::ADDITIVE, DepthMode::OFF);
    p = SetTexture(p, sunTexture);
    p = PushDrawConstants(p, model, material, Blend::ADDITIVE);
    pb_end(p);
    DrawQuads(SKY_VERTICES, 4);
}

// --- 3D drawing

void DrawMesh(MeshId id, const Mat4& model, const Material& material)
{
    if (id < 0 || id >= static_cast<int>(meshes.size()))
        return;
    Flush2D();
    const GpuMesh& mesh = meshes[id];
    float scaleSq = 0.0f;
    for (int row = 0; row < 3; row++) {
        Vec3 axis(model.m[row][0], model.m[row][1], model.m[row][2]);
        scaleSq = std::max(scaleSq, LengthSq(axis));
    }
    float scale = sqrtf(scaleSq);
    DistanceRange range = VisibleRange(material);
    if (!PartVisible(model, mesh.center, mesh.radius * scale, range)) {
        frameStats.culled++;
        return;
    }
    uint32_t* p = BeginPush(128);
    p = SetProgram(p, material.unlit ? Program::UNLIT : Program::MESH);
    p = SetBlend(p, material.blend, DepthMode::TEST);
    p = SetCull(p, material.cullBackFaces);
    p = SetTexture(p, material.texture);
    p = SetDetail(p, material.detail);
    p = SetVertexBase(p, mesh.vertexOffset);
    p = PushDrawConstants(p, model, material, material.blend);
    pb_end(p);
    for (const MeshChunk& chunk : mesh.chunks) {
        if (mesh.chunks.size() == 1 || PartVisible(model, chunk.center, chunk.radius * scale, range))
            DrawStream(chunk.stream);
    }
    frameStats.drawCalls++;
}

void DrawBillboards(TextureId atlas, int atlasColumns, int atlasRows, const Billboard* billboards, int count,
                    Blend blend)
{
    if (count <= 0 || billboards == nullptr)
        return;
    Flush2D();
    int first = 0;
    Vertex* v = AllocDynamic(count * 4, first);
    if (v == nullptr) {
        count = (DYNAMIC_VERTICES - dynamicUsed) / 4;
        v = AllocDynamic(count * 4, first);
        if (v == nullptr || count == 0)
            return;
    }

    billboardCount += count;
    billboardOrder.resize(count);
    for (int i = 0; i < count; i++)
        billboardOrder[i] = i;
    if (blend == Blend::ALPHA) {
        billboardDepth.resize(count);
        for (int i = 0; i < count; i++)
            billboardDepth[i] = Dot(billboards[i].position - camera.position, cameraForward);
        std::sort(billboardOrder.begin(), billboardOrder.end(), [&](int a, int b) {
            return billboardDepth[a] > billboardDepth[b];
        });
    }

    int columns = std::max(atlasColumns, 1);
    int rows = std::max(atlasRows, 1);
    float du = 1.0f / columns;
    float dv = 1.0f / rows;
    float pixelAngle = 2.0f * tanf(camera.fovY * 0.5f) / SCREEN_HEIGHT; // world size of a pixel at 1 m
    for (int i = 0; i < count; i++) {
        const Billboard& b = billboards[billboardOrder[i]];
        // Close to the camera a sprite is capped at MAX_BILLBOARD_SCREEN of the screen height and fades out
        // (a screen-filling smoke puff costs a frame's fill rate and hides everything).
        float depth = Dot(b.position - camera.position, cameraForward);
        float maxSize = std::max(depth, 0.0f) * pixelAngle * SCREEN_HEIGHT * MAX_BILLBOARD_SCREEN;
        float half = std::min(b.size, maxSize) * 0.5f;
        if (depth > 0.0f) {
            float pixels = half * 2.0f / (depth * pixelAngle);
            billboardPixels += pixels * pixels;
        }
        float nearFade = Clamp((depth - BILLBOARD_FADE_NEAR) / (BILLBOARD_FADE_FAR - BILLBOARD_FADE_NEAR), 0.0f, 1.0f);
        uint32_t alpha = static_cast<uint32_t>((b.color >> 24) * nearFade);
        uint32_t color = (b.color & 0x00FFFFFFu) | (alpha << 24);
        float c = cosf(b.rotation) * half;
        float s = sinf(b.rotation) * half;
        Vec3 right = cameraRight * c + cameraUp * s;
        Vec3 up = cameraUp * c - cameraRight * s;
        int frame = std::max(b.frame, 0) % (columns * rows);
        float u0 = (frame % columns) * du;
        float v0 = (frame / columns) * dv;
        Vertex* q = v + i * 4;
        SetVertex(q[0], b.position - right + up, u0, v0, color);
        SetVertex(q[1], b.position + right + up, u0 + du, v0, color);
        SetVertex(q[2], b.position + right - up, u0 + du, v0 + dv, color);
        SetVertex(q[3], b.position - right - up, u0, v0 + dv, color);
    }
    FlushWrites();
    BeginDynamicQuads(atlas, blend);
    DrawQuads(first, count * 4);
}

void DrawStreaks(TextureId texture, const Streak* streaks, int count, Blend blend)
{
    if (count <= 0 || streaks == nullptr)
        return;
    Flush2D();
    int first = 0;
    Vertex* v = AllocDynamic(count * 4, first);
    if (v == nullptr) {
        count = (DYNAMIC_VERTICES - dynamicUsed) / 4;
        v = AllocDynamic(count * 4, first);
        if (v == nullptr || count == 0)
            return;
    }
    float pixelAngle = 2.0f * tanf(camera.fovY * 0.5f) / SCREEN_HEIGHT; // world size of a pixel at 1 m
    for (int i = 0; i < count; i++) {
        const Streak& s = streaks[i];
        Vec3 along = s.to - s.from;
        Vec3 middle = (s.from + s.to) * 0.5f;
        Vec3 side = Normalize(Cross(along, middle - camera.position));
        if (LengthSq(side) < 0.5f)
            side = cameraRight;
        // Never thinner than MIN_STREAK_PIXELS on screen, so distant tracers stay visible.
        float distance = Length(middle - camera.position);
        float width = std::max(s.width, distance * pixelAngle * MIN_STREAK_PIXELS);
        side = side * (width * 0.5f);
        Vertex* q = v + i * 4;
        SetVertex(q[0], s.from - side, 0.0f, 0.0f, s.color);
        SetVertex(q[1], s.from + side, 1.0f, 0.0f, s.color);
        SetVertex(q[2], s.to + side, 1.0f, 1.0f, s.color);
        SetVertex(q[3], s.to - side, 0.0f, 1.0f, s.color);
    }
    FlushWrites();
    BeginDynamicQuads(texture, blend);
    DrawQuads(first, count * 4);
}

void DrawSeaGlints(float time, int count, float seaLevel)
{
    const Vec3& sun = environment.sunDirection;
    float height = camera.position.y - seaLevel;
    if (count <= 0 || height < 0.5f || sun.y < 0.05f)
        return;
    count = std::min(count, MAX_GLINTS);

    // The sun's reflection on the sea: where the ray toward the sun's mirror image meets the water. The glint path
    // runs from there toward the camera and away to the horizon, widening with distance.
    float reach = height / sun.y * sqrtf(sun.x * sun.x + sun.z * sun.z);
    Vec3 across(-sunFlat.z, 0.0f, sunFlat.x);
    float pixelAngle = 2.0f * tanf(camera.fovY * 0.5f) / SCREEN_HEIGHT;
    uint32_t color = PackARGB(environment.sunDisc.x, environment.sunDisc.y, environment.sunDisc.z);
    glints.resize(count);
    int used = 0;
    for (int i = 0; i < count; i++) {
        // Each sparkle lives GLINT_LIFE seconds, then reappears elsewhere; their phases are spread out.
        float phase = static_cast<float>(i) / count;
        float cycles = time / GLINT_LIFE + phase;
        float cycle = floorf(cycles);
        Rng rng(static_cast<uint32_t>(cycle) * 2654435761u + static_cast<uint32_t>(i) * 40503u + 1u);
        float along = 0.3f + 2.2f * rng.Uniform() * rng.Uniform();
        float side = (rng.Uniform() - 0.5f) * 0.35f * along;
        float distance = std::min(reach * along, camera.farZ * 0.8f);
        Vec3 position = Vec3(camera.position.x, seaLevel + 0.05f, camera.position.z) + sunFlat * distance +
                        across * (side * reach);
        float centre = (along - 1.0f) * 1.2f;
        float strength = expf(-centre * centre) * sinf((cycles - cycle) * PI);
        if (strength < 0.05f)
            continue;
        float range = Length(position - camera.position);
        Billboard& g = glints[used++];
        g.position = position;
        g.size = range * pixelAngle * GLINT_PIXELS * (0.6f + 0.8f * rng.Uniform());
        g.rotation = rng.Uniform() * 0.5f;
        g.color = (color & 0x00FFFFFFu) | (static_cast<uint32_t>(strength * 255.0f) << 24);
        g.frame = 0;
    }
    DrawBillboards(glintTexture, 1, 1, glints.data(), used, Blend::ADDITIVE);
}

// --- 2D overlay

int ScreenWidth()
{
    return SCREEN_WIDTH;
}

int ScreenHeight()
{
    return SCREEN_HEIGHT;
}

bool Project(const Vec3& world, float& sx, float& sy)
{
    const Mat4& m = viewProjClip;
    float x = world.x * m.m[0][0] + world.y * m.m[1][0] + world.z * m.m[2][0] + m.m[3][0];
    float y = world.x * m.m[0][1] + world.y * m.m[1][1] + world.z * m.m[2][1] + m.m[3][1];
    float w = world.x * m.m[0][3] + world.y * m.m[1][3] + world.z * m.m[2][3] + m.m[3][3];
    if (w <= 1e-4f) {
        sx = 0.0f;
        sy = 0.0f;
        return false;
    }
    sx = (x / w * 0.5f + 0.5f) * SCREEN_WIDTH;
    sy = (0.5f - y / w * 0.5f) * SCREEN_HEIGHT;
    return true;
}

void DrawRect(float x, float y, float w, float h, uint32_t color)
{
    float u = ((FONT_SOLID_CELL % 16) * FONT_CELL + FONT_CELL * 0.5f) / FONT_TEXTURE_WIDTH;
    float v = ((FONT_SOLID_CELL / 16) * FONT_CELL + FONT_CELL * 0.5f) / FONT_TEXTURE_HEIGHT;
    Quad2D(fontTexture, Blend::ALPHA, x, y, w, h, u, v, u, v, color);
}

void DrawImage(TextureId texture, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
               uint32_t color, Blend blend)
{
    Quad2D(texture, blend, x, y, w, h, u0, v0, u1, v1, color);
}

void DrawText(float x, float y, float scale, uint32_t color, const char* text)
{
    if (text == nullptr)
        return;
    float penX = x;
    float penY = y;
    for (const char* c = text; *c != '\0'; c++) {
        int code = static_cast<unsigned char>(*c);
        if (code == '\n') {
            penX = x;
            penY += GLYPH_LINE_HEIGHT * scale;
            continue;
        }
        if (code >= 'a' && code <= 'z')
            code -= 'a' - 'A';
        if (code > ' ' && code < FONT_SOLID_CELL) {
            float u0 = static_cast<float>((code % 16) * FONT_CELL) / FONT_TEXTURE_WIDTH;
            float v0 = static_cast<float>((code / 16) * FONT_CELL) / FONT_TEXTURE_HEIGHT;
            float u1 = u0 + static_cast<float>(GLYPH_WIDTH) / FONT_TEXTURE_WIDTH;
            float v1 = v0 + static_cast<float>(GLYPH_HEIGHT) / FONT_TEXTURE_HEIGHT;
            Quad2D(fontTexture, Blend::ALPHA, penX, penY, GLYPH_WIDTH * scale, GLYPH_HEIGHT * scale, u0, v0, u1, v1,
                   color);
        }
        penX += GLYPH_ADVANCE * scale;
    }
}

void DrawTextShadow(float x, float y, float scale, uint32_t color, const char* text, uint32_t shadow)
{
    uint32_t shadowColor = (shadow & 0x00FFFFFFu) | (color & 0xFF000000u);
    DrawText(x + scale, y + scale, scale, shadowColor, text);
    DrawText(x, y, scale, color, text);
}

int TextWidth(float scale, const char* text)
{
    if (text == nullptr)
        return 0;
    int longest = 0;
    int length = 0;
    for (const char* c = text; *c != '\0'; c++) {
        if (*c == '\n') {
            length = 0;
            continue;
        }
        length++;
        longest = std::max(longest, length);
    }
    if (longest == 0)
        return 0;
    return static_cast<int>((longest * GLYPH_ADVANCE - (GLYPH_ADVANCE - GLYPH_WIDTH)) * scale + 0.5f);
}

// Logs a frame that took longer than SLOW_FRAME_MS (at most MAX_SLOW_FRAME_LOGS times): where the time went and
// what was drawn, to find what causes single missed blanks on the console.
static void LogSlowFrame()
{
    if (lastFrameEnd == 0 || slowFrameLogs >= MAX_SLOW_FRAME_LOGS || framesSinceUpload < SLOW_FRAME_QUIET_FRAMES)
        return;
    float frameMs = MillisecondsSince(lastFrameEnd);
    if (frameMs < SLOW_FRAME_MS)
        return;
    slowFrameLogs++;
    float gameMs = frameMs - MillisecondsSince(frameStartTime);
    int screenPercent = static_cast<int>(billboardPixels * 100.0f / (SCREEN_WIDTH * SCREEN_HEIGHT));
    DbgPrint("render: slow frame %d ms (game %d, gpu wait %d, flip wait %d ms): %d draws, %d tris, %d billboards "
             "covering %d%% of the screen\n",
             static_cast<int>(frameMs), static_cast<int>(gameMs), static_cast<int>(frameStats.gpuWaitMs),
             static_cast<int>(frameStats.flipWaitMs), frameStats.drawCalls, frameStats.triangles, billboardCount,
             screenPercent);
}

void EndFrame()
{
    Flush2D();
    uint64_t waitStart = KeQueryPerformanceCounter();
    // Waits for a free slot in pbkit's flip queue (three frames finished and not yet shown). If the display stops
    // flipping (xemu), the frame is not queued; the next one draws into the same buffer.
    while (pb_finished()) {
        if (MillisecondsSince(waitStart) > MAX_FLIP_QUEUE_WAIT_MS)
            break;
        NtYieldExecution();
    }
    frameStats.flipWaitMs = MillisecondsSince(waitStart);
    lastStats = frameStats;
    LogSlowFrame();
    lastFrameEnd = KeQueryPerformanceCounter();
    if (framesSinceUpload < SLOW_FRAME_QUIET_FRAMES)
        framesSinceUpload++;
}

FrameStats LastFrameStats()
{
    return lastStats;
}

// --- Lifetime

// Sets 640x480 at the dashboard's refresh rate: progressive (480p) when the console is set up for it (the HDTV AV
// pack, which component cables and HDMI adapters present, with 480p allowed in the dashboard's HDTV settings and
// a 60 Hz region), interlaced otherwise. nxdk's mode table makes that choice; this logs it.
static bool SetVideoMode()
{
    DWORD encoder = XVideoGetEncoderSettings();
    bool pal = (encoder & VIDEO_STANDARD_MASK) == VIDEO_STANDARD_PAL;
    bool progressive = (encoder & VIDEO_ADAPTER_MASK) == AV_PACK_HDTV && (encoder & VIDEO_MODE_480P) != 0 && !pal;
    if (!XVideoSetMode(SCREEN_WIDTH, SCREEN_HEIGHT, 32, REFRESH_DEFAULT)) {
        DbgPrint("video: no 640x480 mode (encoder settings %08lx)\n", static_cast<unsigned long>(encoder));
        return false;
    }
    VIDEO_MODE mode = XVideoGetMode();
    DbgPrint("video: %dx%d %s, %d Hz (encoder settings %08lx)\n", mode.width, mode.height,
             progressive ? "progressive" : "interlaced", mode.refresh, static_cast<unsigned long>(encoder));
    return true;
}

bool Init()
{
    if (!SetVideoMode())
        return false;
    pb_size(PUSH_BUFFER_BYTES);
    if (pb_init() != 0)
        return false;
    pb_show_front_screen();

    for (Vertex*& buffer : dynamicBuffers) {
        buffer = static_cast<Vertex*>(
            MmAllocateContiguousMemoryEx(DYNAMIC_BYTES, 0, MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE));
        if (buffer == nullptr)
            return false;
    }
    pushHead = pb_begin();
    pb_end(pushHead);
    SetUpGpu();
    CreateBuiltInTextures();
    BuildSkyDome();
    return whiteTexture != NO_TEXTURE && fontTexture != NO_TEXTURE && sunTexture != NO_TEXTURE &&
           glintTexture != NO_TEXTURE;
}

} // namespace render
