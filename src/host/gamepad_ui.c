#include "gamepad_ui.h"

#include "debug_menu.h"
#include "gamepad.h"
#include "guest_abi.h"
#include "host_input.h"
#include "overlay_policy.h"
#include "tutorial_prompts.h"

static gamepad_ui_mode_t mode_of(const guest_ui_view_t* v) {
    if (v->kind == GUEST_WND_LOADER || v->kind == GUEST_WND_VIDEO) return GUI_NONE;
    if (v->ui == GUEST_UI_GAMEPLAY) return GUI_GAMEPLAY;
    if (v->ui == GUEST_UI_MENU || v->ui == GUEST_UI_DIALOG) return GUI_MENU;
    return GUI_NONE;
}

static int shortcut(const jpt_gamepad_settings_t* cfg, int control, uint32_t pressed) {
    return (pressed & JPT_PAD_BIT(control)) && jpt_pad_control_free(cfg, control);
}

static const char* button_name(uint32_t buttons) {
    if (buttons & JPT_PAD_BIT(JPT_PAD_EAST)) return "B";
    if (buttons & JPT_PAD_BIT(JPT_PAD_START)) return "Menu";
    return "View";
}

void gamepad_ui_step(gamepad_ui_t* ui, const gamepad_ui_in_t* in) {
    guest_ui_view_t v;
    uint32_t held, pressed, released, before;
    const gamepad_ui_mode_t mode = guest_ui_probe(&v) ? mode_of(&v) : GUI_NONE;
    if (mode != ui->mode || v.top != ui->top) {
        pad_edges_reset(&ui->edges, in->buttons);
        if (mode == GUI_MENU) gamepad_menu_reset();
        ui->mode = mode;
        ui->top = v.top;
    }
    before = ui->edges.previous;
    held = pad_edges_update(&ui->edges, in->buttons, &pressed);
    released = before & ~held;
    if (debug_menu_visible() && mode != GUI_NONE) {
        if (mode == GUI_MENU) ui->left_gameplay = 1;
        if (debug_menu_open_menu() == 2 && shortcut(in->cfg, JPT_PAD_HINT_CONTROL, pressed)) overlay_request(OV_HINT, "D-pad Left");
        else debug_menu_pad(held, pressed);
    } else if (mode == GUI_GAMEPLAY && tutorial_prompts_card_shown()) {
        const uint32_t close = JPT_PAD_BIT(JPT_PAD_BACK) | JPT_PAD_BIT(JPT_PAD_EAST) | JPT_PAD_BIT(JPT_PAD_START);
        if (pressed & close) overlay_request(OV_NONE, button_name(pressed & close));
        else if (shortcut(in->cfg, JPT_PAD_HINT_CONTROL, pressed)) overlay_request(OV_HINT, "D-pad Left");
        else if (shortcut(in->cfg, JPT_PAD_OBJECTIVES_CONTROL, pressed)) overlay_request(OV_OBJECTIVES, "D-pad Right");
    } else if (mode == GUI_GAMEPLAY) {
        if (pressed & JPT_PAD_BIT(JPT_PAD_START)) host_post_key(HOST_VK_ESCAPE);
        if (pressed & JPT_PAD_BIT(JPT_PAD_BACK)) overlay_request(OV_CONTROLS, "View");
        else if (!ui->hint_shown) ui->hint_shown = tutorial_prompts_hint();
        if (pad_bound(in->cfg, JPT_ACT_JUMP, pressed)) host_post_key(HOST_VK_SPACE);
        if (shortcut(in->cfg, JPT_PAD_HINT_CONTROL, pressed)) overlay_request(OV_HINT, "D-pad Left");
        else if (shortcut(in->cfg, JPT_PAD_OBJECTIVES_CONTROL, pressed)) overlay_request(OV_OBJECTIVES, "D-pad Right");
        else if (shortcut(in->cfg, JPT_PAD_SHOW_OBJECTIVE_CONTROL, pressed)) debug_menu_show_objective();
        if (in->chord && in->toggle_minimap) in->toggle_minimap("L3 + R3");
    } else {
        ui->left_gameplay = 1;
        if (mode == GUI_MENU)
            gamepad_menu_frame(in->pad, held, pressed, released, in->device_gamepad, in->now_ms, in->dt > 0.1f ? 0.1f : in->dt,
                               in->stick_deadzone);
    }
}

void gamepad_ui_overlay_changed(gamepad_ui_t* ui, uint32_t buttons) { pad_edges_reset(&ui->edges, buttons); }

void gamepad_ui_video(gamepad_ui_t* ui, uint32_t buttons) {
    const uint32_t skip = JPT_PAD_BIT(JPT_PAD_SOUTH) | JPT_PAD_BIT(JPT_PAD_EAST) | JPT_PAD_BIT(JPT_PAD_START);
    uint32_t pressed;
    ui->left_gameplay = 1;
    if (ui->mode != GUI_VIDEO) {
        pad_edges_reset(&ui->edges, buttons);
        ui->mode = GUI_VIDEO;
        ui->top = 0;
    }
    pad_edges_update(&ui->edges, buttons, &pressed);
    if (pressed & skip) host_post_key_up(HOST_VK_ESCAPE);
}
