// The Debug / Test Menu and the coordinates line: Dear ImGui on the game's own D3D11 device, drawn into the back buffer
// before its Present. Input never reaches ImGui: the menu's state is debug_menu_policy.c's.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <stdio.h>

#include "backends/imgui_impl_dx11.h"
#include "imgui.h"

extern "C" {
#include "debug_menu_panel.h"
}

namespace {

struct Panel {
    ImGuiContext* context = nullptr;
    ID3D11Device* device = nullptr;
    LARGE_INTEGER frequency{}, last{};
    float scale = 0.0f;
};

Panel g_panel;

const ImVec4 kSection(0.95f, 0.72f, 0.30f, 1.0f);
const ImVec4 kSelected(1.0f, 1.0f, 1.0f, 1.0f);
const ImVec4 kFeedback(0.55f, 0.90f, 0.55f, 1.0f);

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
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoMouse;
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

void row(const dm_row_t& r) {
    switch (r.kind) {
    case DM_ROW_TITLE: break;
    case DM_ROW_SECTION:
        ImGui::Spacing();
        ImGui::TextColored(kSection, "%s", r.text);
        break;
    case DM_ROW_INFO:
        ImGui::BeginDisabled(!r.enabled);
        ImGui::Text("%s: %s", r.text, r.value);
        ImGui::EndDisabled();
        break;
    case DM_ROW_ITEM: {
        ImGui::BeginDisabled(!r.enabled);
        ImGui::Selectable(r.text, r.selected != 0);
        if (r.value[0]) {
            ImGui::SameLine(ImGui::GetFontSize() * 14.0f);
            if (r.selected) ImGui::TextColored(kSelected, "%s", r.value);
            else ImGui::TextUnformatted(r.value);
        }
        ImGui::EndDisabled();
        if (r.selected) ImGui::SetScrollHereY(0.5f);
        break;
    }
    }
}

void menu(const dmp_frame_t* f) {
    const float w = ImGui::GetFontSize() * 30.0f;
    ImGui::SetNextWindowPos(ImVec2(f->view_w * 0.5f, f->view_h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, f->view_h * 0.9f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav;
    char title[DM_TEXT_MAX + 16];
    snprintf(title, sizeof title, "%s###menu", f->title ? f->title : "Debug / Test Menu");
    if (ImGui::Begin(title, nullptr, flags)) {
        for (int i = 0; i < f->row_count; i++) row(f->rows[i]);
        ImGui::Separator();
        if (f->feedback) ImGui::TextColored(kFeedback, "%s", f->feedback);
        if (f->help) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", f->help);
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
}

void coords(const dmp_frame_t* f) {
    const float pad = ImGui::GetFontSize() * 0.6f;
    ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##coordinates", nullptr, flags)) {
        for (int i = 0; i < f->coord_lines; i++) ImGui::TextUnformatted(f->coords[i]);
    }
    ImGui::End();
}

}  // namespace

extern "C" void dmp_draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* target, int bb_w,
                         int bb_h, const dmp_frame_t* f) {
    ID3D11RenderTargetView* previous = nullptr;
    ID3D11DepthStencilView* previous_depth = nullptr;
    if (!device || !context || !target || bb_w <= 0 || bb_h <= 0 || f->view_w < 1.0f || f->view_h < 1.0f) return;
    if (!f->menu && !f->coord_lines) return;
    if (!start(device, context)) return;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(f->view_w, f->view_h);
    io.DisplayFramebufferScale = ImVec2((float)bb_w / f->view_w, (float)bb_h / f->view_h);
    io.DeltaTime = frame_seconds();
    apply_scale(f->dpi_scale > 0.0f ? f->dpi_scale : 1.0f);
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    if (f->coord_lines) coords(f);
    if (f->menu) menu(f);
    ImGui::Render();
    context->OMGetRenderTargets(1, &previous, &previous_depth);
    context->OMSetRenderTargets(1, &target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    context->OMSetRenderTargets(1, &previous, previous_depth);
    if (previous) previous->Release();
    if (previous_depth) previous_depth->Release();
}

extern "C" void dmp_release(void) { shut_down(); }
