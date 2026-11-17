// Cheats, Objectives, the compass, the objective markers and the minimap: Dear ImGui on the game's own D3D11 device, drawn into the back buffer
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
#include "imgui_internal.h"

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
const ImVec4 kObjective(MM_COLOR_OBJECTIVE, 1.0f);
const ImVec4 kDone(0.55f, 0.80f, 0.55f, 1.0f);
const float kObjectivesFont = 1.25f;
const float kCurrentFont = 1.2f;
const ImVec4 kWhite(1.0f, 1.0f, 1.0f, 1.0f);

int g_imgui_errors;

// Counts the layout errors Dear ImGui reports; its own diagnostics (log, tooltip, assert) stay as they are.
void count_imgui_error(ImGuiContext*, void*, const char*) { g_imgui_errors++; }

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
    ImGui::GetCurrentContext()->ErrorCallback = count_imgui_error;
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
            if (r.selected) ImGui::TextColored(kSelected, "%s", r.value);
            else ImGui::TextUnformatted(r.value);
        }
        ImGui::EndDisabled();
        if (r.selected) ImGui::SetScrollHereY(0.5f);
        break;
    }
    }
}

void legacy_menu(const dmp_frame_t* f) {
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
    snprintf(title, sizeof title, "%s###menu", f->title ? f->title : "Cheats");
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

dml_hits_t g_hits;

// The widest the open page has been: a value that grows (coordinates, health) never makes the panel shrink back.
struct {
    bool open = false;
    int menu = -1, page = -1;
    float view_w = 0.0f, view_h = 0.0f, width = 0.0f;
} g_stable;

float stable_width(const dmp_frame_t* f, float width) {
    if (!g_stable.open || g_stable.menu != f->menu_id || g_stable.page != f->page || g_stable.view_w != f->view_w ||
        g_stable.view_h != f->view_h) {
        g_stable.open = true;
        g_stable.menu = f->menu_id;
        g_stable.page = f->page;
        g_stable.view_w = f->view_w;
        g_stable.view_h = f->view_h;
        g_stable.width = 0.0f;
    }
    g_stable.width = fmaxf(g_stable.width, width);
    return g_stable.width;
}

const ImVec4 kCloseHover(1.0f, 0.42f, 0.36f, 1.0f);

float row_pitch() { return ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y; }

float row_height(const dm_row_t& r) {
    switch (r.kind) {
    case DM_ROW_TITLE: return 0.0f;
    case DM_ROW_SECTION: return ImGui::GetStyle().ItemSpacing.y + row_pitch();
    default: return row_pitch();
    }
}

float value_width(const dm_row_t& r) {
    if (!r.value[0]) return 0.0f;
    return ImGui::CalcTextSize(r.value).x;
}

struct Table {
    float label_w = 0.0f, value_w = 0.0f, other_w = 0.0f, body_h = 0.0f;
    float x[2] = { 0.0f, 0.0f };
};

Table measure(const dmp_frame_t* f) {
    Table t;
    char line[DM_TEXT_MAX * 2 + 4];
    for (int i = 0; i < f->row_count; i++) {
        const dm_row_t& r = f->rows[i];
        t.body_h += row_height(r);
        if (r.kind == DM_ROW_ITEM) {
            t.label_w = fmaxf(t.label_w, ImGui::CalcTextSize(r.text).x);
            t.value_w = fmaxf(t.value_w, value_width(r));
        } else if (r.kind == DM_ROW_INFO) {
            snprintf(line, sizeof line, "%s: %s", r.text, r.value);
            t.other_w = fmaxf(t.other_w, ImGui::CalcTextSize(line).x);
        } else if (r.kind == DM_ROW_SECTION) {
            t.other_w = fmaxf(t.other_w, ImGui::CalcTextSize(r.text).x);
        }
    }
    return t;
}

float help_area_height(const dmp_frame_t* f) { return (float)f->help_lines * ImGui::GetTextLineHeightWithSpacing(); }

float footer_height(const dmp_frame_t* f, float wrap) {
    float h = help_area_height(f);
    if (f->help && f->help[0]) h += ImGui::CalcTextSize(f->help, nullptr, false, wrap).y + ImGui::GetStyle().ItemSpacing.y;
    return h;
}

void footer_text(const char* text, const ImVec4& colour, float wrap_x) {
    if (!text || !text[0]) return;
    ImGui::PushTextWrapPos(wrap_x);
    ImGui::TextColored(colour, "%s", text);
    ImGui::PopTextWrapPos();
}

void table_row(const dm_row_t& r, int id, float value_x, float pad, ImVec2* lo, ImVec2* hi) {
    switch (r.kind) {
    case DM_ROW_SECTION:
        ImGui::Spacing();
        ImGui::TextColored(kSection, "%s", r.text);
        break;
    case DM_ROW_INFO:
        ImGui::BeginDisabled(!r.enabled);
        ImGui::Text("%s: %s", r.text, r.value);
        ImGui::EndDisabled();
        break;
    case DM_ROW_ITEM:
        ImGui::BeginDisabled(!r.enabled);
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::PushID(id);
            ImGui::Selectable("##row", r.selected != 0);
            ImGui::PopID();
            // The item rectangle already takes in half the spacing above and below.
            *lo = ImGui::GetItemRectMin();
            *hi = ImGui::GetItemRectMax();
            // The label stops short of the value column, so a long name is cut rather than run into the value.
            const float limit = r.value[0] ? at.x + value_x - ImGui::GetFontSize() * 0.5f : hi->x;
            const ImVec4 clip(at.x, at.y, limit, at.y + ImGui::GetTextLineHeight() + 1.0f);
            ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), at,
                                                ImGui::GetColorU32(ImGuiCol_Text), r.text, nullptr, 0.0f, &clip);
        }
        if (r.value[0]) {
            ImGui::SameLine(pad + value_x);
            if (r.selected) ImGui::TextColored(kSelected, "%s", r.value);
            else ImGui::TextUnformatted(r.value);
        }
        ImGui::EndDisabled();
        if (r.selected) ImGui::SetScrollHereY(0.5f);
        break;
    default: break;
    }
}

void record_hit(const dml_rect_t& body, const dm_row_t& r, ImVec2 lo, ImVec2 hi, float panel_x, float panel_w) {
    if (r.kind != DM_ROW_ITEM || !r.enabled || g_hits.count >= DML_HITS_MAX) return;
    const float top = fmaxf(lo.y, body.y), bottom = fminf(hi.y, body.y + body.h);
    if (bottom <= top) return;
    g_hits.hit[g_hits.count].rect = dml_rect_t{ panel_x, top, panel_w, bottom - top };
    g_hits.hit[g_hits.count].item = r.item;
    g_hits.count++;
}

// The close button: a square in the header's corner with a cross, lit under the pointer.
void close_button(const dml_rect_t& c, bool hot) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 lo(c.x, c.y), hi(c.x + c.w, c.y + c.h);
    const float m = c.w * 0.3f, t = fmaxf(1.5f, c.w * 0.08f);
    const ImU32 ink = ImGui::ColorConvertFloat4ToU32(hot ? kSelected : kSection);
    dl->AddRectFilled(lo, hi, ImGui::ColorConvertFloat4ToU32(hot ? ImVec4(0.7f, 0.2f, 0.18f, 0.9f) : ImVec4(1, 1, 1, 0.08f)),
                      c.w * 0.15f);
    dl->AddLine(ImVec2(lo.x + m, lo.y + m), ImVec2(hi.x - m, hi.y - m), ink, t);
    dl->AddLine(ImVec2(hi.x - m, lo.y + m), ImVec2(lo.x + m, hi.y - m), ink, t);
}

void panel(const dmp_frame_t* f) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float em = ImGui::GetFontSize(), pad = style.WindowPadding.x, gap = em * 1.5f;
    const char* title_text = f->title ? f->title : "Cheats";
    Table t = measure(f);
    float widths[2] = { t.label_w, t.value_w }, content_w;
    const float title_w = ImGui::CalcTextSize(title_text).x;
    content_w = fmaxf(t.other_w, t.label_w + (t.value_w > 0.0f ? gap + t.value_w : 0.0f)) + style.FramePadding.x * 2.0f;
    dml_params_t p;
    dml_layout_t l;
    p.view_w = f->view_w;
    p.view_h = f->view_h;
    p.em = em;
    p.pad = pad;
    p.panel_w = stable_width(f, dml_panel_width(f->view_w, f->view_h, em, pad, em * 24.0f, content_w, title_w));
    p.body_h = t.body_h;
    p.footer_h = footer_height(f, dml_footer_width(p.panel_w, pad));
    dml_layout(&p, &l);
    dml_columns(widths, 2, l.panel.w - 2.0f * pad - style.FramePadding.x * 2.0f, gap, em * 0.75f, t.x);

    memset(&g_hits, 0, sizeof g_hits);
    g_hits.valid = 1;
    g_hits.menu = f->menu_id;
    g_hits.page = f->page;
    g_hits.panel = l.panel;
    g_hits.close = l.close;
    g_hits.has_close = 1;

    ImGui::SetNextWindowPos(ImVec2(l.panel.x, l.panel.y));
    ImGui::SetNextWindowSize(ImVec2(l.panel.w, l.panel.h));
    ImGui::SetNextWindowBgAlpha(0.92f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("###menu", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool hot = f->pointer && dml_contains(&l.close, f->pointer_x, f->pointer_y);
        const ImU32 rule = ImGui::GetColorU32(ImGuiCol_Separator);
        dl->AddText(ImVec2(l.title.x, l.title.y + (l.title.h - em) * 0.5f), ImGui::ColorConvertFloat4ToU32(kSection),
                    title_text);
        close_button(l.close, hot);
        dl->AddLine(ImVec2(l.header.x, l.header.y + l.header.h), ImVec2(l.header.x + l.header.w, l.header.y + l.header.h),
                    rule);

        ImGui::SetCursorScreenPos(ImVec2(l.body.x, l.body.y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, 0.0f));
        if (ImGui::BeginChild("##body", ImVec2(l.body.w, l.body.h), ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse)) {
            for (int i = 0; i < f->row_count; i++) {
                ImVec2 lo(0.0f, 0.0f), hi(0.0f, 0.0f);
                table_row(f->rows[i], i, t.x[1], pad, &lo, &hi);
                record_hit(l.body, f->rows[i], lo, hi, l.panel.x, l.panel.w);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        const float rule_y = l.body.y + l.body.h + (l.footer.y - l.body.y - l.body.h) * 0.5f;
        dl->AddLine(ImVec2(l.panel.x, rule_y), ImVec2(l.panel.x + l.panel.w, rule_y), rule);
        const float wrap = pad + l.footer.w, area = help_area_height(f);
        const char* note = f->feedback && f->feedback[0] ? f->feedback : f->context_help;
        g_hits.help = dml_rect_t{ l.footer.x, l.footer.y, l.footer.w, area };
        ImGui::SetCursorScreenPos(ImVec2(l.footer.x, l.footer.y));
        ImGui::Dummy(ImVec2(l.footer.w, area));
        if (note && note[0]) {
            const ImVec4 clip(l.footer.x, l.footer.y, l.footer.x + l.footer.w, l.footer.y + area);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(l.footer.x, l.footer.y),
                        ImGui::ColorConvertFloat4ToU32(note == f->feedback ? kFeedback : kSelected), note, nullptr,
                        l.footer.w, &clip);
        }
        if (f->help && f->help[0]) {
            ImGui::SetCursorScreenPos(ImVec2(l.footer.x, l.footer.y + area));
            footer_text(f->help, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), wrap);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void menu(const dmp_frame_t* f) {
    if (f->objectives) legacy_menu(f);
    else panel(f);
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

struct dmp_rect_t {
    float x, y, w, h;
};

// The game's 3D image inside the surface; the whole surface until a scene has been drawn.
dmp_rect_t picture(const dmp_frame_t* f) {
    if (f->scene_w > 0.0f && f->scene_h > 0.0f) return { f->scene_x, f->scene_y, f->scene_w, f->scene_h };
    return { 0.0f, 0.0f, f->view_w, f->view_h };
}

// On the picture: a diamond over the target; off it, an arrow at the edge pointing the way to turn.
void world_marker(ImDrawList* dl, const dmp_frame_t* f, const cp_hud_t& h, const cp_layout_t& l) {
    const float k = l.height * 0.26f, margin = l.small_px * 2.5f;
    cp_screen_t p;
    const dmp_rect_t r = picture(f);
    if (!cp_project_in(&h.camera, r.x, r.y, r.w, r.h, margin, h.target, &p)) return;
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
    const dmp_rect_t r = picture(f);
    cp_screen_t here, next;
    for (int i = 0; i < h.trail_count; i++) {
        const float* ahead = i + 1 < h.trail_count ? h.trail[i + 1] : h.target;
        float dx, dy, len, fade = h.alpha * (1.0f - 0.08f * (float)i);
        if (!cp_project_in(&h.camera, r.x, r.y, r.w, r.h, 0.0f, h.trail[i], &here) || !here.on_screen) continue;
        if (!cp_project_in(&h.camera, r.x, r.y, r.w, r.h, 0.0f, ahead, &next)) continue;
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
    const dmp_rect_t r = picture(f);
    cp_layout_in(r.x, r.y, r.w, r.h, &l);
    if (h.marker && h.view) {
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
    return f->hud && (f->hud->compass || f->hud->marker || f->hud->objective[0] || f->hud->notice[0] || f->hud->minimap);
}

void fill_polygon(ImDrawList* dl, const minimap_point_t* pts, int n, ImU32 col) {
    for (int i = 0; i < n; i++) dl->PathLineTo(ImVec2(pts[i].x, pts[i].y));
    dl->PathFillConvex(col);
}

void objective_arrow(ImDrawList* dl, const minimap_point_t* t, float alpha, float line) {
    const ImVec2 tip(t[0].x, t[0].y), a(t[1].x, t[1].y), b(t[2].x, t[2].y);
    dl->AddTriangleFilled(a, tip, b, colour(kObjective, alpha));
    dl->AddTriangle(a, tip, b, colour(ImVec4(0.0f, 0.0f, 0.0f, 0.8f), alpha), line * 1.5f);
}

struct layer_fill {
    ImDrawList* dl;
    ImU32 colour;
};

void fill_layer(void* user, const minimap_point_t* pts, int n) {
    const layer_fill* fill = static_cast<const layer_fill*>(user);
    fill_polygon(fill->dl, pts, n, fill->colour);
}

ImU32 landmark_colour(int kind) {
    switch (kind) {
    case MM_LM_ROAD: return colour(ImVec4(MM_COLOR_ROAD, 1.0f), 1.0f);
    case MM_LM_WALL:
    case MM_LM_GATE: return colour(ImVec4(MM_COLOR_WALL, 1.0f), 1.0f);
    default: return colour(ImVec4(MM_COLOR_BUILDING, 1.0f), 1.0f);
    }
}

void minimap_waypoint_marker(ImDrawList* dl, const minimap_frame_t& fr, const cp_hud_t& h) {
    minimap_marker_t m;
    minimap_waypoint(&fr, h.waypoint_pos[0], h.waypoint_pos[1], &m);
    if (m.icon == MM_ICON_DIAMOND) {
        diamond(dl, ImVec2(m.x, m.y), fr.radius * MM_WAYPOINT_K, 1.0f, 1.0f);
        return;
    }
    minimap_point_t t[3];
    minimap_arrow(m.x, m.y, m.dir_x, m.dir_y, fr.radius * MM_EDGE_ARROW_K, t);
    objective_arrow(dl, t, 1.0f, 1.0f);
}

void minimap_creature_markers(ImDrawList* dl, const minimap_frame_t& fr, const cp_hud_t& h) {
    const float k = fr.radius * MM_CREATURE_K;
    for (int i = 0; i < h.creature_count; i++) {
        float x, y;
        if (!minimap_creature_place(&fr, &h.creatures[i], &x, &y)) continue;
        dl->AddCircleFilled(ImVec2(x, y), k + 1.0f, colour(ImVec4(0.0f, 0.0f, 0.0f, 0.85f), 1.0f), 12);
        dl->AddCircleFilled(ImVec2(x, y), k, colour(h.creatures[i].threat ? ImVec4(MM_COLOR_THREAT, 1.0f)
                                                                          : ImVec4(MM_COLOR_PASSIVE, 1.0f), 1.0f), 12);
    }
}

void minimap(const dmp_frame_t* f) {
    const cp_hud_t& h = *f->hud;
    const minimap_bounds_t* b;
    const minimap_level_map_t* m;
    if (!h.minimap) return;
    b = minimap_bounds(h.minimap_level);
    m = minimap_map(h.minimap_level);
    if (!b || !m) return;
    const dmp_rect_t r = picture(f);
    const minimap_rect_t pic = { r.x, r.y, r.w, r.h };
    minimap_frame_t fr;
    if (!minimap_place(&pic, h.player_pos[0], h.player_pos[1], MM_ZOOM, &fr)) return;
    const float radius = fr.radius, cx = fr.cx, cy = fr.cy;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddCircleFilled(ImVec2(cx, cy), radius, colour(ImVec4(0.04f, 0.06f, 0.05f, 0.62f), 1.0f), MM_CLIP_SIDES);

    const ImDrawListFlags flags = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    layer_fill layer = { dl, colour(ImVec4(MM_COLOR_LAND, 1.0f), 1.0f) };
    minimap_layer_visit(&fr, b, m, MM_KIND_LAND, fill_layer, &layer);
    layer.colour = colour(ImVec4(MM_COLOR_WATER, 1.0f), 1.0f);
    minimap_layer_visit(&fr, b, m, MM_KIND_WATER, fill_layer, &layer);
    dl->Flags = flags;
    static const int kDrawOrder[4] = { MM_LM_ROAD, MM_LM_BUILDING, MM_LM_WALL, MM_LM_GATE };
    for (int order = 0; order < 4; order++) {
        const int kind = kDrawOrder[order];
        for (int i = 0; i < m->landmark_count; i++) {
            minimap_point_t pts[MM_CLIP_MAX];
            if (m->landmarks[i].kind != kind) continue;
            const int n = minimap_landmark_polygon(&fr, &m->landmarks[i], pts);
            if (n < 3) continue;
            dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
            fill_polygon(dl, pts, n, landmark_colour(kind));
            dl->Flags = flags;
            if (kind == MM_LM_ROAD) continue;
            for (int k = 0; k < n; k++) dl->PathLineTo(ImVec2(pts[k].x, pts[k].y));
            dl->PathStroke(colour(ImVec4(MM_COLOR_LANDMARK_EDGE, 0.85f), 1.0f), ImDrawFlags_Closed, 1.0f);
        }
    }

    minimap_creature_markers(dl, fr, h);
    if (h.waypoint) minimap_waypoint_marker(dl, fr, h);

    float dx, dy;
    minimap_point_t t[4];
    minimap_heading_dir(h.heading, &dx, &dy);
    minimap_player_shape(cx, cy, dx, dy, radius * MM_PLAYER_K, t);
    const ImVec2 tip(t[0].x, t[0].y), left(t[1].x, t[1].y), notch(t[2].x, t[2].y), right(t[3].x, t[3].y);
    const ImU32 fill = colour(ImVec4(MM_COLOR_PLAYER, 1.0f), 1.0f);
    dl->AddTriangleFilled(tip, left, notch, fill);
    dl->AddTriangleFilled(tip, notch, right, fill);
    const ImVec2 outline[4] = { tip, left, notch, right };
    dl->AddPolyline(outline, 4, colour(ImVec4(0.0f, 0.0f, 0.0f, 0.95f), 1.0f), ImDrawFlags_Closed, 1.6f);

    dl->AddCircle(ImVec2(cx, cy), radius, colour(ImVec4(0.03f, 0.04f, 0.03f, 0.95f), 1.0f), MM_CLIP_SIDES, 3.0f);
    dl->AddCircle(ImVec2(cx, cy), radius - 1.5f, colour(ImVec4(0.78f, 0.84f, 0.72f, 0.55f), 1.0f), MM_CLIP_SIDES, 1.0f);
}

}  // namespace

extern "C" void dmp_draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* target, int bb_w,
                         int bb_h, const dmp_frame_t* f) {
    ID3D11RenderTargetView* previous = nullptr;
    ID3D11DepthStencilView* previous_depth = nullptr;
    g_hits.valid = 0;
    if (!f->menu || f->objectives) g_stable.open = false;
    if (!device || !context || !target || bb_w <= 0 || bb_h <= 0 || f->view_w < 1.0f || f->view_h < 1.0f) return;
    if (!f->menu && !hud_shown(f)) return;
    if (!start(device, context)) return;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(f->view_w, f->view_h);
    io.DisplayFramebufferScale = ImVec2((float)bb_w / f->view_w, (float)bb_h / f->view_h);
    io.DeltaTime = frame_seconds();
    apply_scale(f->dpi_scale > 0.0f ? f->dpi_scale : 1.0f);
    fonts();
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    if (hud_shown(f)) {
        compass(f);
        minimap(f);
    }
    if (f->menu) menu(f);
    ImGui::Render();
    context->OMGetRenderTargets(1, &previous, &previous_depth);
    context->OMSetRenderTargets(1, &target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    context->OMSetRenderTargets(1, &previous, previous_depth);
    if (previous) previous->Release();
    if (previous_depth) previous_depth->Release();
}

extern "C" void dmp_release(void) {
    g_hits.valid = 0;
    shut_down();
}

extern "C" const dml_hits_t* dmp_hits(void) { return &g_hits; }

extern "C" int dmp_imgui_errors(void) { return g_imgui_errors; }

extern "C" void dmp_set_fallback_font(const char* path) {
    snprintf(g_fallback_font, sizeof g_fallback_font, "%s", path ? path : "");
}
