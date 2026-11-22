// Cheats, Objectives, the compass, the objective markers and the minimap: Dear ImGui on the game's own D3D11 device, drawn into the back buffer
// before its Present. Input never reaches ImGui: the menu's state is debug_menu_policy.c's.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <vector>
#include <windows.h>

#include <d3d11.h>
#endif
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <chrono>

#ifdef _WIN32
#include "backends/imgui_impl_dx11.h"
#endif
#include "imgui_software.h"
#include "imgui.h"
#include "imgui_internal.h"

extern "C" {
#include "debug_menu_panel.h"
}

namespace {

struct Panel {
    ImGuiContext* context = nullptr;
    const void* device = nullptr;
    std::chrono::steady_clock::time_point last{};
    float scale = 0.0f;
    ImFont* vector = nullptr;
};

Panel g_panel;
char g_fallback_font[512];
const void* g_fallback_memory = nullptr;
size_t g_fallback_memory_size = 0;

const ImVec4 kSection(0.95f, 0.72f, 0.30f, 1.0f);
const ImVec4 kSelected(1.0f, 1.0f, 1.0f, 1.0f);
const ImVec4 kFeedback(0.55f, 0.90f, 0.55f, 1.0f);
const ImVec4 kNorth(1.0f, 0.42f, 0.36f, 1.0f);
const ImVec4 kObjective(MM_COLOR_OBJECTIVE, 1.0f);
const ImVec4 kDone(0.55f, 0.80f, 0.55f, 1.0f);
const float kObjectivesFont = 1.1f;
const float kCurrentFont = 1.15f;
const ImVec4 kWhite(1.0f, 1.0f, 1.0f, 1.0f);
const ImVec4 kAccent(0x8C / 255.0f, 0xC6 / 255.0f, 0x3F / 255.0f, 1.0f);
const ImVec4 kPanelBackground(0.04f, 0.05f, 0.11f, 0.92f);
const ImVec4 kDim(0.64f, 0.66f, 0.76f, 1.0f);
const ImVec4 kTitleBackground(0.07f, 0.09f, 0.18f, 0.96f);
const ImVec4 kFocus(kAccent.x, kAccent.y, kAccent.z, 0.42f);
const ImU32 kCounterBackground = IM_COL32(7, 10, 22, 190);

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
#ifdef _WIN32
    if (g_panel.device) ImGui_ImplDX11_Shutdown();
#endif
    ImGui::DestroyContext(g_panel.context);
    g_panel = Panel();
}

void create_context() {
    g_panel.context = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_panel.context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::GetCurrentContext()->ErrorCallback = count_imgui_error;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoMouse;
}

#ifdef _WIN32
bool start(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (g_panel.context && g_panel.device == device) return current();
    shut_down();
    create_context();
    if (!ImGui_ImplDX11_Init(device, context)) {
        ImGui::DestroyContext(g_panel.context);
        g_panel = Panel();
        return false;
    }
    g_panel.device = device;
    g_panel.last = std::chrono::steady_clock::now();
    return true;
}
#endif

bool start_software() {
    if (g_panel.context && g_panel.device == nullptr) return current();
    shut_down();
    create_context();
    imgui_software_init(ImGui::GetIO());
    g_panel.device = nullptr;
    g_panel.last = std::chrono::steady_clock::now();
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
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - g_panel.last).count();
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

const int kChromeColors = 7;
const int kChromeVars = 2;

// One look for every panel: the navy body, the accent border and rows, rounded.
void push_chrome(float em) {
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanelBackground);
    ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
    ImGui::PushStyleColor(ImGuiCol_Header, kFocus);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kFocus);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, kFocus);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, kTitleBackground);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kTitleBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, em * 0.35f);
}

void pop_chrome() {
    ImGui::PopStyleVar(kChromeVars);
    ImGui::PopStyleColor(kChromeColors);
}

void objective_row(const dm_row_t& r) {
    const bool current = r.item == DM_GOAL_CURRENT;
    if (current) {
        ImGui::Spacing();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kObjectivesFont * kCurrentFont);
    }
    goal_mark((dm_goal_status_t)r.item);
    ImGui::PushTextWrapPos(0.0f);
    if (current) ImGui::TextColored(kObjective, "%s", r.text);
    else if (r.item == DM_GOAL_DONE) ImGui::TextColored(kDone, "%s", r.text);
    else ImGui::TextDisabled("%s", r.text);
    ImGui::PopTextWrapPos();
    if (current) ImGui::PopFont();
}

void row(const dm_row_t& r) {
    switch (r.kind) {
    case DM_ROW_TITLE: break;
    case DM_ROW_GOAL: objective_row(r); break;
    case DM_ROW_NOTE:
        ImGui::Indent(ImGui::GetFontSize() * 1.1f + ImGui::GetStyle().ItemSpacing.x);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
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
        ImGui::EndDisabled();
        if (r.selected) ImGui::SetScrollHereY(0.5f);
        break;
    }
    }
}

// The window centred in the view: wider and in smaller type than the first version, so a title takes one line.
void objectives_window(const dmp_frame_t* f) {
    ImGui::PushFont(g_panel.vector, ImGui::GetStyle().FontSizeBase * kObjectivesFont);
    const float em = ImGui::GetFontSize(), w = dml_objectives_width(f->view_w, f->view_h, em);
    push_chrome(em);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.1f, em * 0.9f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, em * 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em * 0.7f, em * 0.45f));
    ImGui::SetNextWindowPos(ImVec2(f->view_w * 0.5f, f->view_h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, f->view_h * 0.9f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav;
    char title[DM_TEXT_MAX + 16];
    snprintf(title, sizeof title, "%s###menu", f->title ? f->title : "Objectives");
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    const bool open = ImGui::Begin(title, nullptr, flags);
    ImGui::PopStyleColor();
    if (open) {
        for (int i = 0; i < f->row_count; i++) row(f->rows[i]);
        ImGui::Separator();
        if (f->feedback) ImGui::TextColored(kFeedback, "%s", f->feedback);
        if (f->help) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped("%s", f->help);
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    pop_chrome();
    ImGui::PopFont();
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
};

Table measure(const dmp_frame_t* f) {
    Table t;
    char line[512];
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

// A row's label at the left, its value right-aligned: in the accent when the cheat is on, dim otherwise.
void table_row(const dm_row_t& r, int id, ImVec2* lo, ImVec2* hi) {
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
            const float inner = ImGui::GetFontSize() * 0.4f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImGui::PushID(id);
            ImGui::Selectable("##row", false);
            ImGui::PopID();
            // The item rectangle already takes in half the spacing above and below.
            *lo = ImGui::GetItemRectMin();
            *hi = ImGui::GetItemRectMax();
            if (r.selected) dl->AddRectFilled(*lo, *hi, ImGui::GetColorU32(kFocus), ImGui::GetFontSize() * 0.25f);
            const float value_x = hi->x - inner - value_width(r);
            // The label stops short of the value, so a long name is cut rather than run into it.
            const float limit = r.value[0] ? value_x - ImGui::GetFontSize() * 0.5f : hi->x - inner;
            const ImVec4 clip(lo->x, at.y, limit, at.y + ImGui::GetTextLineHeight() + 1.0f);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(lo->x + inner, at.y),
                        ImGui::GetColorU32(ImGuiCol_Text), r.text, nullptr, 0.0f, &clip);
            if (r.value[0])
                dl->AddText(ImVec2(value_x, at.y),
                            ImGui::ColorConvertFloat4ToU32(r.lit ? kAccent : r.selected ? kSelected : kDim), r.value);
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
    const ImU32 ink = ImGui::ColorConvertFloat4ToU32(hot ? kSelected : kAccent);
    dl->AddRectFilled(lo, hi, ImGui::ColorConvertFloat4ToU32(hot ? ImVec4(0.7f, 0.2f, 0.18f, 0.9f) : ImVec4(1, 1, 1, 0.08f)),
                      c.w * 0.15f);
    dl->AddLine(ImVec2(lo.x + m, lo.y + m), ImVec2(hi.x - m, hi.y - m), ink, t);
    dl->AddLine(ImVec2(hi.x - m, lo.y + m), ImVec2(lo.x + m, hi.y - m), ink, t);
}

// Where the frame counter's box goes for this frame; false when there is no counter.
bool counter_box(const dmp_frame_t* f, dml_counter_t* box) {
    if (!f->counter || !f->counter[0]) return false;
    const ImVec2 text = ImGui::CalcTextSize(f->counter);
    dml_counter_layout(f->view_w, ImGui::GetFontSize(), text.x, text.y, box);
    return true;
}

void frame_counter(ImDrawList* dl, const dmp_frame_t* f) {
    dml_counter_t c;
    if (!counter_box(f, &c)) return;
    const ImVec2 lo(c.box.x, c.box.y), hi(c.box.x + c.box.w, c.box.y + c.box.h);
    dl->AddRectFilled(lo, hi, kCounterBackground, c.rounding);
    dl->AddRect(lo, hi, ImGui::ColorConvertFloat4ToU32(kAccent), c.rounding, 0, 1.0f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(lo.x + c.pad_x, lo.y + c.pad_y),
                ImGui::ColorConvertFloat4ToU32(kWhite), f->counter);
}

ImTextureData g_sw_text_tex;

// The game's own text, redrawn over the HUD.
void text_layer(ImDrawList* dl, const dmp_frame_t* f) {
    const dmp_text_layer_t* t = f->text_layer;
    if (!t || !t->texture || t->legacy_w <= 0 || t->legacy_h <= 0 || t->right <= t->left || t->bottom <= t->top) return;
    const float sx = f->view_w / (float)t->legacy_w, sy = f->view_h / (float)t->legacy_h;
    if (t->software_rgba) {
        g_sw_text_tex = ImTextureData();
        g_sw_text_tex.Status = ImTextureStatus_OK;
        g_sw_text_tex.Format = ImTextureFormat_RGBA32;
        g_sw_text_tex.Width = t->right - t->left;
        g_sw_text_tex.Height = t->bottom - t->top;
        g_sw_text_tex.BytesPerPixel = 4;
        g_sw_text_tex.Pixels = static_cast<unsigned char*>(t->texture);
        g_sw_text_tex.TexID = (ImTextureID)(uintptr_t)&g_sw_text_tex;
        dl->AddImage(ImTextureRef(g_sw_text_tex.TexID), ImVec2((float)t->left * sx, (float)t->top * sy),
                     ImVec2((float)t->right * sx, (float)t->bottom * sy), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
        return;
    }
    dl->AddImage(ImTextureRef((ImTextureID)(uintptr_t)t->texture), ImVec2((float)t->left * sx, (float)t->top * sy),
                 ImVec2((float)t->right * sx, (float)t->bottom * sy),
                 ImVec2((float)t->left / (float)t->legacy_w, (float)t->top / (float)t->legacy_h),
                 ImVec2((float)t->right / (float)t->legacy_w, (float)t->bottom / (float)t->legacy_h));
}

void panel(const dmp_frame_t* f) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float em = ImGui::GetFontSize(), pad = style.WindowPadding.x * 1.5f, gap = em * 1.5f;
    const char* title_text = f->title ? f->title : "Cheats";
    Table t = measure(f);
    dml_counter_t counter;
    const float title_w = ImGui::CalcTextSize(title_text).x;
    const float content_w = fmaxf(t.other_w, t.label_w + (t.value_w > 0.0f ? gap + t.value_w : 0.0f)) + em * 0.8f;
    dml_params_t p;
    dml_layout_t l;
    memset(&p, 0, sizeof p);
    p.view_w = f->view_w;
    p.view_h = f->view_h;
    p.em = em;
    p.pad = pad;
    p.dock_right = 1;
    p.top_inset = counter_box(f, &counter) ? dml_counter_inset(&counter, f->view_w, f->view_h, em) : 0.0f;
    p.panel_w = stable_width(f, dml_panel_width(f->view_w, f->view_h, em, pad, em * 24.0f, content_w, title_w));
    p.body_h = t.body_h;
    p.footer_h = footer_height(f, dml_footer_width(p.panel_w, pad));
    dml_layout(&p, &l);

    memset(&g_hits, 0, sizeof g_hits);
    g_hits.valid = 1;
    g_hits.menu = f->menu_id;
    g_hits.page = f->page;
    g_hits.panel = l.panel;
    g_hits.close = l.close;
    g_hits.has_close = 1;

    push_chrome(em);
    ImGui::SetNextWindowPos(ImVec2(l.panel.x, l.panel.y));
    ImGui::SetNextWindowSize(ImVec2(l.panel.w, l.panel.h));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("###menu", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool hot = f->pointer && dml_contains(&l.close, f->pointer_x, f->pointer_y);
        const ImU32 rule = ImGui::GetColorU32(ImGuiCol_Separator);
        dl->AddText(ImVec2(l.title.x, l.title.y + (l.title.h - em) * 0.5f), ImGui::ColorConvertFloat4ToU32(kAccent),
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
                table_row(f->rows[i], i, &lo, &hi);
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
                        ImGui::ColorConvertFloat4ToU32(note == f->feedback ? kFeedback : kDim), note, nullptr,
                        l.footer.w, &clip);
        }
        if (f->help && f->help[0]) {
            ImGui::SetCursorScreenPos(ImVec2(l.footer.x, l.footer.y + area));
            footer_text(f->help, kDim, wrap);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    pop_chrome();
}

void menu(const dmp_frame_t* f) {
    if (f->objectives) objectives_window(f);
    else panel(f);
}

// The menu keeps the font ImGui would choose; the compass needs one that scales.
void fonts() {
    ImGuiIO& io = ImGui::GetIO();
    if (g_panel.vector) return;
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
    g_panel.vector = io.Fonts->AddFontDefaultVector();
    if (g_fallback_memory) {
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.FontDataOwnedByAtlas = false;
        io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(g_fallback_memory), (int)g_fallback_memory_size, 0.0f, &merge);
    } else if (g_fallback_font[0]) {
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

void minimap_vehicle_markers(ImDrawList* dl, const minimap_frame_t& fr, const minimap_level_map_t& m) {
    const ImU32 fill = colour(ImVec4(MM_COLOR_VEHICLE, 1.0f), 1.0f), edge = colour(ImVec4(0.0f, 0.0f, 0.0f, 0.85f), 1.0f);
    for (int i = 0; i < m.vehicle_count; i++) {
        minimap_point_t pts[MM_CLIP_MAX];
        const int n = minimap_vehicle_polygon(&fr, &m.vehicles[i], pts);
        if (n < 3) continue;
        const ImDrawListFlags flags = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        fill_polygon(dl, pts, n, fill);
        dl->Flags = flags;
        for (int k = 0; k < n; k++) dl->PathLineTo(ImVec2(pts[k].x, pts[k].y));
        dl->PathStroke(edge, ImDrawFlags_Closed, 1.0f);
    }
}

void minimap_creature_markers(ImDrawList* dl, const minimap_frame_t& fr, const cp_hud_t& h) {
    for (int i = 0; i < h.creature_count; i++) {
        float x, y;
        if (!minimap_creature_place(&fr, &h.creatures[i], &x, &y)) continue;
        const float k = minimap_creature_marker_radius(fr.radius, h.creatures[i].size);
        dl->AddCircleFilled(ImVec2(x, y), k + MM_CREATURE_OUTLINE_PX, colour(ImVec4(0.0f, 0.0f, 0.0f, 0.85f), 1.0f), 12);
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
    if (!minimap_place_facing(&pic, h.player_pos[0], h.player_pos[1], h.heading, MM_ZOOM, &fr)) return;
    const float radius = fr.radius, cx = fr.cx, cy = fr.cy;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
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

    minimap_vehicle_markers(dl, fr, *m);
    minimap_creature_markers(dl, fr, h);
    if (h.waypoint) minimap_waypoint_marker(dl, fr, h);

    float dx, dy;
    minimap_point_t t[4];
    minimap_heading_dir(0.0f, &dx, &dy);
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

// The HUD, then the game's text, then the counter, then the windows (the background list is under every window).
void draw_ui(const dmp_frame_t* f) {
    if (hud_shown(f)) {
        compass(f);
        minimap(f);
        text_layer(ImGui::GetBackgroundDrawList(), f);
    }
    frame_counter(ImGui::GetBackgroundDrawList(), f);
    if (f->menu) menu(f);
}

}  // namespace

#ifdef _WIN32
extern "C" void dmp_draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* target, int bb_w,
                         int bb_h, const dmp_frame_t* f) {
    ID3D11RenderTargetView* previous = nullptr;
    ID3D11DepthStencilView* previous_depth = nullptr;
    g_hits.valid = 0;
    if (!f->menu || f->objectives) g_stable.open = false;
    if (!device || !context || !target || bb_w <= 0 || bb_h <= 0 || f->view_w < 1.0f || f->view_h < 1.0f) return;
    if (!f->menu && !hud_shown(f) && !(f->counter && f->counter[0])) return;
    if (!start(device, context)) return;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(f->view_w, f->view_h);
    io.DisplayFramebufferScale = ImVec2((float)bb_w / f->view_w, (float)bb_h / f->view_h);
    io.DeltaTime = frame_seconds();
    apply_scale(f->dpi_scale > 0.0f ? f->dpi_scale : 1.0f);
    fonts();
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    draw_ui(f);
    ImGui::Render();
    context->OMGetRenderTargets(1, &previous, &previous_depth);
    context->OMSetRenderTargets(1, &target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    context->OMSetRenderTargets(1, &previous, previous_depth);
    if (previous) previous->Release();
    if (previous_depth) previous_depth->Release();
}
#endif

extern "C" void dmp_draw_software(uint32_t* pixels, int width, int height, int pitch_pixels, const dmp_frame_t* f) {
    g_hits.valid = 0;
    if (!f->menu || f->objectives) g_stable.open = false;
    if (!pixels || width <= 0 || height <= 0 || f->view_w < 1.0f || f->view_h < 1.0f) return;
    if (!f->menu && !hud_shown(f) && !(f->counter && f->counter[0])) return;
    if (!start_software()) return;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(f->view_w, f->view_h);
    io.DisplayFramebufferScale = ImVec2((float)width / f->view_w, (float)height / f->view_h);
    io.DeltaTime = frame_seconds();
    apply_scale(f->dpi_scale > 0.0f ? f->dpi_scale : 1.0f);
    fonts();
    ImGui::NewFrame();
    draw_ui(f);
    ImGui::Render();
    imgui_software_render(ImGui::GetDrawData(), pixels, width, height, pitch_pixels);
}

namespace {

struct GpuTextureSlot {
    const ImTextureData* tex;
    uint32_t revision;
};

GpuTextureSlot g_gpu_slots[8];
std::vector<ImDrawVert> g_gpu_vertices;
std::vector<ImDrawIdx> g_gpu_indices;
std::vector<dmp_gpu_cmd_t> g_gpu_cmds;
dmp_gpu_texture_t g_gpu_textures[9];
const uint8_t g_white_texel[4] = { 255, 255, 255, 255 };
static_assert(sizeof(ImDrawIdx) == 2, "the GPU overlay draws 16-bit indices");
static_assert(sizeof(ImDrawVert) == 20, "the GPU overlay reads ImDrawVert as 20 bytes");

uint32_t gpu_slot_of(const ImTextureData* tex) {
    for (uint32_t i = 0; i < 8; i++)
        if (g_gpu_slots[i].tex == tex) return i;
    for (uint32_t i = 0; i < 8; i++)
        if (!g_gpu_slots[i].tex) {
            g_gpu_slots[i].tex = tex;
            g_gpu_slots[i].revision = 1;
            return i;
        }
    return 8;
}

}  // namespace

extern "C" int dmp_build_gpu(dmp_gpu_draw_t* out, int width, int height, const dmp_frame_t* f) {
    g_hits.valid = 0;
    if (!f->menu || f->objectives) g_stable.open = false;
    if (!out || width <= 0 || height <= 0 || f->view_w < 1.0f || f->view_h < 1.0f) return 0;
    if (!f->menu && !hud_shown(f) && !(f->counter && f->counter[0])) return 0;
    if (!start_software()) return 0;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(f->view_w, f->view_h);
    io.DisplayFramebufferScale = ImVec2((float)width / f->view_w, (float)height / f->view_h);
    io.DeltaTime = frame_seconds();
    apply_scale(f->dpi_scale > 0.0f ? f->dpi_scale : 1.0f);
    fonts();
    ImGui::NewFrame();
    draw_ui(f);
    ImGui::Render();
    ImDrawData* data = ImGui::GetDrawData();
    if (!data || data->CmdListsCount == 0) return 0;
    if (data->Textures)
        for (ImTextureData* t : *data->Textures)
            if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates) {
                const uint32_t slot = gpu_slot_of(t);
                if (slot < 8) g_gpu_slots[slot].revision++;
            }
    imgui_software_update_textures(data);
    g_gpu_vertices.clear();
    g_gpu_indices.clear();
    g_gpu_cmds.clear();
    uint32_t texture_count = 0;
    uint32_t cmd_texture_of[9];
    for (uint32_t i = 0; i < 9; i++) cmd_texture_of[i] = 0xFFFFFFFFu;
    const ImVec2 origin = data->DisplayPos, scale = data->FramebufferScale;
    for (int n = 0; n < data->CmdListsCount; n++) {
        const ImDrawList* list = data->CmdLists[n];
        const uint32_t base_vertex = (uint32_t)g_gpu_vertices.size(), base_index = (uint32_t)g_gpu_indices.size();
        g_gpu_vertices.insert(g_gpu_vertices.end(), list->VtxBuffer.Data, list->VtxBuffer.Data + list->VtxBuffer.Size);
        g_gpu_indices.insert(g_gpu_indices.end(), list->IdxBuffer.Data, list->IdxBuffer.Data + list->IdxBuffer.Size);
        for (int c = 0; c < list->CmdBuffer.Size; c++) {
            const ImDrawCmd* cmd = &list->CmdBuffer[c];
            if (cmd->UserCallback || cmd->ElemCount == 0) continue;
            const ImTextureData* tex = (const ImTextureData*)(uintptr_t)cmd->GetTexID();
            uint32_t slot = 8;
            if (tex && tex->Pixels) slot = gpu_slot_of(tex);
            if (slot > 8) slot = 8;
            if (cmd_texture_of[slot] == 0xFFFFFFFFu) {
                dmp_gpu_texture_t& t = g_gpu_textures[texture_count];
                if (slot == 8) {
                    t.id = 100;
                    t.width = t.height = 1;
                    t.bytes_per_pixel = 4;
                    t.pixels = g_white_texel;
                    t.revision = 1;
                } else {
                    t.id = slot + 1;
                    t.width = (uint32_t)tex->Width;
                    t.height = (uint32_t)tex->Height;
                    t.bytes_per_pixel = (uint32_t)tex->BytesPerPixel;
                    t.pixels = tex->Pixels;
                    t.revision = tex == &g_sw_text_tex ? ++g_gpu_slots[slot].revision : g_gpu_slots[slot].revision;
                }
                cmd_texture_of[slot] = texture_count++;
            }
            dmp_gpu_cmd_t g;
            g.clip[0] = (cmd->ClipRect.x - origin.x) * scale.x;
            g.clip[1] = (cmd->ClipRect.y - origin.y) * scale.y;
            g.clip[2] = (cmd->ClipRect.z - origin.x) * scale.x;
            g.clip[3] = (cmd->ClipRect.w - origin.y) * scale.y;
            if (g.clip[2] <= g.clip[0] || g.clip[3] <= g.clip[1]) continue;
            g.idx_offset = base_index + cmd->IdxOffset;
            g.idx_count = cmd->ElemCount;
            g.vtx_offset = (int32_t)(base_vertex + cmd->VtxOffset);
            g.texture = cmd_texture_of[slot];
            g_gpu_cmds.push_back(g);
        }
    }
    if (g_gpu_cmds.empty() || g_gpu_vertices.empty() || g_gpu_indices.empty() || texture_count == 0) return 0;
    out->display_pos[0] = data->DisplayPos.x;
    out->display_pos[1] = data->DisplayPos.y;
    out->display_size[0] = data->DisplaySize.x;
    out->display_size[1] = data->DisplaySize.y;
    out->target_w = (uint32_t)width;
    out->target_h = (uint32_t)height;
    out->vertices = g_gpu_vertices.data();
    out->vertex_count = (uint32_t)g_gpu_vertices.size();
    out->indices = g_gpu_indices.data();
    out->index_count = (uint32_t)g_gpu_indices.size();
    out->cmds = g_gpu_cmds.data();
    out->cmd_count = (uint32_t)g_gpu_cmds.size();
    out->textures = g_gpu_textures;
    out->texture_count = texture_count;
    return 1;
}

extern "C" int dmp_draw_rgba(uint8_t* rgba, int width, int height, const dmp_frame_t* f, uint8_t* tiles, int tiles_w, int tiles_h) {
    g_hits.valid = 0;
    if (!f->menu || f->objectives) g_stable.open = false;
    if (!rgba || width <= 0 || height <= 0 || f->view_w < 1.0f || f->view_h < 1.0f) return 0;
    if (!f->menu && !hud_shown(f) && !(f->counter && f->counter[0])) return 0;
    if (!start_software()) return 0;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(f->view_w, f->view_h);
    io.DisplayFramebufferScale = ImVec2((float)width / f->view_w, (float)height / f->view_h);
    io.DeltaTime = frame_seconds();
    apply_scale(f->dpi_scale > 0.0f ? f->dpi_scale : 1.0f);
    fonts();
    ImGui::NewFrame();
    draw_ui(f);
    ImGui::Render();
    imgui_software_render_rgba(ImGui::GetDrawData(), rgba, width, height, tiles, tiles_w, tiles_h);
    g_sw_text_tex.Pixels = nullptr;
    return 1;
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

extern "C" void dmp_set_fallback_font_memory(const void* ttf, size_t size) {
    g_fallback_memory = ttf;
    g_fallback_memory_size = size;
}
