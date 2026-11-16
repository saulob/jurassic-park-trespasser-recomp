// The Debug / Test Menu, Cheats, Objectives, the coordinates line, the compass and the objective markers: Dear ImGui on the game's own D3D11 device, drawn into the back buffer
// before its Present. Input never reaches ImGui: the menu's state is debug_menu_policy.c's.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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
    ImFont* vector = nullptr;
};

Panel g_panel;
char g_fallback_font[MAX_PATH];

const ImVec4 kSection(0.95f, 0.72f, 0.30f, 1.0f);
const ImVec4 kSelected(1.0f, 1.0f, 1.0f, 1.0f);
const ImVec4 kFeedback(0.55f, 0.90f, 0.55f, 1.0f);
const ImVec4 kNorth(1.0f, 0.42f, 0.36f, 1.0f);
const ImVec4 kObjective(1.0f, 0.84f, 0.22f, 1.0f);
const ImVec4 kDone(0.55f, 0.80f, 0.55f, 1.0f);
const float kObjectivesFont = 1.25f;
const float kCurrentFont = 1.2f;
const ImVec4 kWhite(1.0f, 1.0f, 1.0f, 1.0f);

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

// A check mark, an arrow or a dot before an objective, drawn: the fonts have no such glyphs.
void goal_mark(dm_goal_status_t status) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFontSize(), t = h * 0.12f > 1.0f ? h * 0.12f : 1.0f;
    if (status == DM_GOAL_DONE) {
        const ImVec2 p[3] = { ImVec2(at.x + h * 0.15f, at.y + h * 0.55f), ImVec2(at.x + h * 0.4f, at.y + h * 0.8f),
                              ImVec2(at.x + h * 0.85f, at.y + h * 0.2f) };
        dl->AddPolyline(p, 3, ImGui::ColorConvertFloat4ToU32(kDone), 0, t * 1.6f);
    } else if (status == DM_GOAL_CURRENT) {
        dl->AddTriangleFilled(ImVec2(at.x + h * 0.25f, at.y + h * 0.18f), ImVec2(at.x + h * 0.8f, at.y + h * 0.5f),
                              ImVec2(at.x + h * 0.25f, at.y + h * 0.82f), ImGui::ColorConvertFloat4ToU32(kObjective));
    } else {
        dl->AddCircleFilled(ImVec2(at.x + h * 0.5f, at.y + h * 0.5f), h * 0.12f,
                            ImGui::GetColorU32(ImGuiCol_TextDisabled));
    }
    ImGui::Dummy(ImVec2(h * 1.1f, h));
    ImGui::SameLine();
}

float unit(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

void value_bar(const dm_row_t& r) {
    const float h = ImGui::GetFontSize(), w = h * 6.0f, t = unit(r.bar);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float y = at.y + h * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 track = ImGui::GetColorU32(ImGuiCol_FrameBg), fill = ImGui::ColorConvertFloat4ToU32(r.selected ? kSelected : kSection);
    dl->AddRectFilled(ImVec2(at.x, y - h * 0.12f), ImVec2(at.x + w, y + h * 0.12f), track, h * 0.12f);
    dl->AddRectFilled(ImVec2(at.x, y - h * 0.12f), ImVec2(at.x + w * t, y + h * 0.12f), fill, h * 0.12f);
    dl->AddCircleFilled(ImVec2(at.x + w * t, y), h * 0.3f, fill);
    ImGui::Dummy(ImVec2(w + h * 0.5f, h));
    ImGui::SameLine();
}

void row(const dm_row_t& r) {
    switch (r.kind) {
    case DM_ROW_TITLE: break;
    case DM_ROW_GOAL:
        if (r.item == DM_GOAL_CURRENT) {
            ImGui::Spacing();
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kObjectivesFont * kCurrentFont);
        }
        goal_mark((dm_goal_status_t)r.item);
        ImGui::PushTextWrapPos(0.0f);
        if (r.item == DM_GOAL_CURRENT) ImGui::TextColored(kObjective, "%s", r.text);
        else if (r.item == DM_GOAL_DONE) ImGui::TextColored(kDone, "%s", r.text);
        else ImGui::TextDisabled("%s", r.text);
        ImGui::PopTextWrapPos();
        if (r.item == DM_GOAL_CURRENT) ImGui::PopFont();
        break;
    case DM_ROW_NOTE:
        ImGui::Indent(ImGui::GetFontSize() * 1.1f + ImGui::GetStyle().ItemSpacing.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", r.text);
        ImGui::PopStyleColor();
        ImGui::Unindent(ImGui::GetFontSize() * 1.1f + ImGui::GetStyle().ItemSpacing.x);
        break;
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
            if (r.bar >= 0.0f) value_bar(r);
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
    if (f->objectives) ImGui::PushFont(g_panel.vector, ImGui::GetStyle().FontSizeBase * kObjectivesFont);
    const float w = ImGui::GetFontSize() * (f->objectives ? 24.0f : 30.0f);
    if (f->objectives)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize() * 0.8f));
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
    if (f->objectives) {
        ImGui::PopStyleVar();
        ImGui::PopFont();
    }
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

// The menu keeps the font ImGui would choose; the compass needs one that scales.
void fonts() {
    ImGuiIO& io = ImGui::GetIO();
    if (g_panel.vector) return;
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
    g_panel.vector = io.Fonts->AddFontDefaultVector();
    if (g_fallback_font[0]) {
        ImFontConfig merge;
        merge.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(g_fallback_font, 0.0f, &merge);
    }
}

ImU32 colour(const ImVec4& c, float alpha) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

void outlined_text(ImDrawList* dl, float px, float centre_x, float y, const ImVec4& c, float alpha, const char* text,
                   const cp_layout_t& l, float weight = 1.0f) {
    const ImVec2 size = g_panel.vector->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
    const ImVec2 at(floorf(centre_x - size.x * 0.5f), floorf(y));
    const ImU32 dark = colour(ImVec4(0.0f, 0.0f, 0.0f, 0.85f), alpha);
    const float o = l.line_px * weight;
    const ImVec2 around[] = { ImVec2(-o, 0.0f), ImVec2(o, 0.0f),  ImVec2(0.0f, -o), ImVec2(0.0f, o),
                              ImVec2(-o, -o),   ImVec2(o, -o),    ImVec2(-o, o),    ImVec2(o, o * 1.5f) };
    for (const ImVec2& d : around) dl->AddText(g_panel.vector, px, ImVec2(at.x + d.x, at.y + d.y), dark, text);
    dl->AddText(g_panel.vector, px, at, colour(c, alpha), text);
}

// The objective's title: one thin round outline (a ring of samples, a pixel or two at the picture's scale), no shadow.
void titled_text(ImDrawList* dl, float px, float centre_x, float y, const ImVec4& c, float alpha, const char* text,
                 const cp_layout_t& l) {
    const ImVec2 size = g_panel.vector->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
    const ImVec2 at(floorf(centre_x - size.x * 0.5f), floorf(y));
    const ImU32 dark = colour(ImVec4(0.0f, 0.0f, 0.0f, 1.0f), alpha);
    const float r = cp_outline_px(&l);
    const int samples = 16;
    for (int i = 0; i < samples; i++) {
        const float a = 6.2831853f * (float)i / (float)samples;
        dl->AddText(g_panel.vector, px, ImVec2(at.x + cosf(a) * r, at.y + sinf(a) * r), dark, text);
    }
    dl->AddText(g_panel.vector, px, at, colour(c, alpha), text);
}

void outlined_line(ImDrawList* dl, ImVec2 a, ImVec2 b, float alpha, const cp_layout_t& l) {
    dl->AddLine(a, b, colour(ImVec4(0.0f, 0.0f, 0.0f, 0.7f), alpha), l.line_px * 3.0f);
    dl->AddLine(a, b, colour(kWhite, alpha), l.line_px);
}

void strip(ImDrawList* dl, const cp_hud_t& h, const cp_layout_t& l) {
    const float bottom = l.top + l.height, k = l.height * 0.18f;
    const ImU32 clear = colour(ImVec4(0.0f, 0.0f, 0.0f, 0.0f), 1.0f);
    const ImU32 shade = colour(ImVec4(0.0f, 0.0f, 0.0f, 0.3f), h.alpha);
    cp_tick_t ticks[CP_TICKS_MAX];
    dl->AddRectFilledMultiColor(ImVec2(l.left, l.top), ImVec2(l.centre, bottom), clear, shade, shade, clear);
    dl->AddRectFilledMultiColor(ImVec2(l.centre, l.top), ImVec2(l.right, bottom), shade, clear, clear, shade);
    const int n = cp_ticks(h.heading, ticks, CP_TICKS_MAX);
    for (int i = 0; i < n; i++) {
        int side;
        const float x = cp_strip_x(&l, ticks[i].relative, &side), fade = cp_edge_fade(ticks[i].relative) * h.alpha;
        if (fade <= 0.01f) continue;
        if (!ticks[i].name) {
            outlined_line(dl, ImVec2(x, bottom - l.height * 0.32f), ImVec2(x, bottom - l.height * 0.1f), fade, l);
            continue;
        }
        const float px = ticks[i].cardinal ? l.label_px : l.small_px;
        outlined_text(dl, px, x, l.top + (l.height - px) * 0.5f, ticks[i].north ? kNorth : kWhite, fade,
                      ticks[i].name, l);
    }
    dl->AddTriangleFilled(ImVec2(l.centre - k * 1.4f, l.top - k * 0.2f), ImVec2(l.centre + k * 1.4f, l.top - k * 0.2f),
                          ImVec2(l.centre, l.top + k * 1.3f), colour(ImVec4(0.0f, 0.0f, 0.0f, 0.7f), h.alpha));
    dl->AddTriangleFilled(ImVec2(l.centre - k, l.top), ImVec2(l.centre + k, l.top), ImVec2(l.centre, l.top + k),
                          colour(kWhite, h.alpha));
}

// In the strip a diamond; past an end an arrow there, pointing the way to turn.
void marker(ImDrawList* dl, const cp_hud_t& h, const cp_layout_t& l) {
    int side;
    const float x = cp_strip_x(&l, h.relative, &side), k = l.height * 0.24f, y = l.top + l.height - k * 0.3f;
    const ImU32 fill = colour(kObjective, h.alpha), edge = colour(ImVec4(0.0f, 0.0f, 0.0f, 0.8f), h.alpha);
    if (!side) {
        const ImVec2 p[4] = { ImVec2(x, y - k), ImVec2(x + k, y), ImVec2(x, y + k), ImVec2(x - k, y) };
        dl->AddQuadFilled(p[0], p[1], p[2], p[3], fill);
        dl->AddQuad(p[0], p[1], p[2], p[3], edge, l.line_px * 1.5f);
    } else {
        const float tip = x + side * k * 0.9f, back = x - side * k * 0.7f;
        const ImVec2 a(back, y - k), b(tip, y), c(back, y + k);
        dl->AddTriangleFilled(a, b, c, fill);
        dl->AddTriangle(a, b, c, edge, l.line_px * 1.5f);
    }
    if (!h.distance[0]) return;
    const float w = g_panel.vector->CalcTextSizeA(l.small_px, FLT_MAX, 0.0f, h.distance).x * 0.5f;
    const float cx = fminf(fmaxf(x, l.left + w), l.right - w);
    outlined_text(dl, l.small_px, cx, l.text_y, kObjective, h.alpha, h.distance, l);
}

void diamond(ImDrawList* dl, ImVec2 c, float k, float alpha, float line) {
    const ImVec2 p[4] = { ImVec2(c.x, c.y - k), ImVec2(c.x + k, c.y), ImVec2(c.x, c.y + k), ImVec2(c.x - k, c.y) };
    dl->AddQuadFilled(p[0], p[1], p[2], p[3], colour(kObjective, alpha));
    dl->AddQuad(p[0], p[1], p[2], p[3], colour(ImVec4(0.0f, 0.0f, 0.0f, 0.8f), alpha), line * 1.5f);
}

// On the picture: a diamond over the target; off it, an arrow at the edge pointing the way to turn.
void world_marker(ImDrawList* dl, const dmp_frame_t* f, const cp_hud_t& h, const cp_layout_t& l) {
    const float k = l.height * 0.26f, margin = l.small_px * 2.5f;
    cp_screen_t p;
    if (!cp_project(&h.camera, f->view_w, f->view_h, margin, h.target, &p)) return;
    if (p.on_screen) {
        diamond(dl, ImVec2(p.x, p.y), k, h.alpha, l.line_px);
        if (h.distance[0]) outlined_text(dl, l.small_px, p.x, p.y + k * 1.4f, kObjective, h.alpha, h.distance, l);
        return;
    }
    const ImVec2 tip(p.x + p.dir_x * k * 1.2f, p.y + p.dir_y * k * 1.2f);
    const ImVec2 a(p.x - p.dir_y * k, p.y + p.dir_x * k), b(p.x + p.dir_y * k, p.y - p.dir_x * k);
    dl->AddTriangleFilled(a, tip, b, colour(kObjective, h.alpha));
    dl->AddTriangle(a, tip, b, colour(ImVec4(0.0f, 0.0f, 0.0f, 0.8f), h.alpha), l.line_px * 1.5f);
    if (h.distance[0])
        outlined_text(dl, l.small_px, p.x - p.dir_x * k * 2.2f, p.y - p.dir_y * k * 2.2f - l.small_px * 0.5f, kObjective,
                      h.alpha, h.distance, l);
}

// Chevrons over the next anchors on screen, each pointing at the one after it (the last at the target).
void trail(ImDrawList* dl, const dmp_frame_t* f, const cp_hud_t& h, const cp_layout_t& l) {
    const float k = l.height * 0.2f;
    cp_screen_t here, next;
    for (int i = 0; i < h.trail_count; i++) {
        const float* ahead = i + 1 < h.trail_count ? h.trail[i + 1] : h.target;
        float dx, dy, len, fade = h.alpha * (1.0f - 0.08f * (float)i);
        if (!cp_project(&h.camera, f->view_w, f->view_h, 0.0f, h.trail[i], &here) || !here.on_screen) continue;
        if (!cp_project(&h.camera, f->view_w, f->view_h, 0.0f, ahead, &next)) continue;
        dx = next.x - here.x;
        dy = next.y - here.y;
        len = sqrtf(dx * dx + dy * dy);
        if (len < 1.0f || next.depth <= 0.0f) {
            dx = 0.0f;
            dy = -1.0f;
        } else {
            dx /= len;
            dy /= len;
        }
        const ImVec2 tip(here.x + dx * k, here.y + dy * k);
        const ImVec2 a(here.x - dx * k * 0.3f - dy * k, here.y - dy * k * 0.3f + dx * k);
        const ImVec2 b(here.x - dx * k * 0.3f + dy * k, here.y - dy * k * 0.3f - dx * k);
        const ImVec2 points[3] = { a, tip, b };
        dl->AddPolyline(points, 3, colour(ImVec4(0.0f, 0.0f, 0.0f, 0.6f), fade), 0, l.line_px * 4.0f);
        dl->AddPolyline(points, 3, colour(kObjective, fade), 0, l.line_px * 2.0f);
    }
}

void compass(const dmp_frame_t* f) {
    const cp_hud_t& h = *f->hud;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    cp_layout_t l;
    cp_layout(f->view_w, f->view_h, &l);
    if (h.compass && h.marker && h.view) {
        trail(dl, f, h, l);
        world_marker(dl, f, h, l);
    }
    dl = ImGui::GetForegroundDrawList();
    if (h.compass) strip(dl, h, l);
    if (h.compass && h.marker) marker(dl, h, l);
    if (h.objective[0]) titled_text(dl, l.objective_px, l.centre, l.objective_y, kObjective, h.objective_alpha, h.objective, l);
    if (h.notice[0]) outlined_text(dl, l.small_px, l.centre, l.notice_y, kFeedback, 1.0f, h.notice, l);
}

bool hud_shown(const dmp_frame_t* f) {
    return f->hud && (f->hud->compass || f->hud->objective[0] || f->hud->notice[0]);
}

}  // namespace

extern "C" void dmp_draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* target, int bb_w,
                         int bb_h, const dmp_frame_t* f) {
    ID3D11RenderTargetView* previous = nullptr;
    ID3D11DepthStencilView* previous_depth = nullptr;
    if (!device || !context || !target || bb_w <= 0 || bb_h <= 0 || f->view_w < 1.0f || f->view_h < 1.0f) return;
    if (!f->menu && !f->coord_lines && !hud_shown(f)) return;
    if (!start(device, context)) return;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(f->view_w, f->view_h);
    io.DisplayFramebufferScale = ImVec2((float)bb_w / f->view_w, (float)bb_h / f->view_h);
    io.DeltaTime = frame_seconds();
    apply_scale(f->dpi_scale > 0.0f ? f->dpi_scale : 1.0f);
    fonts();
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    if (hud_shown(f)) compass(f);
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

extern "C" void dmp_set_fallback_font(const char* path) {
    snprintf(g_fallback_font, sizeof g_fallback_font, "%s", path ? path : "");
}
