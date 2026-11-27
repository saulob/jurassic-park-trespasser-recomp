// What game scripts draw (headless): GetTexture, its size and its life, DrawTexture's commands from OnPaintHW3D with their
// blend modes, the screen size, errors and budgets while painting, Restart; then the same commands through every backend of the
// overlay: the software one, the GPU list Vulkan draws, the RGBA layer and Direct3D 11 on a WARP device, without a window.
#include "debug_menu_panel.h"
#include "imgui_software.h"
#include "script_api_fixture.h"

#ifdef _WIN32
#include <d3d11.h>
#endif

static const jpt_draw_cmd_t* With(const jpt_draw_frame_t* f, int blend) {
    for (int i = 0; f && i < f->count; i++)
        if (f->cmds[i].blend == blend) return &f->cmds[i];
    return nullptr;
}

static const char* kPainter =
    "class Painter extends GameScript {\n"
    "  tex = null;\n"
    "  function OnScriptInit(b) { tex = TP_Engine.GetTexture(\"textures/white.tga\"); }\n"
    "  function OnPaintHW3D() {\n"
    "    TP_Engine.DrawTexture(tex, 1, 2, 3, 2, 10, 20, 30, 40, 0xFF102030, 0);\n"
    "    TP_Engine.DrawTexture(tex, 0, 0, 8, 4, TP_Engine.GetScreenWidth() / 2, TP_Engine.GetScreenHeight() / 2, 16, 8, 0x80FFFFFF, 1);\n"
    "    TP_Engine.DrawTexture(tex, 0, 0, 8, 4, TP_Engine.GetElapsedSimTime() * 10.0, 0, 8, 4, TP_Engine.rgba(1, 2, 3, 4), 2);\n"
    "  }\n"
    "}\n";

static const char* kTextureProbe =
    "class Probe extends GameScript {\n"
    "  function OnLevelStart() {\n"
    "    local a = TP_Engine.GetTexture(\"textures/white.tga\"), b = TP_Engine.GetTexture(\"TEXTURES\\\\WHITE.TGA\");\n"
    "    print(a.GetWidth() + \"x\" + a.GetHeight() + \" \" + (TP_Engine.GetTexture(\"textures/none.tga\") == null) + \" \" + TP_Engine.IsSoftwareMode() +\n"
    "          \" \" + TP_Engine.GetScreenWidth() + \"x\" + TP_Engine.GetScreenHeight() + \" \" + (a instanceof TP_Engine.Texture));\n"
    "  }\n"
    "}\n";

static void test_textures_and_commands() {
    jpt_script_session* s = ApiSession({{"scripts/painter.nut", kPainter}, {"scripts/probe.nut", kTextureProbe}});
    Ready(s, "draw.scn");
    // Loaded once per name (case and separators alike), a missing one null as CE's.
    CHECK(Has(Print("8x4 true false 1024x768 true")) && g_world.loads == 1);
    const jpt_draw_frame_t* f = jpt_script_level_paint(s);
    CHECK(f && f->count == 3 && f->screen_w == 1024 && f->screen_h == 768 && f->texture_count == 1 && f->generation == jpt_script_session_generation(s));
    // Each command as the script gave it: source and screen rectangles, ARGB and blend mode.
    const jpt_draw_cmd_t* opaque = With(f, JPT_DRAW_OPAQUE);
    CHECK(opaque && opaque->src_x == 1.0f && opaque->src_y == 2.0f && opaque->src_w == 3.0f && opaque->src_h == 2.0f);
    CHECK(opaque && opaque->x == 10.0f && opaque->y == 20.0f && opaque->w == 30.0f && opaque->h == 40.0f && opaque->argb == 0xFF102030u);
    const jpt_draw_cmd_t* alpha = With(f, JPT_DRAW_ALPHA);
    CHECK(alpha && alpha->x == 512.0f && alpha->y == 384.0f && alpha->w == 16.0f && alpha->h == 8.0f && alpha->argb == 0x80FFFFFFu);
    const jpt_draw_cmd_t* add = With(f, JPT_DRAW_ADD);
    CHECK(add && add->x == 0.0f && add->argb == 0x04010203u);
    const jpt_draw_texture_t* white = f && f->texture_count == 1 ? &f->textures[0] : nullptr;
    CHECK(white && white->id == f->cmds[0].texture && white->width == 8 && white->height == 4 && white->rgba && white->rgba[0] == 0xFF);
    // Every frame asks the script again: the engine's state and the screen's size as they are now.
    g_world.time = 3.0f;
    g_world.screen_w = 2048;
    g_world.screen_h = 1536;
    f = jpt_script_level_paint(s);
    add = With(f, JPT_DRAW_ADD);
    alpha = With(f, JPT_DRAW_ALPHA);
    CHECK(add && add->x == 30.0f && alpha && alpha->x == 1024.0f && alpha->y == 768.0f && f->screen_w == 2048);
    // Restart: a new generation, the old texture gone with its VM.
    const uint32_t before = f->generation;
    Begin(s, "draw.scn", JPT_SCRIPT_BEGIN_RESTART);
    CHECK(jpt_script_level_paint_frame(s)->count == 0);
    jpt_script_level_ready(s, 1);
    f = jpt_script_level_paint(s);
    CHECK(f->generation == before + 1 && f->count == 3 && g_world.loads == 2);
    // No paint over a world being loaded.
    Begin(s, "draw.scn", JPT_SCRIPT_BEGIN_RESTART);
    CHECK(jpt_script_level_paint(s)->count == 0);
    Close(s);
}

static void test_paint_errors() {
    const char* scripts =
        "gTex <- null;\n"
        "class Good extends GameScript { function OnScriptInit(b) { gTex = TP_Engine.GetTexture(\"textures/white.tga\"); }\n"
        "  function OnPaintHW3D() { TP_Engine.DrawTexture(gTex, 0, 0, 8, 8, 0, 0, 8, 8, 0xFFFFFFFF, 1); } }\n"
        "class Bad extends GameScript { function OnPaintHW3D() { TP_Engine.DrawTexture(gTex, 0, 0, 8, 8, 10, 10, 8, 8, 0xFFFFFFFF, 1); throw \"paint broke\"; } }\n"
        "class Mode extends GameScript { function OnPaintHW3D() { TP_Engine.DrawTexture(gTex, 0, 0, 8, 8, 0, 0, 8, 8, 0, 3); } }\n"
        "class Early extends GameScript { function OnSimStep(d) { TP_Engine.DrawTexture(gTex, 0, 0, 8, 8, 0, 0, 8, 8, 0, 1); } }\n"
        "class Wrong extends GameScript { function OnPaintHW3D() { TP_Engine.DrawTexture(Vector3(), 0, 0, 8, 8, 0, 0, 8, 8, 0, 1); } }\n"
        "class Many extends GameScript { function OnPaintHW3D() { for (local i = 0; i < 5000; i++) TP_Engine.DrawTexture(gTex, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1); } }\n";
    jpt_script_session* s = ApiSession({{"scripts/errors.nut", scripts}});
    Ready(s, "e.scn");
    const jpt_draw_frame_t* f = jpt_script_level_paint(s);
    // A script that fails while painting draws nothing that frame; the others' commands stay.
    CHECK(Contains("paint broke") && Contains("a blend mode 0, 1 or 2") && Contains("not a texture of TP_Engine.GetTexture") &&
          Contains("more than 4096 draws in a frame"));
    CHECK(f->count == 1 && f->cmds[0].x == 0.0f && f->cmds[0].w == 8.0f);
    Steps(s, 1);
    CHECK(Contains("DrawTexture: only in OnPaintHW3D"));
    // Stopped scripts are not called again: the next frame is the good one's alone.
    f = jpt_script_level_paint(s);
    CHECK(f->count == 1);
    Close(s);
    // An endless paint runs out of its budget; out of memory, the VM is abandoned and nothing is drawn.
    s = ApiSession({{"scripts/loop.nut", "class Loop extends GameScript { function OnPaintHW3D() { while (true) {} } }\n"}}, 20000);
    Ready(s, "l.scn");
    CHECK(jpt_script_level_paint(s)->count == 0 && Contains("callback=OnPaintHW3D") && Contains("instruction budget exceeded (20000)"));
    Close(s);
    s = ApiSession({{"scripts/big.nut",
                     "gTex <- null; gAll <- [];\n"
                     "class Big extends GameScript { function OnScriptInit(b) { gTex = TP_Engine.GetTexture(\"textures/white.tga\"); }\n"
                     "  function OnPaintHW3D() { TP_Engine.DrawTexture(gTex, 0, 0, 8, 8, 0, 0, 8, 8, 0, 1); while (true) gAll.append(array(4096)); } }\n"}},
                   200000000, 1u << 20);
    Ready(s, "b.scn");
    CHECK(jpt_script_level_paint(s)->count == 0 && Contains("result=vm_disabled"));
    CHECK(jpt_script_level_paint(s)->count == 0);
    Close(s);
}

// ---- the backends ----

static uint32_t g_pixels[320 * 240];
static uint8_t g_white[4 * 4 * 4];
static const jpt_draw_texture_t kWhite = {1, 4, 4, g_white};
// Left: red at half alpha over grey; middle: the same added; right: mode 0, its alpha ignored.
static const jpt_draw_cmd_t kCmds[3] = {
    {1, 0, 0, 4, 4, 0, 0, 100, 240, 0x80FF0000u, JPT_DRAW_ALPHA},
    {1, 0, 0, 4, 4, 110, 0, 100, 240, 0x80FF0000u, JPT_DRAW_ADD},
    {1, 0, 0, 4, 4, 220, 0, 100, 240, 0x00FF0000u, JPT_DRAW_OPAQUE},
};

static dmp_frame_t Frame(const jpt_draw_frame_t* script) {
    dmp_frame_t f;
    memset(&f, 0, sizeof f);
    f.view_w = 320.0f;
    f.view_h = 240.0f;
    f.dpi_scale = 1.0f;
    f.script = script;
    return f;
}

static bool Near(unsigned a, unsigned b) { return a + 3 >= b && b + 3 >= a; }

static void test_backends() {
    memset(g_white, 0xFF, sizeof g_white);
    const jpt_draw_frame_t script = {7, 320, 240, kCmds, 3, &kWhite, 1};
    dmp_frame_t f = Frame(&script);
    // Software: the same arithmetic as the GPU's blend states.
    for (uint32_t& p : g_pixels) p = 0x404040u;
    dmp_draw_software(g_pixels, 320, 240, 320, &f);
    const uint32_t alpha = g_pixels[120 * 320 + 50], add = g_pixels[120 * 320 + 160], opaque = g_pixels[120 * 320 + 270];
    CHECK(Near(alpha >> 16 & 0xFF, 160) && Near(alpha >> 8 & 0xFF, 32));
    CHECK(Near(add >> 16 & 0xFF, 192) && Near(add >> 8 & 0xFF, 64));
    CHECK(Near(opaque >> 16 & 0xFF, 255) && Near(opaque >> 8 & 0xFF, 0));
    CHECK(g_pixels[120 * 320 + 105] == 0x404040u);
    // Off the screen: clipped, nothing written out of bounds.
    const jpt_draw_cmd_t off = {1, 0, 0, 4, 4, -300, -300, 400, 400, 0xFFFFFFFFu, JPT_DRAW_ALPHA};
    const jpt_draw_frame_t outside = {7, 320, 240, &off, 1, &kWhite, 1};
    dmp_frame_t g = Frame(&outside);
    for (uint32_t& p : g_pixels) p = 0x404040u;
    dmp_draw_software(g_pixels, 320, 240, 320, &g);
    CHECK(g_pixels[0] == 0xFFFFFFu && g_pixels[120 * 320 + 160] == 0x404040u);

    // The GPU list (Vulkan): the additive command marked, the others alpha, the script texture among its textures.
    dmp_gpu_draw_t gpu;
    memset(&gpu, 0, sizeof gpu);
    CHECK(dmp_build_gpu(&gpu, 320, 240, &f) == 1);
    int added = 0, plain = 0, script_tex = 0;
    for (uint32_t i = 0; i < gpu.cmd_count; i++) gpu.cmds[i].blend ? added++ : plain++;
    for (uint32_t i = 0; i < gpu.texture_count; i++) script_tex += gpu.textures[i].width == 4 && gpu.textures[i].height == 4;
    CHECK(added == 1 && plain >= 2 && script_tex >= 1);
    // A new generation's textures replace the old ones in the GPU slots.
    const jpt_draw_frame_t later = {8, 320, 240, kCmds, 3, &kWhite, 1};
    for (int i = 0; i < 40; i++) {
        dmp_frame_t h = Frame(&later);
        const jpt_draw_frame_t again = {static_cast<uint32_t>(9 + i), 320, 240, kCmds, 3, &kWhite, 1};
        h.script = &again;
        CHECK(dmp_build_gpu(&gpu, 320, 240, &h) == 1);
    }

    // The RGBA layer Vulkan composites as straight alpha cannot add to the picture under it: it counts what it drew as alpha.
    static uint8_t rgba[320 * 240 * 4];
    const unsigned before = imgui_software_rgba_additive;
    CHECK(dmp_draw_rgba(rgba, 320, 240, &f, nullptr, 0, 0) == 1 && imgui_software_rgba_additive == before + 1);

#ifdef _WIN32
    // Direct3D 11 on WARP: the additive blend state through the marker.
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        printf("script_draw_tests: no WARP device, Direct3D 11 skipped\n");
        return;
    }
    D3D11_TEXTURE2D_DESC d;
    memset(&d, 0, sizeof d);
    d.Width = 320;
    d.Height = 240;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *tex = nullptr, *staging = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &tex)) && SUCCEEDED(dev->CreateRenderTargetView(tex, nullptr, &rtv))) {
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &staging))) {
            static const float grey[4] = {64 / 255.0f, 64 / 255.0f, 64 / 255.0f, 1.0f};
            for (int frame_no = 0; frame_no < 2; frame_no++) {
                ctx->ClearRenderTargetView(rtv, grey);
                dmp_draw(dev, ctx, rtv, 320, 240, &f);
            }
            ctx->CopyResource(staging, tex);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(m.pData) + 120 * m.RowPitch);
                const uint32_t a = row[50], b = row[160], c = row[270];
                CHECK(Near(a >> 16 & 0xFF, 160) && Near(a >> 8 & 0xFF, 32));
                CHECK(Near(b >> 16 & 0xFF, 192) && Near(b >> 8 & 0xFF, 64));
                CHECK(Near(c >> 16 & 0xFF, 255) && Near(c >> 8 & 0xFF, 0));
                ctx->Unmap(staging, 0);
            }
            staging->Release();
        }
    }
    if (rtv) rtv->Release();
    if (tex) tex->Release();
    dmp_release();
    ctx->Release();
    dev->Release();
#endif
}

int main() {
    test_textures_and_commands();
    test_paint_errors();
    test_backends();
    dmp_release();
    printf("script_draw_tests: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
