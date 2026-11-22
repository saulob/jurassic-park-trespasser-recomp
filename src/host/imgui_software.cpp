#include "imgui_software.h"

#include <math.h>

namespace {

struct Vertex {
    float x, y, u, v, r, g, b, a;
};

Vertex load(const ImDrawVert& in, const ImVec2& origin, const ImVec2& scale) {
    Vertex out;
    out.x = (in.pos.x - origin.x) * scale.x;
    out.y = (in.pos.y - origin.y) * scale.y;
    out.u = in.uv.x;
    out.v = in.uv.y;
    out.r = (float)((in.col >> IM_COL32_R_SHIFT) & 0xFF);
    out.g = (float)((in.col >> IM_COL32_G_SHIFT) & 0xFF);
    out.b = (float)((in.col >> IM_COL32_B_SHIFT) & 0xFF);
    out.a = (float)((in.col >> IM_COL32_A_SHIFT) & 0xFF);
    return out;
}

float edge(float ax, float ay, float bx, float by, float px, float py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }

// Bilinear sample of a texture at (u, v) in 0..1: alpha is the 8-bit alpha channel or, for RGBA32, the texel's own four channels.
void sample(const ImTextureData* tex, float u, float v, float out[4]) {
    const int w = tex->Width, h = tex->Height, bpp = tex->BytesPerPixel;
    const float fx = u * (float)w - 0.5f, fy = v * (float)h - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    const float tx = fx - (float)x0, ty = fy - (float)y0;
    float acc[4] = { 0, 0, 0, 0 };
    for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
            int x = x0 + dx, y = y0 + dy;
            const float wgt = (dx ? tx : 1.0f - tx) * (dy ? ty : 1.0f - ty);
            x = x < 0 ? 0 : x >= w ? w - 1 : x;
            y = y < 0 ? 0 : y >= h ? h - 1 : y;
            const unsigned char* p = tex->Pixels + ((size_t)y * (size_t)w + (size_t)x) * (size_t)bpp;
            if (bpp == 1) {
                acc[0] += 255.0f * wgt, acc[1] += 255.0f * wgt, acc[2] += 255.0f * wgt, acc[3] += (float)p[0] * wgt;
            } else {
                acc[0] += (float)p[0] * wgt, acc[1] += (float)p[1] * wgt, acc[2] += (float)p[2] * wgt, acc[3] += (float)p[3] * wgt;
            }
        }
    }
    for (int i = 0; i < 4; i++) out[i] = acc[i] / 255.0f;
}

void triangle(const Vertex& a, const Vertex& b, const Vertex& c, const ImVec4& clip, const ImTextureData* tex, uint32_t* pixels,
              int width, int height, int pitch) {
    const float area = edge(a.x, a.y, b.x, b.y, c.x, c.y);
    if (area == 0.0f) return;
    float min_x = fminf(a.x, fminf(b.x, c.x)), max_x = fmaxf(a.x, fmaxf(b.x, c.x));
    float min_y = fminf(a.y, fminf(b.y, c.y)), max_y = fmaxf(a.y, fmaxf(b.y, c.y));
    min_x = fmaxf(min_x, fmaxf(clip.x, 0.0f)), min_y = fmaxf(min_y, fmaxf(clip.y, 0.0f));
    max_x = fminf(max_x, fminf(clip.z, (float)width)), max_y = fminf(max_y, fminf(clip.w, (float)height));
    const int x0 = (int)floorf(min_x), x1 = (int)ceilf(max_x), y0 = (int)floorf(min_y), y1 = (int)ceilf(max_y);
    const float inv = 1.0f / area;
    for (int y = y0; y < y1; y++) {
        const float py = (float)y + 0.5f;
        if (py < clip.y || py >= clip.w) continue;
        uint32_t* row = pixels + (size_t)y * (size_t)pitch;
        for (int x = x0; x < x1; x++) {
            const float px = (float)x + 0.5f;
            if (px < clip.x || px >= clip.z) continue;
            const float w0 = edge(b.x, b.y, c.x, c.y, px, py) * inv, w1 = edge(c.x, c.y, a.x, a.y, px, py) * inv, w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            float r = a.r * w0 + b.r * w1 + c.r * w2, g = a.g * w0 + b.g * w1 + c.g * w2, bl = a.b * w0 + b.b * w1 + c.b * w2;
            float al = (a.a * w0 + b.a * w1 + c.a * w2) / 255.0f;
            if (tex) {
                float t[4];
                sample(tex, a.u * w0 + b.u * w1 + c.u * w2, a.v * w0 + b.v * w1 + c.v * w2, t);
                r *= t[0], g *= t[1], bl *= t[2], al *= t[3];
            }
            if (al <= 0.0f) continue;
            if (al > 1.0f) al = 1.0f;
            const uint32_t d = row[x];
            const float dr = (float)((d >> 16) & 0xFF), dg = (float)((d >> 8) & 0xFF), db = (float)(d & 0xFF);
            const unsigned or_ = (unsigned)(r * al + dr * (1.0f - al) + 0.5f), og = (unsigned)(g * al + dg * (1.0f - al) + 0.5f),
                           ob = (unsigned)(bl * al + db * (1.0f - al) + 0.5f);
            row[x] = (or_ > 255 ? 255u : or_) << 16 | (og > 255 ? 255u : og) << 8 | (ob > 255 ? 255u : ob);
        }
    }
}

void triangle_rgba(const Vertex& a, const Vertex& b, const Vertex& c, const ImVec4& clip, const ImTextureData* tex,
                   uint8_t* rgba, int width, int height, uint8_t* tiles, int tiles_w, int tiles_h) {
    const float area = edge(a.x, a.y, b.x, b.y, c.x, c.y);
    if (area == 0.0f) return;
    float min_x = fminf(a.x, fminf(b.x, c.x)), max_x = fmaxf(a.x, fmaxf(b.x, c.x));
    float min_y = fminf(a.y, fminf(b.y, c.y)), max_y = fmaxf(a.y, fmaxf(b.y, c.y));
    min_x = fmaxf(min_x, fmaxf(clip.x, 0.0f)), min_y = fmaxf(min_y, fmaxf(clip.y, 0.0f));
    max_x = fminf(max_x, fminf(clip.z, (float)width)), max_y = fminf(max_y, fminf(clip.w, (float)height));
    const int x0 = (int)floorf(min_x), x1 = (int)ceilf(max_x), y0 = (int)floorf(min_y), y1 = (int)ceilf(max_y);
    const float inv = 1.0f / area;
    if (tiles && x1 > x0 && y1 > y0) {
        const int tx1 = (x1 - 1) >> OVERLAY_TILE_SHIFT, ty1 = (y1 - 1) >> OVERLAY_TILE_SHIFT;
        for (int ty = y0 >> OVERLAY_TILE_SHIFT; ty <= ty1 && ty < tiles_h; ty++)
            for (int tx = x0 >> OVERLAY_TILE_SHIFT; tx <= tx1 && tx < tiles_w; tx++) tiles[ty * tiles_w + tx] = 1;
    }
    for (int y = y0; y < y1; y++) {
        const float py = (float)y + 0.5f;
        if (py < clip.y || py >= clip.w) continue;
        uint8_t* row = rgba + (size_t)y * (size_t)width * 4u;
        for (int x = x0; x < x1; x++) {
            const float px = (float)x + 0.5f;
            if (px < clip.x || px >= clip.z) continue;
            const float w0 = edge(b.x, b.y, c.x, c.y, px, py) * inv, w1 = edge(c.x, c.y, a.x, a.y, px, py) * inv,
                        w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            float r = a.r * w0 + b.r * w1 + c.r * w2, g = a.g * w0 + b.g * w1 + c.g * w2,
                  bl = a.b * w0 + b.b * w1 + c.b * w2;
            float sa = (a.a * w0 + b.a * w1 + c.a * w2) / 255.0f;
            if (tex) {
                float t[4];
                sample(tex, a.u * w0 + b.u * w1 + c.u * w2, a.v * w0 + b.v * w1 + c.v * w2, t);
                r *= t[0], g *= t[1], bl *= t[2], sa *= t[3];
            }
            if (sa <= 0.0f) continue;
            if (sa > 1.0f) sa = 1.0f;
            uint8_t* dst = row + (size_t)x * 4u;
            const float da = (float)dst[3] / 255.0f;
            const float oa = sa + da * (1.0f - sa);
            if (oa <= 1e-6f) continue;
            const float inv_oa = 1.0f / oa;
            const unsigned or_ = (unsigned)((r * sa + (float)dst[0] * da * (1.0f - sa)) * inv_oa + 0.5f);
            const unsigned og = (unsigned)((g * sa + (float)dst[1] * da * (1.0f - sa)) * inv_oa + 0.5f);
            const unsigned ob = (unsigned)((bl * sa + (float)dst[2] * da * (1.0f - sa)) * inv_oa + 0.5f);
            const unsigned oa8 = (unsigned)(oa * 255.0f + 0.5f);
            dst[0] = (uint8_t)(or_ > 255u ? 255u : or_);
            dst[1] = (uint8_t)(og > 255u ? 255u : og);
            dst[2] = (uint8_t)(ob > 255u ? 255u : ob);
            dst[3] = (uint8_t)(oa8 > 255u ? 255u : oa8);
        }
    }
}

void update_textures(ImDrawData* data) {
    if (!data->Textures) return;
    for (ImTextureData* tex : *data->Textures) {
        if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
            tex->SetTexID((ImTextureID)(uintptr_t)tex);
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

}  // namespace

void imgui_software_update_textures(ImDrawData* data) { update_textures(data); }

void imgui_software_init(ImGuiIO& io) { io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; }

void imgui_software_render(ImDrawData* data, uint32_t* pixels, int width, int height, int pitch_pixels) {
    if (!data || !pixels || width <= 0 || height <= 0) return;
    update_textures(data);
    const ImVec2 origin = data->DisplayPos, scale = data->FramebufferScale;
    for (int n = 0; n < data->CmdListsCount; n++) {
        const ImDrawList* list = data->CmdLists[n];
        const ImDrawVert* vertices = list->VtxBuffer.Data;
        const ImDrawIdx* indices = list->IdxBuffer.Data;
        for (int c = 0; c < list->CmdBuffer.Size; c++) {
            const ImDrawCmd* cmd = &list->CmdBuffer[c];
            if (cmd->UserCallback) continue;
            const ImVec4 clip((cmd->ClipRect.x - origin.x) * scale.x, (cmd->ClipRect.y - origin.y) * scale.y,
                              (cmd->ClipRect.z - origin.x) * scale.x, (cmd->ClipRect.w - origin.y) * scale.y);
            if (clip.z <= clip.x || clip.w <= clip.y) continue;
            const ImTextureData* tex = (const ImTextureData*)(uintptr_t)cmd->GetTexID();
            if (tex && !tex->Pixels) tex = nullptr;
            const ImDrawIdx* idx = indices + cmd->IdxOffset;
            const ImDrawVert* base = vertices + cmd->VtxOffset;
            for (unsigned i = 0; i + 2 < cmd->ElemCount; i += 3)
                triangle(load(base[idx[i]], origin, scale), load(base[idx[i + 1]], origin, scale), load(base[idx[i + 2]], origin, scale),
                         clip, tex, pixels, width, height, pitch_pixels);
        }
    }
}

void imgui_software_render_rgba(ImDrawData* data, uint8_t* rgba, int width, int height, uint8_t* tiles, int tiles_w,
                                int tiles_h) {
    if (!data || !rgba || width <= 0 || height <= 0) return;
    update_textures(data);
    const ImVec2 origin = data->DisplayPos, scale = data->FramebufferScale;
    for (int n = 0; n < data->CmdListsCount; n++) {
        const ImDrawList* list = data->CmdLists[n];
        const ImDrawVert* vertices = list->VtxBuffer.Data;
        const ImDrawIdx* indices = list->IdxBuffer.Data;
        for (int c = 0; c < list->CmdBuffer.Size; c++) {
            const ImDrawCmd* cmd = &list->CmdBuffer[c];
            if (cmd->UserCallback) continue;
            const ImVec4 clip((cmd->ClipRect.x - origin.x) * scale.x, (cmd->ClipRect.y - origin.y) * scale.y,
                              (cmd->ClipRect.z - origin.x) * scale.x, (cmd->ClipRect.w - origin.y) * scale.y);
            if (clip.z <= clip.x || clip.w <= clip.y) continue;
            const ImTextureData* tex = (const ImTextureData*)(uintptr_t)cmd->GetTexID();
            if (tex && !tex->Pixels) tex = nullptr;
            const ImDrawIdx* idx = indices + cmd->IdxOffset;
            const ImDrawVert* base = vertices + cmd->VtxOffset;
            for (unsigned i = 0; i + 2 < cmd->ElemCount; i += 3)
                triangle_rgba(load(base[idx[i]], origin, scale), load(base[idx[i + 1]], origin, scale),
                              load(base[idx[i + 2]], origin, scale), clip, tex, rgba, width, height, tiles, tiles_w, tiles_h);
        }
    }
}
