// The Video Test panel: Dear ImGui on the game's own D3D11 device, drawn into the back buffer before its Present.
// No platform backend: the host feeds the mouse in the presentation surface's pixels.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>

#include "backends/imgui_impl_dx11.h"
#include "imgui.h"

extern "C" {
#include "video_layout.h"
#include "video_test_panel.h"
}

namespace {

struct Panel {
    ImGuiContext* context = nullptr;
    ID3D11Device* device = nullptr;
    LARGE_INTEGER frequency{}, last{};
    float scale = 0.0f;
};

Panel g_panel;

bool current() {
    if (!g_panel.context) return false;
    ImGui::SetCurrentContext(g_panel.context);
    return true;
}

void shut_down() {
    if (!current()) return;
    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext(g_panel.context);
    g_panel = Panel();
}

bool start(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (g_panel.context && g_panel.device == device) return current();
    shut_down();
    g_panel.context = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_panel.context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.MouseDrawCursor = true;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    if (!ImGui_ImplDX11_Init(device, context)) {
        ImGui::DestroyContext(g_panel.context);
        g_panel = Panel();
        return false;
    }
    g_panel.device = device;
    QueryPerformanceFrequency(&g_panel.frequency);
    QueryPerformanceCounter(&g_panel.last);
    return true;
}

void apply_scale(float scale) {
    if (scale == g_panel.scale) return;
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark(&style);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    g_panel.scale = scale;
}

float frame_seconds() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = (float)(now.QuadPart - g_panel.last.QuadPart) / (float)g_panel.frequency.QuadPart;
    g_panel.last = now;
    return dt < 1e-4f ? 1e-4f : dt > 0.25f ? 0.25f : dt;
}

bool rows(const vt_panel_frame_t* f, int* video, int* mode) {
    bool clicked = false;
    if (!ImGui::BeginTable("videos", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) return false;
    for (int i = 0; i < f->count; i++) {
        const vt_panel_row_t& r = f->rows[i];
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.file);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.size);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.length);
        ImGui::TableNextColumn();
        if (ImGui::Button("Original")) {
            *video = i;
            *mode = VL_PLAY_ORIGINAL;
            clicked = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!f->hd_available);
        if (ImGui::Button("HD")) {
            *video = i;
            *mode = VL_PLAY_HD;
            clicked = true;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndTable();
    return clicked;
}

}  // namespace

extern "C" int vt_panel_draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
                             int bb_w, int bb_h, const vt_panel_frame_t* f, int* video, int* mode) {
    ID3D11RenderTargetView* previous = nullptr;
    ID3D11DepthStencilView* previous_depth = nullptr;
    bool clicked = false;
    if (!device || !context || !target || bb_w <= 0 || bb_h <= 0 || f->view_w < 1.0f || f->view_h < 1.0f) return 0;
    if (!start(device, context)) return 0;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(f->view_w, f->view_h);
    io.DisplayFramebufferScale = ImVec2((float)bb_w / f->view_w, (float)bb_h / f->view_h);
    io.DeltaTime = frame_seconds();
    apply_scale(f->dpi_scale > 0.0f ? f->dpi_scale : 1.0f);
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(f->view_w * 0.5f, f->view_h * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("Video Test", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("The game's own player, audio and timing: only the presentation differs.");
        clicked = rows(f, video, mode);
        if (!f->hd_available) ImGui::TextUnformatted("HD needs the D3D11 renderer.");
        ImGui::TextUnformatted("F5 closes the panel. Esc, a key or a click skips a video, as in the game.");
    }
    ImGui::End();
    ImGui::Render();
    context->OMGetRenderTargets(1, &previous, &previous_depth);
    context->OMSetRenderTargets(1, &target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    context->OMSetRenderTargets(1, &previous, previous_depth);
    if (previous) previous->Release();
    if (previous_depth) previous_depth->Release();
    return clicked ? 1 : 0;
}

extern "C" void vt_panel_mouse_move(float x, float y) {
    if (current()) ImGui::GetIO().AddMousePosEvent(x, y);
}

extern "C" void vt_panel_mouse_button(int button, int down) {
    if (current()) ImGui::GetIO().AddMouseButtonEvent(button, down != 0);
}

extern "C" void vt_panel_mouse_wheel(float steps) {
    if (current()) ImGui::GetIO().AddMouseWheelEvent(0.0f, steps);
}

extern "C" void vt_panel_release(void) { shut_down(); }
