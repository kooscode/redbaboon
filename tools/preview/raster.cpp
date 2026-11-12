// Host-side software rasteriser for previewing the procedural content, and a minimal PNG writer.
#include "raster.h"

#include <math.h>
#include <stdio.h>

namespace preview {

// --- Set-up

Raster::Raster(int width, int height)
    : bufferWidth(width), bufferHeight(height), colors(static_cast<size_t>(width) * height),
      depths(colors.size(), 1e30f)
{
    view = MatIdentity();
    proj = MatIdentity();
}

void Raster::SetCamera(const Vec3& eye, const Quat& rotation, float fovY, float nearZ, float farZ)
{
    eyePos = eye;
    view = MatView(eye, rotation);
    float f = 1.0f / tanf(fovY * 0.5f);
    float aspect = static_cast<float>(bufferWidth) / bufferHeight;
    Mat4 p = {};
    p.m[0][0] = f / aspect;
    p.m[1][1] = f;
    p.m[2][2] = farZ / (nearZ - farZ);
    p.m[2][3] = -1.0f;
    p.m[3][2] = nearZ * farZ / (nearZ - farZ);
    proj = p;
}

void Raster::SetLight(const Light& l)
{
    light = l;
}

void Raster::ClearSky(const Vec3& zenith, const Vec3& horizon)
{
    for (int y = 0; y < bufferHeight; y++) {
        float k = static_cast<float>(y) / bufferHeight;
        Vec3 c = Lerp(zenith, horizon, powf(k, 0.7f));
        for (int x = 0; x < bufferWidth; x++)
            colors[static_cast<size_t>(y) * bufferWidth + x] = c;
    }
    for (float& d : depths)
        d = 1e30f;
}

void Raster::Clear(const Vec3& color)
{
    for (Vec3& c : colors)
        c = color;
    for (float& d : depths)
        d = 1e30f;
}

// --- Drawing

static Vec3 Unpack(uint32_t c)
{
    return Vec3(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f);
}

static Vec4 SampleTexture(const ImageData& img, float u, float v)
{
    float x = u * img.width - 0.5f;
    float y = v * img.height - 0.5f;
    int x0 = static_cast<int>(floorf(x));
    int y0 = static_cast<int>(floorf(y));
    float fx = x - x0;
    float fy = y - y0;
    Vec4 r = {0, 0, 0, 0};
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            int xi = ((x0 + i) % img.width + img.width) % img.width;
            int yi = ((y0 + j) % img.height + img.height) % img.height;
            uint32_t p = img.pixels[static_cast<size_t>(yi) * img.width + xi];
            float w = (i ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
            r.x += ((p >> 16) & 0xFF) / 255.0f * w;
            r.y += ((p >> 8) & 0xFF) / 255.0f * w;
            r.z += (p & 0xFF) / 255.0f * w;
            r.w += ((p >> 24) & 0xFF) / 255.0f * w;
        }
    }
    return r;
}

void Raster::Draw(const MeshData& mesh, const Mat4& model, const DrawOptions& o)
{
    Mat4 mvp = model * view * proj;
    Vec3 tint = Unpack(o.tint);
    float tintA = ((o.tint >> 24) & 0xFF) / 255.0f;
    std::vector<ClipVertex> cv(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); i++) {
        const Vertex& v = mesh.vertices[i];
        Vec3 p(v.px, v.py, v.pz);
        Vec3 wp = TransformPoint(model, p);
        Vec3 n = Normalize(TransformDir(model, Vec3(v.nx, v.ny, v.nz)));
        Vec3 vc = Mul(Unpack(v.color), tint);
        float alpha = ((v.color >> 24) & 0xFF) / 255.0f * tintA;
        Vec3 lit = vc;
        if (!o.unlit) {
            float diff = fmaxf(Dot(n, light.sunDir), 0.0f);
            float hemi = n.y * 0.5f + 0.5f;
            Vec3 amb = Lerp(light.groundAmbient, light.skyAmbient, hemi);
            Vec3 viewDir = Normalize(eyePos - wp);
            Vec3 half = Normalize(viewDir + light.sunDir);
            float spec = diff > 0.0f ? powf(fmaxf(Dot(n, half), 0.0f), o.shininess) * o.specular : 0.0f;
            float rim = powf(1.0f - fmaxf(Dot(n, viewDir), 0.0f), 3.0f) * 0.25f;
            lit = Mul(vc, amb + light.sunColor * diff) + light.sunColor * spec + light.skyAmbient * rim * 0.6f;
        }
        Vec4 c;
        c.x = p.x * mvp.m[0][0] + p.y * mvp.m[1][0] + p.z * mvp.m[2][0] + mvp.m[3][0];
        c.y = p.x * mvp.m[0][1] + p.y * mvp.m[1][1] + p.z * mvp.m[2][1] + mvp.m[3][1];
        c.z = p.x * mvp.m[0][2] + p.y * mvp.m[1][2] + p.z * mvp.m[2][2] + mvp.m[3][2];
        c.w = p.x * mvp.m[0][3] + p.y * mvp.m[1][3] + p.z * mvp.m[2][3] + mvp.m[3][3];
        float dist = Length(wp - eyePos);
        cv[i].clip = c;
        cv[i].u = v.u;
        cv[i].v = v.v;
        cv[i].color = lit;
        cv[i].alpha = alpha;
        cv[i].fog = o.fog ? Clamp((dist - light.fogStart) / (light.fogEnd - light.fogStart), 0.0f, 1.0f) : 0.0f;
    }
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        DrawTriangle(cv[mesh.indices[i]], cv[mesh.indices[i + 1]], cv[mesh.indices[i + 2]], o);
}

void Raster::DrawTriangle(const ClipVertex& a, const ClipVertex& b, const ClipVertex& c, const DrawOptions& o)
{
    // Clip against a plane just in front of the eye (w > 0.01).
    const ClipVertex* in[3] = {&a, &b, &c};
    ClipVertex out[4];
    int count = 0;
    for (int i = 0; i < 3; i++) {
        const ClipVertex& p = *in[i];
        const ClipVertex& q = *in[(i + 1) % 3];
        float dp = p.clip.w - 0.01f;
        float dq = q.clip.w - 0.01f;
        if (dp >= 0.0f)
            out[count++] = p;
        if ((dp >= 0.0f) != (dq >= 0.0f)) {
            float t = dp / (dp - dq);
            ClipVertex r;
            r.clip = Vec4{Lerp(p.clip.x, q.clip.x, t), Lerp(p.clip.y, q.clip.y, t), Lerp(p.clip.z, q.clip.z, t),
                          Lerp(p.clip.w, q.clip.w, t)};
            r.u = Lerp(p.u, q.u, t);
            r.v = Lerp(p.v, q.v, t);
            r.color = Lerp(p.color, q.color, t);
            r.alpha = Lerp(p.alpha, q.alpha, t);
            r.fog = Lerp(p.fog, q.fog, t);
            out[count++] = r;
        }
    }
    if (count < 3)
        return;
    Raster3(out[0], out[1], out[2], o);
    if (count == 4)
        Raster3(out[0], out[2], out[3], o);
}

void Raster::Raster3(const ClipVertex& a, const ClipVertex& b, const ClipVertex& c, const DrawOptions& o)
{
    const ClipVertex* v[3] = {&a, &b, &c};
    float sx[3];
    float sy[3];
    float iw[3];
    for (int i = 0; i < 3; i++) {
        iw[i] = 1.0f / v[i]->clip.w;
        sx[i] = (v[i]->clip.x * iw[i] * 0.5f + 0.5f) * bufferWidth;
        sy[i] = (0.5f - v[i]->clip.y * iw[i] * 0.5f) * bufferHeight;
    }
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (fabsf(area) < 1e-9f)
        return;
    // Screen y points down, so counter-clockwise in world gives negative area here.
    if (o.cull && area > 0.0f)
        return;
    int minX = static_cast<int>(fmaxf(floorf(fminf(sx[0], fminf(sx[1], sx[2]))), 0.0f));
    int maxX = static_cast<int>(fminf(ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2]))), bufferWidth - 1.0f));
    int minY = static_cast<int>(fmaxf(floorf(fminf(sy[0], fminf(sy[1], sy[2]))), 0.0f));
    int maxY = static_cast<int>(fminf(ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2]))), bufferHeight - 1.0f));
    float inv = 1.0f / area;
    for (int y = minY; y <= maxY; y++) {
        float py = y + 0.5f;
        for (int x = minX; x <= maxX; x++) {
            float px = x + 0.5f;
            float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) * inv;
            float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) * inv;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                continue;
            float pw = w0 * iw[0] + w1 * iw[1] + w2 * iw[2];
            float depth = 1.0f / pw;
            size_t idx = static_cast<size_t>(y) * bufferWidth + x;
            if (depth >= depths[idx])
                continue;
            float k0 = w0 * iw[0] * depth;
            float k1 = w1 * iw[1] * depth;
            float k2 = w2 * iw[2] * depth;
            Vec3 col = v[0]->color * k0 + v[1]->color * k1 + v[2]->color * k2;
            float alpha = v[0]->alpha * k0 + v[1]->alpha * k1 + v[2]->alpha * k2;
            float fog = v[0]->fog * k0 + v[1]->fog * k1 + v[2]->fog * k2;
            if (o.texture) {
                float u = v[0]->u * k0 + v[1]->u * k1 + v[2]->u * k2;
                float tv = v[0]->v * k0 + v[1]->v * k1 + v[2]->v * k2;
                Vec4 t = SampleTexture(*o.texture, u, tv);
                col = Mul(col, Vec3(t.x, t.y, t.z));
                alpha *= t.w;
                if (o.detail) {
                    Vec4 d = SampleTexture(*o.detail, u * o.detailScale, tv * o.detailScale);
                    col = Mul(col, Vec3(d.x, d.y, d.z)) * 4.0f;
                }
            }
            if (o.blend == Blend::ALPHA_TEST && alpha < 0.5f)
                continue;
            col = Lerp(col, light.fogColor, fog);
            if (o.blend == Blend::ALPHA) {
                colors[idx] = Lerp(colors[idx], col, Clamp(alpha, 0.0f, 1.0f));
            } else if (o.blend == Blend::ADDITIVE) {
                colors[idx] = colors[idx] + col * alpha;
            } else {
                colors[idx] = col;
                depths[idx] = depth;
            }
        }
    }
}

std::vector<uint8_t> Raster::Resolve(int& outWidth, int& outHeight) const
{
    outWidth = bufferWidth / 2;
    outHeight = bufferHeight / 2;
    std::vector<uint8_t> out(static_cast<size_t>(outWidth) * outHeight * 3);
    for (int y = 0; y < outHeight; y++) {
        for (int x = 0; x < outWidth; x++) {
            Vec3 s = colors[(y * 2) * bufferWidth + x * 2] + colors[(y * 2) * bufferWidth + x * 2 + 1] +
                     colors[(y * 2 + 1) * bufferWidth + x * 2] + colors[(y * 2 + 1) * bufferWidth + x * 2 + 1];
            s = s * 0.25f;
            uint8_t* p = &out[(static_cast<size_t>(y) * outWidth + x) * 3];
            p[0] = static_cast<uint8_t>(Clamp(s.x, 0, 1) * 255.0f + 0.5f);
            p[1] = static_cast<uint8_t>(Clamp(s.y, 0, 1) * 255.0f + 0.5f);
            p[2] = static_cast<uint8_t>(Clamp(s.z, 0, 1) * 255.0f + 0.5f);
        }
    }
    return out;
}

// --- PNG writer

static uint32_t Crc(const uint8_t* data, size_t len, uint32_t crc)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void Put32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

static void Chunk(FILE* f, const char* type, const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> buf;
    Put32(buf, static_cast<uint32_t>(data.size()));
    buf.insert(buf.end(), type, type + 4);
    buf.insert(buf.end(), data.begin(), data.end());
    uint32_t crc = Crc(buf.data() + 4, buf.size() - 4, 0);
    Put32(buf, crc);
    fwrite(buf.data(), 1, buf.size(), f);
}

bool WritePng(const char* path, int width, int height, const uint8_t* pixels, int channels)
{
    FILE* f = fopen(path, "wb");
    if (!f)
        return false;
    static const uint8_t SIG[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(SIG, 1, 8, f);
    std::vector<uint8_t> ihdr;
    Put32(ihdr, static_cast<uint32_t>(width));
    Put32(ihdr, static_cast<uint32_t>(height));
    ihdr.push_back(8);
    ihdr.push_back(channels == 4 ? 6 : 2);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    Chunk(f, "IHDR", ihdr);
    std::vector<uint8_t> raw;
    size_t row = static_cast<size_t>(width) * channels;
    for (int y = 0; y < height; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), pixels + y * row, pixels + (y + 1) * row);
    }
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    uint32_t a = 1;
    uint32_t b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    while (pos < raw.size() || pos == 0) {
        size_t n = raw.size() - pos;
        if (n > 65535)
            n = 65535;
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last)
            break;
    }
    Put32(z, (b << 16) | a);
    Chunk(f, "IDAT", z);
    Chunk(f, "IEND", std::vector<uint8_t>());
    fclose(f);
    return true;
}

} // namespace preview
